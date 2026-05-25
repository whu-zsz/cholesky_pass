cmake_minimum_required(VERSION 3.16)
project(CholeleskyPass CXX C)

set(CMAKE_CXX_STANDARD 17)

# 指向你刚编译的 LLVM（改为你的实际路径；这里必须写绝对路径，不能写 ~）
find_package(LLVM REQUIRED CONFIG
    PATHS /home/whu_zsz/llvm-project/build
    NO_DEFAULT_PATH)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

include(AddLLVM)
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Pass 编译为动态库（毕昇编译器通过 -fpass-plugin 加载）
add_llvm_pass_plugin(CholeleskyPass
    lib/CholeleskyPass.cpp
)

# 运行时库编译为静态库
add_library(cholesky_runtime STATIC runtime/runtime.c)
target_include_directories(cholesky_runtime PUBLIC runtime/)
target_compile_options(cholesky_runtime PRIVATE -O2 -pthread)
target_link_libraries(cholesky_runtime pthread)