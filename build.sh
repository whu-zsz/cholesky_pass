#!/bin/bash
set -e

LLVM=~/llvm-project/build/bin
CLANG=$LLVM/clang
OPT=$LLVM/opt
PASS=./build/CholeleskyPass.so

echo "[1/3] 生成IR..."
$CLANG -O1 -S -emit-llvm test/block_cholesky.c -o test/block_cholesky_pass.ll

echo "[2/3] Pass处理..."
$OPT -load-pass-plugin=$PASS \
     -passes="cholesky-parallel" \
     test/block_cholesky_pass.ll \
     -S -o test/block_cholesky_opt.ll

echo "[3/3] 编译链接..."
$CLANG test/block_cholesky_opt.ll test/main.c runtime/runtime.c \
       -o test/block_cholesky_parallel \
       -lm -lpthread

echo "完成，运行: ./test/block_cholesky_parallel"
