# Third-party notices

`manuel_logger/manuel_logger.hpp` is copied from
https://github.com/lutfia95/cpp_manual_logger at commit
`b0018ace1c6c020b6dfbdb7ab87c077ab1c025f3` (MIT; accompanying LICENSE).
The upstream CMake build references a missing configuration template, so this
project integrates the header directly. One compatibility patch replaces the
removed `fmt::localtime` helper with thread-safe platform local-time conversion
and `std::strftime`; timestamp formatting is preserved across fmt versions.

The default fetched spdlog is v1.15.3, commit
`6fa36017cfd5731d617e1a934f0e5ea9c4445b13`, from
https://github.com/gabime/spdlog (MIT, including bundled fmt notices in its LICENSE).
It is used header-only inside the compiled library. Redistributors must retain
its license as well as the ManuelLogger license. CMake installs that license.
