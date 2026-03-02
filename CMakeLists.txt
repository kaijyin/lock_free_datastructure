cmake_minimum_required(VERSION 3.14)

# 编译器必须在 project() 之前设置
set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)

project(LockFreeDataStructure CXX)

# 代码补全数据库
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 默认 Release 构建
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# C++20（用于 std::barrier）
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter")
    # Debug 模式：开启 ThreadSanitizer
    set(CMAKE_CXX_FLAGS_DEBUG   "-Og -g -fsanitize=thread -fno-omit-frame-pointer")
    # Release 模式：最大优化
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
endif()

# 头文件目录
include_directories(include)

# ---- GoogleTest ----
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# ---- 正确性测试 ----
add_executable(correctness_test tests/correctness_test.cpp)
target_link_libraries(correctness_test
    PRIVATE
    GTest::GTest
    GTest::Main
    pthread
)

# ---- 性能测试 ----
add_executable(performance_test tests/performance_test.cpp)
target_link_libraries(performance_test
    PRIVATE
    pthread
)
# 性能测试强制 Release 优化（即使 CMAKE_BUILD_TYPE=Debug 也保留高优化）
target_compile_options(performance_test PRIVATE
    $<$<CONFIG:Debug>:-O2>
)
