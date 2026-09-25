#pragma once

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <spdlog/details/log_msg.h>
#include <spdlog/fmt/chrono.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace manuel {

enum class Level {
    debug = 10,
    info = 20,
    success = 25,
    warning = 30,
    error = 40,
    critical = 50,
};

struct LogStyle {
    bool use_color = true;
    bool show_time = true;
    bool show_level = true;
};

class ManuelLogger {
public:
    explicit ManuelLogger(std::string name = "app",
                          std::string level = {},
                          std::string log_file = {},
                          bool use_color = true,
                          bool auto_detect_color = true,
                          bool show_time = true,
                          bool show_level = true)
        : name_(std::move(name)),
          style_{auto_detect_color ? detect_color_support() : use_color, show_time, show_level},
          level_(normalize_level(level.empty() ? read_env_level() : level)) {
        initialize_console_sink();

        if (!log_file.empty()) {
            initialize_file_sink(log_file);
        }
    }

    void set_level(std::string level) {
        std::lock_guard<std::mutex> lock(mutex_);
        level_ = normalize_level(level);
    }

    [[nodiscard]] Level level() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return level_;
    }

    void debug(const std::string& message) { emit(Level::debug, message); }
    void info(const std::string& message) { emit(Level::info, message); }
    void success(const std::string& message) { emit(Level::success, message); }
    void warning(const std::string& message) { emit(Level::warning, message); }
    void error(const std::string& message) { emit(Level::error, message); }
    void critical(const std::string& message) { emit(Level::critical, message); }

    void banner(const std::string& title) {
        const auto width = std::max<std::size_t>(10, title.size() + 4);
        const std::string line(width, '=');
        emit(Level::info, line + "\n  " + title + "\n" + line);
    }

    void section(const std::string& title) {
        emit(Level::info, style_.use_color ? fmt::format("{}{}{}", ansi::bold, title, ansi::reset) : title);
    }

    template <typename T>
    void kv(const std::string& key, T&& value, Level level = Level::info) {
        emit(level, fmt::format("{:<22}: {}", key, std::forward<T>(value)));
    }

    void pair(const std::string& left,
              const std::string& right,
              std::string arrow = "->",
              Level level = Level::info) {
        emit(level, fmt::format("{} {} {}", left, arrow, right));
    }

    template <typename T>
    void missing(const std::string& what, T&& value) {
        warning(fmt::format("Missing {}: {}", what, quote_value(std::forward<T>(value))));
    }

    template <typename Exception>
    void exception(const std::string& message, const Exception& exception) {
        error(fmt::format("{}: {}", message, exception.what()));
    }

    void exception(const std::string& message, const std::exception_ptr& exception_ptr) {
        if (!exception_ptr) {
            error(message);
            return;
        }

        try {
            std::rethrow_exception(exception_ptr);
        } catch (const std::exception& exception) {
            error(fmt::format("{}: {}", message, exception.what()));
        } catch (...) {
            error(fmt::format("{}: unknown exception", message));
        }
    }

private:
    struct ansi {
        static constexpr const char* reset = "\033[0m";
        static constexpr const char* bold = "\033[1m";
        static constexpr const char* debug = "\033[38;5;244m";
        static constexpr const char* info = "\033[38;5;39m";
        static constexpr const char* success = "\033[38;5;42m";
        static constexpr const char* warning = "\033[38;5;214m";
        static constexpr const char* error = "\033[38;5;196m";
        static constexpr const char* critical = "\033[48;5;196m\033[38;5;231m";
        static constexpr const char* banner = "\033[38;5;141m";
    };

    static std::string read_env_level() {
        if (const char* raw_level = std::getenv("MANUEL_LOG_LEVEL")) {
            return raw_level;
        }
        return "info";
    }

    static bool detect_color_support() {
        if (std::getenv("NO_COLOR") != nullptr) {
            return false;
        }

#if defined(_WIN32)
        return ::_isatty(_fileno(stderr)) != 0;
#else
        return ::isatty(STDERR_FILENO) != 0;
#endif
    }

    static Level normalize_level(std::string_view level) {
        const auto lowered = lowercase(level);
        if (lowered == "debug") {
            return Level::debug;
        }
        if (lowered == "info") {
            return Level::info;
        }
        if (lowered == "success") {
            return Level::success;
        }
        if (lowered == "warning" || lowered == "warn") {
            return Level::warning;
        }
        if (lowered == "error") {
            return Level::error;
        }
        if (lowered == "critical") {
            return Level::critical;
        }

        throw std::invalid_argument(
            "Invalid log level '" + std::string(level) +
            "'. Valid levels: debug, info, success, warning, error, critical");
    }

    static std::string lowercase(std::string_view value) {
        std::string result(value);
        for (char& ch : result) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        return result;
    }

    static spdlog::level::level_enum to_spdlog_level(Level level) {
        switch (level) {
        case Level::debug:
            return spdlog::level::debug;
        case Level::info:
        case Level::success:
            return spdlog::level::info;
        case Level::warning:
            return spdlog::level::warn;
        case Level::error:
            return spdlog::level::err;
        case Level::critical:
            return spdlog::level::critical;
        }

        return spdlog::level::info;
    }

    static const char* level_label(Level level) {
        switch (level) {
        case Level::debug:
            return "DEBUG";
        case Level::info:
            return "INFO";
        case Level::success:
            return "SUCCESS";
        case Level::warning:
            return "WARNING";
        case Level::error:
            return "ERROR";
        case Level::critical:
            return "CRITICAL";
        }

        return "INFO";
    }

    static const char* level_color(Level level) {
        switch (level) {
        case Level::debug:
            return ansi::debug;
        case Level::info:
            return ansi::info;
        case Level::success:
            return ansi::success;
        case Level::warning:
            return ansi::warning;
        case Level::error:
            return ansi::error;
        case Level::critical:
            return ansi::critical;
        }

        return "";
    }

    static std::string quote_value(const std::string& value) {
        return "'" + value + "'";
    }

    static std::string quote_value(const char* value) {
        return value == nullptr ? "null" : quote_value(std::string(value));
    }

    template <typename T>
    static std::string quote_value(T&& value) {
        if constexpr (std::is_convertible_v<T, std::string_view>) {
            return quote_value(std::string(std::string_view(value)));
        } else {
            return fmt::format("{}", std::forward<T>(value));
        }
    }

    void initialize_console_sink() {
        console_sink_ = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        console_sink_->set_level(spdlog::level::trace);
        console_sink_->set_pattern("%v");
    }

    void initialize_file_sink(const std::string& log_file) {
        const std::filesystem::path path(log_file);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        file_sink_ = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        file_sink_->set_level(spdlog::level::trace);
        file_sink_->set_pattern("%v");
    }

    [[nodiscard]] bool should_log(Level level) const {
        return static_cast<int>(level) >= static_cast<int>(level_);
    }

    [[nodiscard]] std::string timestamp() const {
        // ThreadPoolCPP compatibility patch: fmt 12 removed fmt::localtime.
        const auto now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        if (::localtime_s(&local, &now) != 0) return {};
#else
        if (::localtime_r(&now, &local) == nullptr) return {};
#endif
        char buffer[32]{};
        if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local) == 0) return {};
        return buffer;
    }

    [[nodiscard]] std::string format_prefix(Level level, bool use_color) const {
        std::vector<std::string> parts;
        parts.reserve(3);

        if (style_.show_time) {
            const auto value = timestamp();
            parts.push_back(use_color ? colorize(value, ansi::debug) : value);
        }

        if (style_.show_level) {
            const auto label = fmt::format("{:<8}", level_label(level));
            parts.push_back(use_color ? colorize(label, level_color(level)) : label);
        }

        parts.push_back(use_color ? colorize(name_, ansi::banner) : name_);

        return fmt::format("{}", fmt::join(parts, " | "));
    }

    [[nodiscard]] std::string colorize(std::string_view text, const char* color) const {
        if (!style_.use_color || color == nullptr || *color == '\0') {
            return std::string(text);
        }

        return fmt::format("{}{}{}", color, text, ansi::reset);
    }

    void emit(Level level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!should_log(level)) {
            return;
        }

        sink_message(console_sink_, level, fmt::format("{} | {}", format_prefix(level, style_.use_color), message));

        if (file_sink_) {
            sink_message(file_sink_, level, fmt::format("{} | {}", format_prefix(level, false), message));
        }
    }

    void sink_message(const std::shared_ptr<spdlog::sinks::sink>& sink,
                      Level level,
                      const std::string& text) const {
        if (!sink) {
            return;
        }

        const spdlog::string_view_t payload(text.data(), text.size());
        const spdlog::details::log_msg log_message{name_, to_spdlog_level(level), payload};
        sink->log(log_message);
        sink->flush();
    }

    std::string name_;
    LogStyle style_;
    Level level_;
    std::shared_ptr<spdlog::sinks::sink> console_sink_;
    std::shared_ptr<spdlog::sinks::sink> file_sink_;
    mutable std::mutex mutex_;
};

}  // namespace manuel
