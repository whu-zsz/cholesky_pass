# CLAUDE.md — cholesky_pass 项目上下文

## 一句话背景
编译竞赛项目：用LLVM Pass把分块Cholesky串行程序改成并行，跑在鲲鹏920 ARM上。当前42/100分，加速比1.37倍，目标32倍。

## 目录结构
```
~/compiler_contest/
├── llvm-project/build/bin/   # clang, opt, llvm-config
├── contestant_sdk/            # 官方SDK（只读）
│   ├── src/baseline/block_cholesky.cpp  # 官方baseline（不能改）
│   ├── include/kernels.h               # cholesky/trsm/madd声明
│   ├── lib/libkernels.a                # 算子实现
│   ├── bin/verifier_local              # 验证器
│   └── workdir/test.bin               # 150组测试矩阵
└── cholesky_pass/             # 我们的代码（在这里工作）
    ├── CMakeLists.txt
    ├── manifest.json
    ├── lib/CholeleskyPass.cpp
    └── runtime/
        ├── runtime.c                   # 当前版本（pipeline，待测）
        └── runtime_batch_backup.c      # 稳定备份（42分版本）
```

## 当前状态
- **稳定版**：`runtime_batch_backup.c`，150/150 PASS，42分，1.37倍加速
- **正在调试**：pipeline版，`runtime.c`里POOL_SIZE刚从32MB改到128MB，**中断前还没跑完测试**

## 立刻要做：接上中断的测试

```bash
cd ~/compiler_contest/cholesky_pass/build && ninja

~/compiler_contest/llvm-project/build/bin/clang++ \
    /home/whu_zsz/compiler_contest/workdir/official_bc_opt.ll \
    /home/whu_zsz/compiler_contest/contestant_sdk/src/baseline/main.cpp \
    ./runtime/libcontestant_runtime.a \
    /home/whu_zsz/compiler_contest/contestant_sdk/lib/libkernels.a \
    -I/home/whu_zsz/compiler_contest/contestant_sdk/include \
    -o /home/whu_zsz/compiler_contest/workdir/official_pipeline3 \
    -lpthread -O2

cd ~/compiler_contest/contestant_sdk
time /home/whu_zsz/compiler_contest/workdir/official_pipeline3 \
    /home/whu_zsz/compiler_contest/workdir/small_test.bin \
    /home/whu_zsz/compiler_contest/workdir/small_output_pl3.bin
echo "exit: $?"
./bin/verifier_local \
    /home/whu_zsz/compiler_contest/workdir/small_test.bin \
    /home/whu_zsz/compiler_contest/workdir/small_output_pl3.bin
```

**结果判断：**
- exit 0 + PASS → 测全量150组 → 通过则打包提交
- 超时/崩溃 → 回退：`cp runtime/runtime_batch_backup.c runtime/runtime.c`

## Pass关键设计（不要改）
- 扫描所有函数（不硬编码函数名，C++会mangle block_cholesky）
- 所有alloca必须放函数入口块，不能在循环体里（否则栈溢出崩溃）
- 插桩顺序：入口`runtime_init(0)` → 调用点`runtime_submit` → 返回前`runtime_wait_all`+`runtime_destroy`
- Pass注册名：`contestant-pass`

## 算子读写关系
| 算子 | 读 | 写 |
|------|----|----|
| `cholesky(A,L,b,lda)` | arg0 | arg1 |
| `trsm(A,L,X,b,lda)` | arg0,arg1 | arg2 |
| `madd(A,B,C,b,lda)` | arg0,arg1 | arg2 |

## Pipeline版设计要点
- `runtime_init`时就启动worker线程（边提交边执行是关键）
- `submit`中dep_count==0立刻入队
- `atomic_int submit_done`：wait_all设1，worker据此退出
- tasks预分配MAX_TASKS个，不realloc（避免race condition）
- pool主线程独占无锁，需≥128MB

## 提交命令
```bash
cd ~/compiler_contest/cholesky_pass
zip -r ~/compiler_contest/submission.zip CMakeLists.txt manifest.json lib/ runtime/
```

## 评测关键数字
- 平台：鲲鹏920 AArch64，毕昇编译器3.2.0，约32核
- M_ideal=32，满分需32倍加速比，现在1.37倍
- 150/150 PASS是底线，不能破
