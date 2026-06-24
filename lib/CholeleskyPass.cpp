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

struct TaskInfo {
    CallInst   *call;
    std::string name;
    Value      *write_ptr;
    std::vector<Value*> read_ptrs;
};

struct CholeleskyPass : PassInfoMixin<CholeleskyPass> {

    PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
        LLVMContext &Ctx = M.getContext();

        Type *VoidTy       = Type::getVoidTy(Ctx);
        Type *Int32Ty      = Type::getInt32Ty(Ctx);
        PointerType *PtrTy = PointerType::getUnqual(Ctx);

        FunctionCallee RuntimeInit = M.getOrInsertFunction(
            "runtime_init",
            FunctionType::get(VoidTy, {Int32Ty}, false));
        FunctionCallee RuntimeSubmit = M.getOrInsertFunction(
            "runtime_submit",
            FunctionType::get(VoidTy,
                {PtrTy, PtrTy, Int32Ty, PtrTy, PtrTy, Int32Ty}, false));
        FunctionCallee RuntimeWaitAll = M.getOrInsertFunction(
            "runtime_wait_all",
            FunctionType::get(VoidTy, {}, false));
        FunctionCallee RuntimeDestroy = M.getOrInsertFunction(
            "runtime_destroy",
            FunctionType::get(VoidTy, {}, false));
        FunctionCallee CholeskyWrapper = M.getOrInsertFunction(
            "cholesky_wrapper",
            FunctionType::get(VoidTy, {PtrTy}, false));
        FunctionCallee TrsmWrapper = M.getOrInsertFunction(
            "trsm_wrapper",
            FunctionType::get(VoidTy, {PtrTy}, false));
        FunctionCallee MaddWrapper = M.getOrInsertFunction(
            "madd_wrapper",
            FunctionType::get(VoidTy, {PtrTy}, false));

        for (auto &F : M) {
            if (F.isDeclaration()) continue;

            // 收集算子调用
            std::vector<TaskInfo> tasks;
            for (auto &BB : F) {
                for (auto &I : BB) {
                    auto *CI = dyn_cast<CallInst>(&I);
                    if (!CI) continue;
                    auto *callee = CI->getCalledFunction();
                    if (!callee) continue;
                    StringRef name = callee->getName();
                    if (name != "cholesky" && name != "trsm" && name != "madd") continue;

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
                    } else {
                        t.read_ptrs.push_back(CI->getArgOperand(0));
                        t.read_ptrs.push_back(CI->getArgOperand(1));
                        t.write_ptr = CI->getArgOperand(2);
                    }
                    tasks.push_back(t);
                }
            }

            if (tasks.empty()) continue;

            errs() << "[Pass] instrumenting function: "
                   << F.getName() << " (" << tasks.size()
                   << " operator calls)\n";

            // 在函数入口插入 runtime_init
            // 同时在入口预分配所有 alloca（避免在循环体内反复分配栈空间）
            BasicBlock &EntryBB = F.getEntryBlock();
            IRBuilder<> EntryBuilder(&*EntryBB.getFirstInsertionPt());
            EntryBuilder.CreateCall(RuntimeInit,
                {ConstantInt::get(Int32Ty, 8)});

            // 预分配每个task的args/reads数组和整数slot
            // 这些alloca全部放在入口块，不随循环迭代重复分配
            std::vector<Value*> argsArrays;
            std::vector<Value*> readsArrays;
            std::vector<std::vector<Value*>> allArgSlots;

            for (auto &t : tasks) {
                int nargs  = (t.name == "cholesky") ? 4 : 5;
                int nreads = (t.name == "cholesky") ? 1 : 2;

                Value *argsArr = EntryBuilder.CreateAlloca(
                    PtrTy, ConstantInt::get(Int32Ty, nargs), "args_pre");
                Value *readsArr = EntryBuilder.CreateAlloca(
                    PtrTy, ConstantInt::get(Int32Ty, nreads), "reads_pre");

                std::vector<Value*> slots;
                for (unsigned i = 0; i < t.call->arg_size(); i++) {
                    Value *arg = t.call->getArgOperand(i);
                    if (arg->getType()->isIntegerTy()) {
                        Value *slot = EntryBuilder.CreateAlloca(
                            arg->getType(), nullptr, "int_slot");
                        slots.push_back(slot);
                    } else {
                        slots.push_back(nullptr);
                    }
                }

                argsArrays.push_back(argsArr);
                readsArrays.push_back(readsArr);
                allArgSlots.push_back(slots);
            }

            // 在所有 return 前插入 runtime_wait_all + runtime_destroy
            for (auto &BB : F) {
                auto *ret = dyn_cast<ReturnInst>(BB.getTerminator());
                if (!ret) continue;
                IRBuilder<> RetBuilder(ret);
                RetBuilder.CreateCall(RuntimeWaitAll, {});
                RetBuilder.CreateCall(RuntimeDestroy, {});
            }

            // 在每个调用点替换为 runtime_submit（只做 store，不做 alloca）
            for (size_t idx = 0; idx < tasks.size(); idx++) {
                auto &t = tasks[idx];
                CallInst *CI = t.call;
                IRBuilder<> B(CI);

                FunctionCallee wrapper;
                int nargs, nreads;
                if (t.name == "cholesky") {
                    wrapper = CholeskyWrapper; nargs = 4; nreads = 1;
                } else if (t.name == "trsm") {
                    wrapper = TrsmWrapper; nargs = 5; nreads = 2;
                } else {
                    wrapper = MaddWrapper; nargs = 5; nreads = 2;
                }

                Value *argsArray  = argsArrays[idx];
                Value *readsArray = readsArrays[idx];

                // 把参数存入预分配的args数组
                for (unsigned i = 0; i < CI->arg_size(); i++) {
                    Value *arg = CI->getArgOperand(i);
                    if (arg->getType()->isIntegerTy()) {
                        Value *slot = allArgSlots[idx][i];
                        B.CreateStore(arg, slot);
                        arg = slot;
                    }
                    Value *gep = B.CreateConstGEP1_32(PtrTy, argsArray, i);
                    B.CreateStore(arg, gep);
                }

                // 把读指针存入预分配的reads数组
                for (int i = 0; i < nreads; i++) {
                    Value *gep = B.CreateConstGEP1_32(PtrTy, readsArray, i);
                    B.CreateStore(t.read_ptrs[i], gep);
                }

                // 插入 runtime_submit 调用
                B.CreateCall(RuntimeSubmit, {
                    wrapper.getCallee(),
                    argsArray,
                    ConstantInt::get(Int32Ty, nargs),
                    t.write_ptr,
                    readsArray,
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
        LLVM_PLUGIN_API_VERSION, "CholeleskyPass", "v0.3",
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "contestant-pass") {
                        MPM.addPass(CholeleskyPass());
                        return true;
                    }
                    return false;
                });
        }
    };
}
