cmake_minimum_required(VERSION 3.15)
project(LiteDB CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Core library: everything under src/ except main.cpp.
file(GLOB_RECURSE LITEDB_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(FILTER LITEDB_SOURCES EXCLUDE REGEX ".*src/main\\.cpp$")

add_library(litedb_core STATIC ${LITEDB_SOURCES})
target_include_directories(litedb_core PUBLIC src/)

# Server binary.
add_executable(litedb src/main.cpp)
target_link_libraries(litedb PRIVATE litedb_core)

# Tests.
enable_testing()
find_package(GTest REQUIRED)
include(GoogleTest)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "tests/*.cpp")
add_executable(litedb_tests ${TEST_SOURCES})
target_link_libraries(litedb_tests PRIVATE litedb_core GTest::GTest GTest::Main pthread)
gtest_discover_tests(litedb_tests)
