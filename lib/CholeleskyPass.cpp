#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>
#include <string>

using namespace llvm;

// 描述一个算子调用的读写信息
struct TaskInfo {
    CallInst   *call;
    std::string name;
    Value      *write_ptr;
    std::vector<Value*> read_ptrs;
};

struct CholeleskyPass : PassInfoMixin<CholeleskyPass> {

    PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
        LLVMContext &Ctx = M.getContext();

        // 基础类型
        Type *VoidTy    = Type::getVoidTy(Ctx);
        Type *Int32Ty   = Type::getInt32Ty(Ctx);
        PointerType *PtrTy = PointerType::getUnqual(Ctx);

        // 声明运行时函数
        // void runtime_init(int)
        FunctionCallee RuntimeInit = M.getOrInsertFunction(
            "runtime_init",
            FunctionType::get(VoidTy, {Int32Ty}, false));

        // void runtime_submit(void(*)(void**), void**, int,
        //                     void*, void**, int)
        FunctionCallee RuntimeSubmit = M.getOrInsertFunction(
            "runtime_submit",
            FunctionType::get(VoidTy,
                {PtrTy, PtrTy, Int32Ty, PtrTy, PtrTy, Int32Ty},
                false));

        // void runtime_wait_all()
        FunctionCallee RuntimeWaitAll = M.getOrInsertFunction(
            "runtime_wait_all",
            FunctionType::get(VoidTy, {}, false));

        // void runtime_destroy()
        FunctionCallee RuntimeDestroy = M.getOrInsertFunction(
            "runtime_destroy",
            FunctionType::get(VoidTy, {}, false));

        // 声明三个wrapper函数（在runtime.c里实现）
        // void cholesky_wrapper(void**)
        FunctionCallee CholeskyWrapper = M.getOrInsertFunction(
            "cholesky_wrapper",
            FunctionType::get(VoidTy, {PtrTy}, false));
        FunctionCallee TrsmWrapper = M.getOrInsertFunction(
            "trsm_wrapper",
            FunctionType::get(VoidTy, {PtrTy}, false));
        FunctionCallee MaddWrapper = M.getOrInsertFunction(
            "madd_wrapper",
            FunctionType::get(VoidTy, {PtrTy}, false));
        errs() << "[Pass] running on module\n";
        for (auto &F : M) {
            errs() << "[Pass] seeing function: " << F.getName() << "\n"; 
            if (F.getName() != "block_cholesky") continue;
            errs() << "[Pass] entered block_cholesky\n";
            // 收集所有算子调用
            std::vector<TaskInfo> tasks;
            for (auto &BB : F) {
                for (auto &I : BB) {
                    auto *CI = dyn_cast<CallInst>(&I);
                    if (!CI) continue;
                    auto *callee = CI->getCalledFunction();
                    if (!callee) continue;

                    StringRef name = callee->getName();
                    if (name != "cholesky" &&
                        name != "trsm"     &&
                        name != "madd") continue;

                    TaskInfo t;
                    t.call = CI;
                    t.name = name.str();

                    if (name == "cholesky") {
                        t.read_ptrs.push_back(CI->getArgOperand(0));
                        t.write_ptr = CI->getArgOperand(1);
                    } else if (name == "trsm") {
                        t.read_ptrs.push_back(CI->getArgOperand(0));
                        t.read_ptrs.push_back(CI->getArgOperand(1));
                        t.write_ptr = CI->getArgOperand(2);
                    } else { // madd
                        t.read_ptrs.push_back(CI->getArgOperand(0));
                        t.read_ptrs.push_back(CI->getArgOperand(1));
                        t.write_ptr = CI->getArgOperand(2);
                    }
                    tasks.push_back(t);
                }
            }

            errs() << "[Pass] found " << tasks.size()
                   << " operator calls\n";

            // 在函数入口插入 runtime_init(4)
            IRBuilder<> EntryBuilder(
                &*F.getEntryBlock().getFirstInsertionPt());
            EntryBuilder.CreateCall(RuntimeInit,
                {ConstantInt::get(Int32Ty, 4)});

            // 在函数所有return前插入 runtime_wait_all + runtime_destroy
            for (auto &BB : F) {
                auto *ret = dyn_cast<ReturnInst>(BB.getTerminator());
                if (!ret) continue;
                IRBuilder<> RetBuilder(ret);
                RetBuilder.CreateCall(RuntimeWaitAll, {});
                RetBuilder.CreateCall(RuntimeDestroy, {});
            }

            // 替换每个算子调用为 runtime_submit
            for (auto &t : tasks) {
                CallInst *CI = t.call;
                IRBuilder<> B(CI);  // 在原调用位置插入

                // 确定wrapper和参数个数
                FunctionCallee wrapper;
                int nargs, nreads;
                if (t.name == "cholesky") {
                    wrapper = CholeskyWrapper;
                    nargs = 4; nreads = 1;
                } else if (t.name == "trsm") {
                    wrapper = TrsmWrapper;
                    nargs = 6; nreads = 2;
                } else {
                    wrapper = MaddWrapper;
                    nargs = 6; nreads = 2;
                }

                // 分配 args 数组（在栈上）：void* args[nargs]
                Value *argsArray = B.CreateAlloca(
                    PtrTy,
                    ConstantInt::get(Int32Ty, nargs),
                    "args");

                // 把每个参数存进 args[i]
                for (unsigned i = 0; i < CI->arg_size(); i++) {
                    Value *arg = CI->getArgOperand(i);
                    // 如果是整数，先alloca一个int存起来，传地址
                    if (arg->getType()->isIntegerTy()) {
                        Value *slot = B.CreateAlloca(
                            arg->getType(), nullptr, "arg_slot");
                        B.CreateStore(arg, slot);
                        arg = slot;  // 传指针
                    }
                    Value *gep = B.CreateConstGEP1_32(
                        PtrTy, argsArray, i);
                    B.CreateStore(arg, gep);
                }

                // 分配 read_ptrs 数组
                Value *readsArray = B.CreateAlloca(
                    PtrTy,
                    ConstantInt::get(Int32Ty, nreads),
                    "reads");
                for (int i = 0; i < nreads; i++) {
                    Value *gep = B.CreateConstGEP1_32(
                        PtrTy, readsArray, i);
                    B.CreateStore(t.read_ptrs[i], gep);
                }

                // 调用 runtime_submit
                B.CreateCall(RuntimeSubmit, {
                    wrapper.getCallee(),   // func
                    argsArray,             // args
                    ConstantInt::get(Int32Ty, nargs),
                    t.write_ptr,           // write_ptr
                    readsArray,            // read_ptrs
                    ConstantInt::get(Int32Ty, nreads)
                });

                // 删除原始调用
                CI->eraseFromParent();
            }
        }

        return PreservedAnalyses::none();
    }
};

extern "C" LLVM_ATTRIBUTE_WEAK
PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION, "CholeleskyPass", "v0.1",
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "cholesky-parallel") {
                        MPM.addPass(CholeleskyPass());
                        return true;
                    }
                    return false;
                });
        }
    };
}