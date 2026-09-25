cmake_minimum_required(VERSION 3.24)
project(ThreadPoolCPP VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)
option(THREADPOOL_BUILD_TESTS "Build the correctness and stress suite" ${PROJECT_IS_TOP_LEVEL})
option(THREADPOOL_BUILD_EXAMPLES "Build examples and benchmark" ${PROJECT_IS_TOP_LEVEL})
option(THREADPOOL_FETCH_SPDLOG "Download pinned spdlog if no installed package exists" ON)
set(THREADPOOL_SANITIZER "" CACHE STRING "Sanitizer: address, thread, or undefined (GCC/Clang)")
set_property(CACHE THREADPOOL_SANITIZER PROPERTY STRINGS "" address thread undefined)

find_package(spdlog 1.15.3 CONFIG QUIET)
if(NOT spdlog_FOUND)
    if(NOT THREADPOOL_FETCH_SPDLOG)
        message(FATAL_ERROR "Install spdlog >= 1.15.3 or enable THREADPOOL_FETCH_SPDLOG")
    endif()
    include(FetchContent)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG 6fa36017cfd5731d617e1a934f0e5ea9c4445b13)
    set(SPDLOG_INSTALL ON CACHE BOOL "Install fetched spdlog" FORCE)
    FetchContent_MakeAvailable(spdlog)
endif()

get_target_property(_threadpool_spdlog_definitions spdlog::spdlog_header_only INTERFACE_COMPILE_DEFINITIONS)
if("${_threadpool_spdlog_definitions}" MATCHES "SPDLOG_USE_STD_FORMAT")
    message(FATAL_ERROR "ManuelLogger requires a fmt-based spdlog build; SPDLOG_USE_STD_FORMAT is unsupported")
endif()

# Keep logger headers out of the public C++ API.
add_library(threadpool src/thread_pool.cpp)
add_library(ThreadPoolCPP::threadpool ALIAS threadpool)
set_target_properties(threadpool PROPERTIES EXPORT_NAME threadpool WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_compile_features(threadpool PUBLIC cxx_std_20)
target_include_directories(threadpool PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(threadpool SYSTEM PRIVATE third_party/manuel_logger)
target_link_libraries(threadpool PUBLIC Threads::Threads PRIVATE spdlog::spdlog_header_only)
if(MSVC)
    target_compile_options(threadpool PRIVATE /W4 /permissive-)
else()
    target_compile_options(threadpool PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(THREADPOOL_SANITIZER)
    if(MSVC OR NOT THREADPOOL_SANITIZER MATCHES "^(address|thread|undefined)$")
        message(FATAL_ERROR "Unsupported sanitizer/compiler combination")
    endif()
    target_compile_options(threadpool PUBLIC -fsanitize=${THREADPOOL_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(threadpool PUBLIC -fsanitize=${THREADPOOL_SANITIZER})
endif()

if(THREADPOOL_BUILD_TESTS)
    enable_testing()
    add_executable(threadpool_tests tests/thread_pool_tests.cpp)
    target_link_libraries(threadpool_tests PRIVATE ThreadPoolCPP::threadpool)
    add_test(NAME threadpool.correctness COMMAND threadpool_tests)
    set_tests_properties(threadpool.correctness PROPERTIES TIMEOUT 90)
    add_executable(threadpool_stress tests/stress.cpp)
    target_link_libraries(threadpool_stress PRIVATE ThreadPoolCPP::threadpool)
    add_test(NAME threadpool.stress COMMAND threadpool_stress)
    set_tests_properties(threadpool.stress PROPERTIES TIMEOUT 120)
endif()
if(THREADPOOL_BUILD_EXAMPLES)
    foreach(example basic cancellation benchmark)
        add_executable(threadpool_${example} examples/${example}.cpp)
        target_link_libraries(threadpool_${example} PRIVATE ThreadPoolCPP::threadpool)
    endforeach()
endif()

find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    configure_file(docs/Doxyfile.in Doxyfile @ONLY)
    add_custom_target(docs COMMAND Doxygen::doxygen ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMENT "Build API and guide documentation")
endif()
install(TARGETS threadpool EXPORT ThreadPoolCPPTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ThreadPoolCPPTargets NAMESPACE ThreadPoolCPP:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThreadPoolCPP)
configure_package_config_file(cmake/ThreadPoolCPPConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolCPPConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThreadPoolCPP)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolCPPConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolCPPConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolCPPConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThreadPoolCPP)
install(FILES LICENSE third_party/NOTICE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/ThreadPoolCPP)
install(FILES third_party/manuel_logger/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/ThreadPoolCPP/manuel_logger)

install(FILES third_party/spdlog-LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/ThreadPoolCPP/spdlog RENAME LICENSE)
