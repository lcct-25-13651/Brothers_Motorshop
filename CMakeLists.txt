cmake_minimum_required(VERSION 3.14)
project(re_parts_tracker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Business logic (no HTTP): reused by the app and by the self-test.
add_library(core STATIC src/util.cpp src/auth.cpp src/inventory.cpp src/reports.cpp)
target_include_directories(core PUBLIC src)
if(MSVC)
  target_compile_options(core PRIVATE /W4)
else()
  target_compile_options(core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/httplib.h")
  message(FATAL_ERROR
    "Missing third_party/httplib.h. Download the single header from\n"
    "  https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h\n"
    "and save it as third_party/httplib.h")
endif()

find_package(Threads REQUIRED)
add_executable(re_parts_tracker src/main.cpp src/web.cpp)
target_include_directories(re_parts_tracker PRIVATE third_party)
target_link_libraries(re_parts_tracker PRIVATE core Threads::Threads)
if(WIN32)
  target_link_libraries(re_parts_tracker PRIVATE ws2_32)
  target_compile_definitions(re_parts_tracker PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX)
endif()

enable_testing()
add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE core)
add_test(NAME smoke COMMAND smoke_test)

# Web-layer test against an in-process stand-in for httplib (no sockets).
add_executable(web_e2e tests/web_e2e.cpp src/web.cpp)
target_include_directories(web_e2e PRIVATE tests/stub)
target_link_libraries(web_e2e PRIVATE core)
add_test(NAME web_e2e COMMAND web_e2e)
