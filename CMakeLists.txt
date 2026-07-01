cmake_minimum_required(VERSION 3.16)
project(Compiler2026Contestant LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 查找 LLVM ──────────────────────────────────────
find_program(LLVM_CONFIG
    NAMES llvm-config llvm-config-15 llvm-config-17 llvm-config-18 llvm-config-19 llvm-config-20
    REQUIRED)

function(run_llvm_config out_var)
    execute_process(
        COMMAND "${LLVM_CONFIG}" ${ARGN}
        RESULT_VARIABLE cmd_status
        OUTPUT_VARIABLE cmd_output
        ERROR_VARIABLE cmd_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT cmd_status EQUAL 0)
        message(FATAL_ERROR "llvm-config ${ARGN} failed: ${cmd_error}")
    endif()
    set(${out_var} "${cmd_output}" PARENT_SCOPE)
endfunction()

run_llvm_config(LLVM_INCLUDE_DIR  --includedir)
run_llvm_config(LLVM_OBJ_ROOT     --obj-root)
run_llvm_config(LLVM_CXXFLAGS     --cxxflags)
run_llvm_config(LLVM_LDFLAGS      --ldflags)
run_llvm_config(LLVM_SHARED_MODE  --shared-mode)
run_llvm_config(LLVM_LIBS         --libs core passes support)
run_llvm_config(LLVM_SYSTEM_LIBS  --system-libs)

separate_arguments(LLVM_CXXFLAGS_LIST  UNIX_COMMAND "${LLVM_CXXFLAGS}")
separate_arguments(LLVM_LDFLAGS_LIST   UNIX_COMMAND "${LLVM_LDFLAGS}")
separate_arguments(LLVM_LIBS_LIST      UNIX_COMMAND "${LLVM_LIBS}")
separate_arguments(LLVM_SYSTEM_LIBS_LIST UNIX_COMMAND "${LLVM_SYSTEM_LIBS}")

list(FILTER LLVM_CXXFLAGS_LIST EXCLUDE REGEX "^-std=")
list(FILTER LLVM_CXXFLAGS_LIST EXCLUDE REGEX "^-I")

# ── Pass 动态库 ────────────────────────────────────
add_library(contestant_pass MODULE lib/CholeleskyPass.cpp)
target_compile_features(contestant_pass PRIVATE cxx_std_17)

# includedir（源码头文件）+ obj-root/include（生成头文件，如 abi-breaking.h）
target_include_directories(contestant_pass SYSTEM PRIVATE
    "${LLVM_INCLUDE_DIR}"
    "${LLVM_OBJ_ROOT}/include")

target_compile_options(contestant_pass PRIVATE ${LLVM_CXXFLAGS_LIST})
target_link_options(contestant_pass PRIVATE ${LLVM_LDFLAGS_LIST})

if(NOT LLVM_SHARED_MODE STREQUAL "static")
    target_link_libraries(contestant_pass PRIVATE
        ${LLVM_LIBS_LIST} ${LLVM_SYSTEM_LIBS_LIST})
endif()

set_target_properties(contestant_pass PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pass")

# ── 运行时静态库 ───────────────────────────────────
add_library(contestant_runtime STATIC runtime/runtime.c)
target_include_directories(contestant_runtime PUBLIC runtime/)
target_compile_options(contestant_runtime PRIVATE -O2)
target_link_libraries(contestant_runtime pthread)
set_target_properties(contestant_runtime PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime"
    OUTPUT_NAME "contestant_runtime")

# ── 复制 manifest ──────────────────────────────────
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
    ${CMAKE_BINARY_DIR}/manifest.json
    COPYONLY)
