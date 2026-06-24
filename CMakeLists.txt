cmake_minimum_required(VERSION 3.16)
project(Compiler2026ContestantPass LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 指向LLVM构建目录
set(LLVM_BUILD_DIR "/home/whu_zsz/compiler_contest/llvm-project/build"
    CACHE PATH "LLVM build directory")

find_package(LLVM REQUIRED CONFIG
    PATHS ${LLVM_BUILD_DIR}
    NO_DEFAULT_PATH)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

include(AddLLVM)
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(/home/whu_zsz/compiler_contest/llvm-project/build/include)
add_definitions(${LLVM_DEFINITIONS})

# Pass动态库，输出到 build/pass/libcontestant_pass.so
add_llvm_pass_plugin(contestant_pass
    lib/CholeleskyPass.cpp
)
set_target_properties(contestant_pass PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pass"
)

# 运行时静态库，输出到 build/runtime/libcontestant_runtime.a
add_library(contestant_runtime STATIC
    runtime/runtime.c
)
target_include_directories(contestant_runtime PUBLIC runtime/)
target_compile_options(contestant_runtime PRIVATE -O2 -std=c11)
target_link_libraries(contestant_runtime pthread)
set_target_properties(contestant_runtime PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime"
    OUTPUT_NAME "contestant_runtime"
)

# 复制manifest.json到build目录
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
    ${CMAKE_BINARY_DIR}/manifest.json
    COPYONLY
)
