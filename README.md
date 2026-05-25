# cholesky_pass

基于 LLVM Pass 的分块 Cholesky 分解并行调度系统。

面向 2026 年全国大学生计算机系统能力大赛编译系统设计赛（华为毕昇杯）"动态算子图编译与并行调度"赛题。

---

## 项目概述

本项目通过在 LLVM 中添加 Pass 的方式，自动分析分块 Cholesky 分解算法中 `cholesky`、`trsm`、`madd` 三个算子之间的数据依赖关系，将原始串行程序改造为基于任务图的并行调度程序，在鲲鹏 920 ARM 平台上最大化计算性能。

### 核心思路

```
原始串行程序
    cholesky(...)   ← 直接调用，顺序执行
    trsm(...)
    madd(...)

经过 Pass 处理后
    runtime_submit(cholesky_wrapper, ...)  ← 提交给运行时
    runtime_submit(trsm_wrapper, ...)
    runtime_submit(madd_wrapper, ...)
    runtime_wait_all()                     ← 等待并行执行完成
```

运行时库在收到任务后，自动分析任务间的内存读写依赖，构建任务 DAG，用线程池并行调度执行无依赖冲突的任务。

---

## 项目结构

```
cholesky_pass/
├── CMakeLists.txt          # 构建配置
├── build.sh                # 一键编译脚本
├── lib/
│   └── CholeleskyPass.cpp  # LLVM Pass 核心实现
├── runtime/
│   ├── runtime.h           # 运行时库接口
│   └── runtime.c           # 并行运行时库实现（线程池 + 任务图调度）
└── test/
    ├── block_cholesky.c    # 分块 Cholesky 分解算法源码
    └── main.c              # 测试驱动
```

---

## 依赖环境

### LLVM 17（openEuler 分支）

```bash
git clone https://gitee.com/openeuler/llvm-project.git \
    --branch dev_17.0.6 --depth=1

cd llvm-project && mkdir build && cd build

cmake ../llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  -DBUILD_SHARED_LIBS=ON \
  -GNinja

ninja -j$(nproc)
```

### 毕昇编译器（鲲鹏 920 平台）

```bash
wget https://mirrors.huaweicloud.com/kunpeng/archive/compiler/bisheng_compiler/BiShengCompiler-3.2.0.1-aarch64-linux.tar.gz
tar xzf BiShengCompiler-3.2.0.1-aarch64-linux.tar.gz
```

---

## 构建

```bash
# 编译 Pass
mkdir build && cd build
cmake .. -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_BUILD_DIR=/path/to/llvm-project/build
ninja
cd ..
```

---

## 使用方法

### 一键编译（推荐）

```bash
./build.sh
./test/block_cholesky_parallel
```

### 手动三步编译

```bash
# Step 1：生成 LLVM IR
~/llvm-project/build/bin/clang \
    -O1 -S -emit-llvm \
    test/block_cholesky.c \
    -o test/block_cholesky_pass.ll

# Step 2：Pass 处理，插入运行时调用
~/llvm-project/build/bin/opt \
    -load-pass-plugin=./build/CholeleskyPass.so \
    -passes="cholesky-parallel" \
    test/block_cholesky_pass.ll \
    -S -o test/block_cholesky_opt.ll

# Step 3：编译链接
~/llvm-project/build/bin/clang \
    test/block_cholesky_opt.ll \
    test/main.c \
    runtime/runtime.c \
    -o test/block_cholesky_parallel \
    -lm -lpthread
```

---

## 技术方案

### LLVM Pass（`lib/CholeleskyPass.cpp`）

Pass 在 `block_cholesky` 函数中执行以下操作：

1. **识别算子调用**：遍历 IR 指令，找到所有 `cholesky`、`trsm`、`madd` 的 `CallInst`
2. **提取读写指针**：
   - `cholesky(A, L, ...)` → 读 `arg0`，写 `arg1`
   - `trsm(A, Bt, X, ...)` → 读 `arg0`、`arg1`，写 `arg2`
   - `madd(A, Bt, C, ...)` → 读 `arg0`、`arg1`，写 `arg2`
3. **插入运行时调用**：将每个算子调用替换为 `runtime_submit(...)`，在函数入口插入 `runtime_init`，在返回前插入 `runtime_wait_all`

### 运行时库（`runtime/runtime.c`）

运行时库在程序执行过程中动态构建任务 DAG 并并行调度：

**依赖分析**（三种依赖全部处理）：
- **RAW**（读后写）：任务 B 读的地址被任务 A 写过 → B 依赖 A
- **WAW**（写后写）：任务 B 写的地址被任务 A 写过 → B 依赖 A
- **WAR**（写后读）：任务 B 写的地址被任务 A 读过 → B 依赖 A

**调度流程**：

```
runtime_submit() 阶段：
  收集任务，分析依赖，建立后继关系

runtime_wait_all() 阶段：
  dep_count == 0 的任务入队
  → 启动线程池
  → 工作线程取出任务执行
  → 执行完后将后继任务 dep_count--
  → dep_count 降为 0 则入队
  → 所有任务完成后退出
```

---

## 验证结果

在 x86 Linux 开发环境下，串行与并行结果完全一致：

| 规模 | 串行 L[0][0] | 并行 L[0][0] | 结果 |
|------|-------------|-------------|------|
| n=4, b=2 | 2.1190 | 2.1190 | ✓ |
| n=8, b=2 | 2.9138 | 2.9138 | ✓ |
| n=8, b=4 | 2.9138 | 2.9138 | ✓ |
| n=16, b=4 | 4.0608 | 4.0608 | ✓ |
| n=32, b=4 | 5.7000 | 5.7000 | ✓ |
| n=32, b=8 | 5.7000 | 5.7000 | ✓ |

---

## 待完成

- [ ] 在鲲鹏 920 平台上验证功能正确性
- [ ] 性能测试与调优（线程数、调度策略）
- [ ] 集成到毕昇编译器（`-fpass-plugin` 方式）
- [ ] 通过赛题 200 组测试矩阵

---

## 参考资料

- [LLVM New Pass Manager 官方指南](https://llvm.org/docs/WritingAnLLVMNewPMPass.html)
- [LLVM IR 语言参考](https://llvm.org/docs/LangRef.html)
- [毕昇编译器用户指南](https://mirrors.huaweicloud.com/kunpeng/archive/compiler/bisheng_compiler/)
- 赛题技术方案文档
