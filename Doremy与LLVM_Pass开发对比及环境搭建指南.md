# Doremy 编译器与 LLVM Pass 开发：对比分析与环境搭建指南

> 本文档面向已完成 Doremy（SysY/ToyC）编译器开发的同学，
> 帮助你快速将已有的编译器知识迁移到"动态算子图编译与并行调度"赛题中。

---

## 一、整体定位对比

| 维度 | Doremy 编译器 | 本赛题（LLVM Pass） |
|------|--------------|-------------------|
| **你负责的部分** | 前端 + 中端 + 后端，全栈 | 只负责中端 Pass + 运行时库 |
| **前端** | 自研（flex/bison + AST） | 毕昇 clang（不用动） |
| **IR** | 自研 MidIR（SSA） | LLVM IR（标准 SSA） |
| **Pass 框架** | 自研 PassManager | LLVM New Pass Manager |
| **后端** | 自研 RISC-V 代码生成 | 毕昇后端（不用动） |
| **目标平台** | RV32 Linux | 鲲鹏 920 AArch64 |
| **核心产出** | 完整编译器可执行文件 | `CholeleskyPass.so` + `libruntime.a` |

**结论**：你不需要写前端和后端，工作量比 Doremy 小，但中端的分析深度要求更高。

---

## 二、核心概念对比：思维完全相同，API 不同

### 2.1 Pass 架构

你在 Doremy 里已经实现了一套 PassManager，本赛题的 LLVM Pass 框架和它**结构完全同构**。

**Doremy 的 Pass 管线**：
```cpp
// 你在 Doremy 里的组织方式（示意）
PassManager pm;
pm.addPass(std::make_unique<VerifySSAPass>());
pm.addPass(std::make_unique<InlinePass>());
pm.addPass(std::make_unique<GVNPass>());
pm.addPass(std::make_unique<DSEPass>());
pm.run(module);
```

**LLVM 的 Pass 管线**：
```cpp
// LLVM New Pass Manager，结构完全一致
ModulePassManager MPM;
MPM.addPass(VerifierPass());
MPM.addPass(AlwaysInlinerPass());
MPM.addPass(GVNPass());
MPM.addPass(CholeleskyPass());   // ← 你要写的
MPM.run(M, MAM);
```

**Pass 类定义对比**：

```cpp
// Doremy 风格（示意）
class DSEPass : public Pass {
public:
    void run(midir::Module &M) override {
        for (auto &F : M.functions) {
            // 分析逻辑
        }
    }
};

// LLVM 风格
struct CholeleskyPass : PassInfoMixin<CholeleskyPass> {
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
        for (auto &F : M) {
            // 分析逻辑，结构完全一样
        }
        return PreservedAnalyses::none();
    }
};
```

唯一的区别：LLVM 的 `run()` 返回 `PreservedAnalyses`，用于告知框架本次 Pass 是否修改了 IR（类似你的 Pass 是否改变了 module 的标志位）。

---

### 2.2 IR 遍历

这是你每天都在做的操作，换个 API 名字而已。

| 操作 | Doremy 风格（示意） | LLVM 风格 |
|------|-------------------|-----------|
| 遍历函数 | `for (auto &f : module.functions)` | `for (auto &F : M)` |
| 遍历基本块 | `for (auto &bb : f.basicBlocks)` | `for (auto &BB : F)` |
| 遍历指令 | `for (auto &inst : bb.instructions)` | `for (auto &I : BB)` |
| 类型判断 | `if (inst->kind == CALL)` | `if (auto *CI = dyn_cast<CallInst>(&I))` |
| 获取函数名 | `callInst->callee->name` | `CI->getCalledFunction()->getName()` |
| 获取操作数 | `inst->operands[0]` | `I.getOperand(0)` |

`dyn_cast<T>` 是 LLVM 的核心模式，等价于你自己写的带类型检查的向下转型。如果转型失败返回 `nullptr`，成功则返回目标类型指针，非常常用。

---

### 2.3 依赖分析

你在 Doremy 里实现了 `DSEPass`（死存储消除）和 `DeadLoadElimPass`（无效加载消除）。这两个 Pass 的核心逻辑就是**分析内存读写依赖**——判断某个 Store 写入的内存位置之后有没有 Load 去读它。

本赛题的依赖分析问题：

```
// 分块 Cholesky 中的调用序列
cholesky(&A[i*n+i], &L[i*n+i], ...);   // 写 L[i][i..n]
trsm(&A[j*n+i], &L[i*n+i], &L[j*n+i], ...);  // 读 L[i][i..n]，写 L[j][i]
madd(&L[j*n+i], &L[k*n+i], &L[j*n+k], ...);  // 读 L[j][i]、L[k][i]，写 L[j][k]
```

你在 DSEPass 里做的事：判断 `store addr` 和 `load addr` 之间是否有别名（alias）。
本赛题要做的事：判断 `trsm(写L[j][i])` 和 `madd(读L[j][i])` 之间是否有数据依赖。

**逻辑完全相同**，粒度从单条 Load/Store 指令提升到了函数调用级别，反而更简单，因为算子的读写范围可以通过参数直接推断。

---

### 2.4 图结构

你实现了 `SimplifyCFGPass`，处理过控制流图（CFG）——有向图，节点是基本块，边是跳转关系，拓扑结构决定执行顺序。

本赛题要构建**任务依赖图（Task DAG）**——有向无环图，节点是算子调用，边是数据依赖，拓扑顺序决定调度顺序。

```
CFG（你已经做过）:           Task DAG（本赛题）:

BB0 ──→ BB1                 cholesky(0,0)
 │       │                       │
 └──→ BB2 ──→ BB3           trsm(1,0)  trsm(2,0)
                                  │         │
                             madd(1,1)  madd(2,1)
```

数据结构和遍历算法（BFS/拓扑排序）完全复用。

---

### 2.5 代码插桩

你在 Doremy 里做 `LICMPass`（循环不变代码外提）时，需要：
1. 识别某条指令可以外提
2. 在循环前插入新指令
3. 删除循环内的旧指令

本赛题要做的插桩：
1. 识别串行算子调用（`cholesky`/`trsm`/`madd`）
2. 在其前后插入运行时调度调用（`runtime_submit_task`）
3. 替换原有的直接调用

**IRBuilder 对比**：

```cpp
// Doremy 里（示意）：在 insertPoint 前插入一条 Move 指令
auto *newInst = new MoveInst(dst, src);
bb->instructions.insert(insertPoint, newInst);

// LLVM 里：在 CI 前插入一个函数调用
IRBuilder<> Builder(CI);   // 设置插入点在 CI 之前
Value *taskId = Builder.getInt32(taskCounter++);
Builder.CreateCall(submitTaskFn, {taskId, funcPtr, argsPtr});
CI->eraseFromParent();     // 删除原来的直接调用
```

---

## 三、全新的部分：并行运行时库

这是 Doremy 里没有的东西，需要从零设计。但概念不难：

```
编译时（Pass 负责）：
  原始串行调用  →  替换为  runtime_submit(task_id, deps, func, args)

运行时（你的库负责）：
  维护任务队列 → 依赖满足时入队 → 线程池并发执行
```

**核心数据结构**：

```c
// runtime/runtime.h
typedef struct Task {
    int      task_id;
    void   (*func)(void *);    // 算子函数指针
    void    *args;             // 参数打包
    int      dep_count;        // 剩余未满足依赖数
    int     *successors;       // 后继任务 id 列表
    int      successor_count;
} Task;

// 提交任务（Pass 插入的调用）
void runtime_submit(int id, int *dep_ids, int dep_count,
                    void (*func)(void *), void *args);

// 等待所有任务完成
void runtime_wait_all();
```

**调度器核心逻辑**（和你的 PassManager 驱动 Pass 链很像）：

```c
// 初始化：dep_count == 0 的任务直接入队
// 工作线程：取出任务 → 执行 → 将后继 dep_count-- → 为 0 则入队
// 主线程：提交所有任务后调用 runtime_wait_all() 阻塞
```

---

## 四、环境搭建步骤

### 4.1 整体架构

```
Windows 11 主机（宿主环境）
└── WSL2 Ubuntu 22.04（实际开发环境，下文命令默认都在这里执行）
    ├── ~/llvm-project/           ← 赛题指定 LLVM 17 源码及编译产物
    │   └── build/
    │       ├── bin/clang         ← 生成 .ll 文件用于调试 Pass
    │       └── bin/opt           ← 单独测试 Pass 的工具
    │
    └── ~/cholesky_pass/          ← 你的新工程（类比 Doremy 仓库）
        ├── include/
        ├── lib/                  ← LLVM Pass 源码
        ├── runtime/              ← 并行运行时库
        └── build/
            ├── CholeleskyPass.so
            └── libcholesky_runtime.a

鲲鹏 920 ARM 服务器（赛题评测机，用于最终验证）
├── BiShengCompiler/              ← 毕昇编译器二进制
├── CholeleskyPass.so             ← 可从 WSL 编译后上传，或直接在 ARM 上本地编译
└── libcholesky_runtime.a
```

> **约定**：
> 1. 下文所有 `bash` 命令默认在 **WSL Ubuntu shell** 中执行，不是在 Windows PowerShell / CMD 中执行。
> 2. 建议把工程放在 `~/` 下，不要放在 `/mnt/c/...` 里，否则 LLVM / CMake / Ninja 的 I/O 会明显变慢。
> 3. 你可以用 VS Code Remote - WSL 打开 `~/compiler_contest` 之类的目录，或者从 Windows 资源管理器通过 `\\wsl.localhost\Ubuntu-22.04\home\whu_zsz\` 访问文件。

---

### 4.2 Step 0：进入 WSL 并安装依赖

```bash
# 在 Windows Terminal / PowerShell 中进入 Ubuntu 22.04
wsl -d Ubuntu-22.04

# 下面开始是在 WSL 里执行
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  git \
  python3
```

> **提示**：如果 WSL 默认分配的内存偏小，LLVM 编译时可能会很慢或被系统杀掉；这种情况下可以在 Windows 用户目录配置 `.wslconfig` 后执行 `wsl --shutdown` 再重启 WSL。

---

### 4.3 Step 1：在 WSL 本地编译 LLVM 17

```bash
# 1. 在 WSL 的 home 目录下克隆赛题指定仓库
git clone https://gitee.com/openeuler/llvm-project.git \
    --branch dev_17.0.6 \
    --depth=1 \
    ~/llvm-project

cd ~/llvm-project
mkdir -p build
cd build

# 2. CMake 配置
#    - Release 模式：编译快，体积小
#    - 只编译 clang：不需要其他 LLVM 子项目
#    - 同时支持 X86（WSL 本地调试）和 AArch64（鲲鹏目标）
#    - BUILD_SHARED_LIBS=ON：动态库，减少链接时间
cmake ../llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  -DLLVM_BUILD_EXAMPLES=ON \
  -DBUILD_SHARED_LIBS=ON \
  -GNinja

# 3. 编译（Release 模式约 30~60 分钟）
#    内存不足 8GB 的机器请用 -j4，而不是 -j$(nproc)
ninja -j$(nproc)

# 4. 验证
./bin/clang --version   # 期望输出：clang version 17.x.x
./bin/opt --version
```

> **注意**：在 WSL 中编译 LLVM，建议至少预留 8GB 内存和 30GB 磁盘空间。

---

### 4.4 Step 2：建立你的 Pass 工程

参考 Doremy 的目录组织方式，在 WSL 的 home 目录下新建独立工程（Out-of-tree，不修改 LLVM 主仓）：

```bash
cd ~
mkdir -p cholesky_pass/{include,lib,runtime,test,build}
cd ~/cholesky_pass
```

**`CMakeLists.txt`**（根目录）：

```cmake
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
```

```bash
# 配置和编译
cd ~/cholesky_pass/build
cmake .. -GNinja \
  -DCMAKE_BUILD_TYPE=Release
ninja
```

---

### 4.5 Step 3：写 Pass 骨架并验证

**`lib/CholeleskyPass.cpp`**（最小可运行版本）：

```cpp
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

struct CholeleskyPass : PassInfoMixin<CholeleskyPass> {

    PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {

        // 类比 Doremy：for (auto &func : module.functions)
        for (auto &F : M) {
            if (F.getName() != "block_cholesky") continue;

            // 类比 Doremy：for (auto &bb : func.basicBlocks)
            for (auto &BB : F) {

                // 类比 Doremy：for (auto &inst : bb.instructions)
                for (auto &I : BB) {

                    // 类比 Doremy：if (inst->kind == CALL)
                    if (auto *CI = dyn_cast<CallInst>(&I)) {
                        auto *callee = CI->getCalledFunction();
                        if (!callee) continue;

                        StringRef name = callee->getName();
                        if (name == "cholesky" ||
                            name == "trsm"     ||
                            name == "madd") {
                            errs() << "[Found operator] " << name << "\n";
                            // 后续步骤：
                            // 1. 记录此调用点及其参数（读写哪些内存块）
                            // 2. 构建任务依赖图 DAG
                            // 3. 将调用替换为 runtime_submit(...)
                        }
                    }
                }
            }
        }
        return PreservedAnalyses::none();
    }
};

// Pass 注册（New PM 必须有，类比 Doremy 的 PassManager::registerPass）
extern "C" LLVM_ATTRIBUTE_WEAK
PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION, "CholeleskyPass", "v0.1",
        [](PassBuilder &PB) {
            // 手动触发：clang -fpass-plugin=... -mllvm -passes=cholesky-parallel
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "cholesky-parallel") {
                        MPM.addPass(CholeleskyPass());
                        return true;
                    }
                    return false;
                });
            // 自动挂载：编译时自动在优化管线末尾执行
            PB.registerOptimizerLastEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                    MPM.addPass(CholeleskyPass());
                });
        }
    };
}
```

**验证流程**：

```bash
cd ~/cholesky_pass

# Step 1：把 C 源码编译成 LLVM IR（可读文本格式）
/home/whu_zsz/llvm-project/build/bin/clang \
    -O0 -S -emit-llvm \
    test/block_cholesky.c \
    -o test/block_cholesky.ll

# 用 VS Code Remote - WSL 或 vim/nvim 打开 .ll 文件，找 "call" 指令，
# 确认 cholesky/trsm/madd 的函数调用长什么样

# Step 2：用 opt 单独运行你的 Pass（类比 ./doremy -O1 input.sy）
/home/whu_zsz/llvm-project/build/bin/opt \
    -load-pass-plugin ./build/CholeleskyPass.so \
    -passes="cholesky-parallel" \
    test/block_cholesky.ll \
    -o /dev/null

# 期望输出：
# [Found operator] cholesky
# [Found operator] trsm
# [Found operator] madd
# ... （多次，因为有循环展开）
```

> 如果你嫌绝对路径太长，可以在 `~/.bashrc` 里自己加：
>
> ```bash
> export LLVM_BUILD=$HOME/llvm-project/build
> export PATH=$LLVM_BUILD/bin:$PATH
> ```
>
> 这样后面就可以直接写 `clang` / `opt`。不过第一次搭环境时，建议先写全路径，排查问题更直接。

---

### 4.6 Step 4：获取毕昇编译器（鲲鹏服务器上操作）

```bash
# 下面这些命令是在鲲鹏 920 ARM 服务器上执行，不是在 WSL 里执行
wget https://mirrors.huaweicloud.com/kunpeng/archive/compiler/bisheng_compiler/BiShengCompiler-3.2.0.1-aarch64-linux.tar.gz

tar xzf BiShengCompiler-3.2.0.1-aarch64-linux.tar.gz
export PATH=$PWD/BiShengCompiler-3.2.0.1-aarch64-linux/bin:$PATH

# 验证
clang --version   # 应显示 BiShengCompiler / clang 17.x
```

**用毕昇编译器 + 你的 Pass 完整编译**：

```bash
clang \
    -fpass-plugin=./build/CholeleskyPass.so \
    -O2 \
    test/block_cholesky.c \
    -o block_cholesky_parallel \
    -L./build -lcholesky_runtime \
    -lpthread

./block_cholesky_parallel   # 功能验证
```

> 实际操作里，一般是在 WSL 中开发和调试 Pass，然后把 `CholeleskyPass.so` 和 `libcholesky_runtime.a` 传到鲲鹏服务器做最终验证。

---

### 4.7 日常开发工作流

```
在 Windows Terminal / VS Code Remote - WSL 中修改 Pass 代码
    ↓
在 WSL 里执行 ninja（几秒重编，只编译改动的文件）
    ↓
在 WSL 里执行 opt -load-pass-plugin ... block_cholesky.ll
（先在 x86 本地快速验证 Pass 逻辑，不需要 ARM 环境）
    ↓
确认逻辑正确后，再同步到鲲鹏服务器
    ↓
在鲲鹏服务器上编译 .so 并运行完整测试
    ↓
提交评测系统
```

---

## 五、学习资源

| 资源 | 用途 | 链接 |
|------|------|------|
| LLVM New PM Pass 官方指南 | Pass 写法权威文档 | https://llvm.org/docs/WritingAnLLVMNewPMPass.html |
| LLVM IR 语言参考 | 理解 .ll 文件的每条指令 | https://llvm.org/docs/LangRef.html |
| LLVM Programmer's Manual | dyn_cast / IRBuilder 等核心 API | https://llvm.org/docs/ProgrammersManual.html |
| 毕昇编译器用户指南 | 如何用毕昇 clang 加载 Pass | 见赛题附录四 |
| StarPU 论文 | 任务图运行时调度设计参考 | https://hal.inria.fr/inria-00467677 |
| PLASMA 项目 | 分块 Cholesky 并行的学术标准实现 | https://icl.utk.edu/plasma/ |

---

## 六、总结

**你不需要从零开始**。Doremy 编译器给了你：

- Pass 架构的完整理解 → 直接映射到 LLVM Pass Manager
- 数据流分析经验（DSE/DeadLoad） → 直接用于算子依赖分析
- 图结构处理经验（SimplifyCFG） → 直接用于任务 DAG 构建
- 代码插桩经验（LICM） → 直接用于运行时调用替换

你真正需要新学的只有两件事：

1. **LLVM C++ API**（1~2 周，查文档即可，思维不用变）
2. **并行运行时库设计**（任务图调度，全新内容，但概念不复杂）

整个迁移路径是：**熟悉 LLVM API → 跑通 Pass 骨架 → 实现依赖分析 → 实现运行时库 → 性能调优**。
cholesky_pass/
├── CMakeLists.txt          ← 构建配置，告诉cmake怎么编译你的Pass和运行时库
├── include/                ← 现在是空的，后续放Pass的头文件（可选）
├── lib/
│   └── CholeleskyPass.cpp  ← 你的Pass核心代码，目前是识别三个算子的骨架
├── runtime/
│   └── runtime.c           ← 并行运行时库，目前是串行占位版本，后续改成真正的线程池
├── test/
│   ├── block_cholesky.c    ← 赛题的C源码，Pass的分析对象
│   └── block_cholesky.ll   ← 上面的C编译成的LLVM IR文本，用来调试Pass
├── build/                  ← 编译产物目录，里面有CholeleskyPass.so和libruntime.a
└── Doremy与LLVM_Pass...md  ← 文档，不参与编译