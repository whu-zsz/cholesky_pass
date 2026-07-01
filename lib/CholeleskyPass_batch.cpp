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
        Type *VoidTy = Type::getVoidTy(Ctx);
        Type *Int32Ty = Type::getInt32Ty(Ctx);
        PointerType *PtrTy = PointerType::getUnqual(Ctx);

        FunctionCallee RuntimeInit = M.getOrInsertFunction("runtime_init", FunctionType::get(VoidTy, {Int32Ty}, false));
        FunctionCallee RuntimeSubmit = M.getOrInsertFunction("runtime_submit", FunctionType::get(VoidTy, {PtrTy, PtrTy, Int32Ty, PtrTy, Int32Ty, PtrTy, Int32Ty}, false));
        FunctionCallee RuntimeWaitAll = M.getOrInsertFunction("runtime_wait_all", FunctionType::get(VoidTy, {}, false));
        FunctionCallee RuntimeDestroy = M.getOrInsertFunction("runtime_destroy", FunctionType::get(VoidTy, {}, false));
        FunctionCallee CholeskyWrapper = M.getOrInsertFunction("cholesky_wrapper", FunctionType::get(VoidTy, {PtrTy}, false));
        FunctionCallee TrsmWrapper = M.getOrInsertFunction("trsm_wrapper", FunctionType::get(VoidTy, {PtrTy}, false));
        FunctionCallee MaddWrapper = M.getOrInsertFunction("madd_wrapper", FunctionType::get(VoidTy, {PtrTy}, false));

        for (auto &F : M) {
            if (F.isDeclaration()) continue;
            std::vector<TaskInfo> tasks;
            for (auto &BB : F) {
                for (auto &I : BB) {
                    auto *CI = dyn_cast<CallInst>(&I);
                    if (!CI) continue;
                    auto *callee = CI->getCalledFunction();
                    if (!callee) continue;
                    StringRef name = callee->getName();
                    if (name != "cholesky" && name != "trsm" && name != "madd") continue;
                    TaskInfo t; t.call = CI; t.name = name.str();
                    if (name == "cholesky") { t.read_ptrs.push_back(CI->getArgOperand(0)); t.write_ptr = CI->getArgOperand(1); }
                    else if (name == "trsm") { t.read_ptrs.push_back(CI->getArgOperand(0)); t.read_ptrs.push_back(CI->getArgOperand(1)); t.write_ptr = CI->getArgOperand(2); }
                    else { t.read_ptrs.push_back(CI->getArgOperand(0)); t.read_ptrs.push_back(CI->getArgOperand(1)); t.write_ptr = CI->getArgOperand(2); }
                    tasks.push_back(t);
                }
            }
            if (tasks.empty()) continue;

            // ── 找 madd 调用，判断是否在自循环 BB ──
            const int BATCH_SZ = 256;
            CallInst *maddCI = nullptr; int maddIdx = -1;
            for (size_t i = 0; i < tasks.size(); i++) { if (tasks[i].name == "madd") { maddCI = tasks[i].call; maddIdx = (int)i; break; } }
            bool doBatch = true;
            BasicBlock *kLoopBB = nullptr, *kExitBB = nullptr;
            Value *bufB = nullptr, *bufC = nullptr, *bufCnt = nullptr;
            Value *maddA = nullptr, *maddB = nullptr, *maddC = nullptr, *maddBV = nullptr, *maddLD = nullptr;
            if (maddCI) {
                kLoopBB = maddCI->getParent();
                auto *term = dyn_cast<BranchInst>(kLoopBB->getTerminator());
                if (term && term->isConditional() && term->getSuccessor(0) == kLoopBB) {
                    kExitBB = term->getSuccessor(1);
                    maddA = maddCI->getArgOperand(0); maddB = maddCI->getArgOperand(1);
                    maddC = maddCI->getArgOperand(2); maddBV = maddCI->getArgOperand(3);
                    maddLD = maddCI->getArgOperand(4);
                    doBatch = true;
                }
            }

            errs() << "[Pass] instrumenting function: " << F.getName() << " (" << tasks.size() << " operator calls)" << (doBatch ? " [batch]" : "") << "\n";
            BasicBlock &EntryBB = F.getEntryBlock();
            IRBuilder<> EntryBuilder(&*EntryBB.getFirstInsertionPt());
            EntryBuilder.CreateCall(RuntimeInit, {ConstantInt::get(Int32Ty, 0)});

            // batch metadata
            Value *batchArgs = nullptr, *batchReads = nullptr;
            Value *slotB = nullptr, *slotL = nullptr, *cntSlot = nullptr;
            FunctionCallee MaddBatchWrapper;
            if (0) {
                bufB = EntryBuilder.CreateAlloca(PtrTy, ConstantInt::get(Int32Ty, BATCH_SZ), "mB");
                bufC = EntryBuilder.CreateAlloca(PtrTy, ConstantInt::get(Int32Ty, BATCH_SZ), "mC");
                bufCnt = EntryBuilder.CreateAlloca(Int32Ty, nullptr, "mCnt");
                EntryBuilder.CreateStore(ConstantInt::get(Int32Ty, 0), bufCnt);
                slotB = EntryBuilder.CreateAlloca(Int32Ty, nullptr);
                slotL = EntryBuilder.CreateAlloca(Int32Ty, nullptr);
                cntSlot = EntryBuilder.CreateAlloca(Int32Ty, nullptr);
                batchArgs = EntryBuilder.CreateAlloca(PtrTy, ConstantInt::get(Int32Ty, 6), "bArgs");
                batchReads = EntryBuilder.CreateAlloca(PtrTy, ConstantInt::get(Int32Ty, 1+BATCH_SZ), "bReads");
                MaddBatchWrapper = M.getOrInsertFunction("madd_batch_wrapper", FunctionType::get(VoidTy, {PtrTy}, false));
            }

            std::vector<Value*> argsArrays, readsArrays, writesArrays;
            std::vector<std::vector<Value*>> allArgSlots;
            for (auto &t : tasks) {
                int nargs = (t.name == "cholesky") ? 4 : 5;
                int nreads = (t.name == "cholesky") ? 1 : 2;
                Value *argsArr = EntryBuilder.CreateAlloca(PtrTy, ConstantInt::get(Int32Ty, nargs));
                Value *readsArr = EntryBuilder.CreateAlloca(PtrTy, ConstantInt::get(Int32Ty, nreads));
                Value *writesArr = EntryBuilder.CreateAlloca(PtrTy, ConstantInt::get(Int32Ty, 1));
                std::vector<Value*> slots;
                for (unsigned i = 0; i < t.call->arg_size(); i++) {
                    Value *arg = t.call->getArgOperand(i);
                    if (arg->getType()->isIntegerTy()) { Value *slot = EntryBuilder.CreateAlloca(arg->getType(), nullptr); slots.push_back(slot); }
                    else slots.push_back(nullptr);
                }
                argsArrays.push_back(argsArr); readsArrays.push_back(readsArr);
                writesArrays.push_back(writesArr); allArgSlots.push_back(slots);
            }
            for (auto &BB : F) {
                auto *ret = dyn_cast<ReturnInst>(BB.getTerminator());
                if (!ret) continue;
                IRBuilder<> RetBuilder(ret);
                RetBuilder.CreateCall(RuntimeWaitAll, {}); RetBuilder.CreateCall(RuntimeDestroy, {});
            }
            // ── batch transform ──
            if (0) {
                BasicBlock *kHead = kLoopBB;
                BasicBlock *kTail = kHead->splitBasicBlock(maddCI);
                maddCI->eraseFromParent();
                kHead->getTerminator()->eraseFromParent();
                IRBuilder<> HB(kHead);
                Value *cnt = HB.CreateLoad(Int32Ty, bufCnt);
                HB.CreateStore(maddB, HB.CreateGEP(PtrTy, bufB, {cnt}));
                HB.CreateStore(maddC, HB.CreateGEP(PtrTy, bufC, {cnt}));
                HB.CreateStore(HB.CreateAdd(cnt, ConstantInt::get(Int32Ty, 1)), bufCnt);
                HB.CreateBr(kTail);
                // kExit flush
                BasicBlock *kExitBody = kExitBB->splitBasicBlock(kExitBB->getTerminator());
                kExitBB->getTerminator()->eraseFromParent();
                IRBuilder<> EB(kExitBB);
                Value *rem = EB.CreateLoad(Int32Ty, bufCnt);
                Value *has = EB.CreateICmpSGT(rem, ConstantInt::get(Int32Ty, 0));
                BasicBlock *rFB = BasicBlock::Create(Ctx, "mb_rem", &F, kExitBody);
                EB.CreateCondBr(has, rFB, kExitBody);
                IRBuilder<> RB(rFB);
                RB.CreateStore(maddBV, slotB); RB.CreateStore(maddLD, slotL);
                RB.CreateStore(rem, cntSlot);
                RB.CreateStore(maddA, RB.CreateConstGEP1_32(PtrTy, batchArgs, 0));
                RB.CreateStore(slotB, RB.CreateConstGEP1_32(PtrTy, batchArgs, 1));
                RB.CreateStore(slotL, RB.CreateConstGEP1_32(PtrTy, batchArgs, 2));
                RB.CreateStore(cntSlot, RB.CreateConstGEP1_32(PtrTy, batchArgs, 3));
                RB.CreateStore(bufB, RB.CreateConstGEP1_32(PtrTy, batchArgs, 4));
                RB.CreateStore(bufC, RB.CreateConstGEP1_32(PtrTy, batchArgs, 5));
                RB.CreateStore(maddA, RB.CreateConstGEP1_32(PtrTy, batchReads, 0));
                for (int r = 0; r < BATCH_SZ; r++) {
                    Value *bv = RB.CreateLoad(PtrTy, RB.CreateConstGEP1_32(PtrTy, bufB, r));
                    RB.CreateStore(bv, RB.CreateConstGEP1_32(PtrTy, batchReads, 1+r));
                }
                Value *nr = RB.CreateAdd(rem, ConstantInt::get(Int32Ty, 1));
                RB.CreateCall(RuntimeSubmit, {MaddBatchWrapper.getCallee(), batchArgs, ConstantInt::get(Int32Ty, 6), bufC, rem, batchReads, nr});
                RB.CreateStore(ConstantInt::get(Int32Ty, 0), bufCnt);
                RB.CreateBr(kExitBody);
            }

            for (size_t idx = 0; idx < tasks.size(); idx++) {
                if (doBatch && (int)idx == maddIdx) continue;
                auto &t = tasks[idx]; CallInst *CI = t.call; IRBuilder<> B(CI);
                FunctionCallee wrapper = (t.name == "cholesky") ? CholeskyWrapper : TrsmWrapper;
                int nargs = (t.name == "cholesky") ? 4 : 5, nreads = (t.name == "cholesky") ? 1 : 2;
                Value *argsArray = argsArrays[idx], *readsArray = readsArrays[idx], *writesArray = writesArrays[idx];
                for (unsigned i = 0; i < CI->arg_size(); i++) {
                    Value *arg = CI->getArgOperand(i);
                    if (arg->getType()->isIntegerTy()) { Value *slot = allArgSlots[idx][i]; B.CreateStore(arg, slot); arg = slot; }
                    B.CreateStore(arg, B.CreateConstGEP1_32(PtrTy, argsArray, i));
                }
                for (int i = 0; i < nreads; i++) B.CreateStore(t.read_ptrs[i], B.CreateConstGEP1_32(PtrTy, readsArray, i));
                B.CreateStore(t.write_ptr, B.CreateConstGEP1_32(PtrTy, writesArray, 0));
                B.CreateCall(RuntimeSubmit, {wrapper.getCallee(), argsArray, ConstantInt::get(Int32Ty, nargs), writesArray, ConstantInt::get(Int32Ty, 1), readsArray, ConstantInt::get(Int32Ty, nreads)});
                CI->eraseFromParent();
            }
        }
        return PreservedAnalyses::none();
    }
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "CholeleskyPass", "v0.3",
        [](PassBuilder &PB) { PB.registerPipelineParsingCallback([](StringRef Name, ModulePassManager &MPM, ArrayRef<PassBuilder::PipelineElement>) { if (Name == "contestant-pass") { MPM.addPass(CholeleskyPass()); return true; } return false; }); }};
}
