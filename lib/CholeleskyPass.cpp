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

struct TaskInfo { CallInst *call; std::string name; Value *write_ptr; std::vector<Value*> read_ptrs; };

struct CholeleskyPass : PassInfoMixin<CholeleskyPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    LLVMContext &C = M.getContext();
    Type *V=Type::getVoidTy(C), *I32=Type::getInt32Ty(C); PointerType *P=PointerType::getUnqual(C);
    auto RI=M.getOrInsertFunction("runtime_init",FunctionType::get(V,{I32},false));
    auto RS=M.getOrInsertFunction("runtime_submit",FunctionType::get(V,{P,P,I32,P,P,I32},false));
    auto RW=M.getOrInsertFunction("runtime_wait_all",FunctionType::get(V,{},false));
    auto RD=M.getOrInsertFunction("runtime_destroy",FunctionType::get(V,{},false));
    auto CW=M.getOrInsertFunction("cholesky_wrapper",FunctionType::get(V,{P},false));
    auto TW=M.getOrInsertFunction("trsm_wrapper",FunctionType::get(V,{P},false));
    auto MW=M.getOrInsertFunction("madd_wrapper",FunctionType::get(V,{P},false));

    for(auto &F:M){if(F.isDeclaration())continue;
      std::vector<TaskInfo> tasks;
      for(auto &BB:F)for(auto &I:BB){auto*CI=dyn_cast<CallInst>(&I);if(!CI)continue;
        auto*callee=CI->getCalledFunction();if(!callee)continue;
        auto name=callee->getName();if(name!="cholesky"&&name!="trsm"&&name!="madd")continue;
        TaskInfo t;t.call=CI;t.name=name.str();
        if(name=="cholesky"){t.read_ptrs.push_back(CI->getArgOperand(0));t.write_ptr=CI->getArgOperand(1);}
        else if(name=="trsm"){t.read_ptrs.push_back(CI->getArgOperand(0));t.read_ptrs.push_back(CI->getArgOperand(1));t.write_ptr=CI->getArgOperand(2);}
        else{t.read_ptrs.push_back(CI->getArgOperand(0));t.read_ptrs.push_back(CI->getArgOperand(1));t.write_ptr=CI->getArgOperand(2);}
        tasks.push_back(t);
      }
      if(tasks.empty())continue;

      errs()<<"[Pass] "<<F.getName()<<" ("<<tasks.size()<<" calls)\n";

      BasicBlock &E=F.getEntryBlock();IRBuilder<> EB(&*E.getFirstInsertionPt());
      EB.CreateCall(RI,{ConstantInt::get(I32,0)});

      std::vector<Value*> aA,rA,wA; std::vector<std::vector<Value*>> aS;
      for(size_t ti=0;ti<tasks.size();ti++){auto&t=tasks[ti];
        int na=t.name=="cholesky"?4:5, nr=t.name=="cholesky"?1:2;
        aA.push_back(EB.CreateAlloca(P,ConstantInt::get(I32,na)));
        rA.push_back(EB.CreateAlloca(P,ConstantInt::get(I32,nr)));
        wA.push_back(EB.CreateAlloca(P,ConstantInt::get(I32,1)));
        std::vector<Value*> sl;
        for(unsigned i=0;i<t.call->arg_size();i++){
          auto*a=t.call->getArgOperand(i);
          sl.push_back(a->getType()->isIntegerTy()?EB.CreateAlloca(a->getType(),nullptr):nullptr);
        }
        aS.push_back(sl);
      }

      for(auto &BB:F){auto*ret=dyn_cast<ReturnInst>(BB.getTerminator());if(!ret)continue;
        IRBuilder<> RB(ret);RB.CreateCall(RW,{});RB.CreateCall(RD,{});}

      for(size_t i=0;i<tasks.size();i++){
        auto &t=tasks[i];CallInst *CI=t.call;IRBuilder<> B(CI);
        auto wrapper=(t.name=="cholesky")?CW:((t.name=="trsm")?TW:MW);
        int na=t.name=="cholesky"?4:5, nr=t.name=="cholesky"?1:2;
        for(unsigned j=0;j<CI->arg_size();j++){
          auto*a=CI->getArgOperand(j);
          if(a->getType()->isIntegerTy()){auto*s=aS[i][j];B.CreateStore(a,s);a=s;}
          B.CreateStore(a,B.CreateConstGEP1_32(P,aA[i],j));
        }
        for(int j=0;j<nr;j++)B.CreateStore(t.read_ptrs[j],B.CreateConstGEP1_32(P,rA[i],j));
        B.CreateStore(t.write_ptr,B.CreateConstGEP1_32(P,wA[i],0));
        // CRITICAL: Load write_ptr VALUE from alloca slot, pass the actual GEP result
        Value *wp_val = B.CreateLoad(P, B.CreateConstGEP1_32(P, wA[i], 0));
        B.CreateCall(RS,{wrapper.getCallee(),aA[i],ConstantInt::get(I32,na),wp_val,rA[i],ConstantInt::get(I32,nr)});
        CI->eraseFromParent();
      }
    }
    return PreservedAnalyses::none();
  }
};

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo(){
  return{LLVM_PLUGIN_API_VERSION,"CholeleskyPass","v0.5",
    [](PassBuilder &P){P.registerPipelineParsingCallback([](StringRef N,ModulePassManager &M,ArrayRef<PassBuilder::PipelineElement>){if(N=="contestant-pass"){M.addPass(CholeleskyPass());return true;}return false;});}};
}
