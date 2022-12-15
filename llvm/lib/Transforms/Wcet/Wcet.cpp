
#include "llvm/Pass.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Constants.h"
#include "llvm/ADT/StringRef.h"

using namespace llvm;

//anonymous namespace 
namespace{

/**
 * @brief this method returns reference to metadata node llvm::MDNode
 * that contains as the first operand an llvm::MDString with name eq.
 * to "wcet.loop", the second operand of this node will be a 32bit 
 * unsigned equal to the time.
 * 
 * @param L *llvm::Loop
 * 
 * @return MD *llvm::MDNode
 * 
 */
MDNode * getWcetMetadataNode(const Loop *L) {
  // No loop metadata node, no loop properties.
  MDNode *LoopID = L->getLoopID();
  StringRef Name = "wcet.loop";

  if (!LoopID)
    return nullptr;

  // First operand should refer to the metadata node itself, for legacy reasons.
  assert(LoopID->getNumOperands() > 0 && "requires at least one operand");
  assert(LoopID->getOperand(0) == LoopID && "invalid loop id");

  // Iterate over the metdata node operands and look for MDString metadata.
  for (unsigned i = 1, e = LoopID->getNumOperands(); i < e; ++i) {
    
    MDNode *MD = dyn_cast<MDNode>(LoopID->getOperand(i));
    

    if (!MD || MD->getNumOperands() < 1)
      continue;
    
    MDString *S = dyn_cast<MDString>(MD->getOperand(0));
    if (!S)
      continue;

    // Return the operand node if MDString holds expected metadata.
    if (Name.equals(S->getString())){
      return MD;
    }   
  }
  // Wcet metadata not found
  return nullptr;
}

/**
 * @brief this method returns the time (parameter) of
 *        the wcet.loop attribute, which is an operand
 *        of the wcet.loop metadata 
 * 
 * @param L *llvm::Loop
 * 
 */
static int getWcetTime(const Loop *L) {
  MDNode *MD = getWcetMetadataNode(L);
  if (MD != nullptr) {
    assert(MD->getNumOperands() == 2 &&
           "Wcet loop metadata should have two operands.");
    int time = mdconst::extract<ConstantInt>(MD->getOperand(1))->getZExtValue();
    return time;
  }
  return -1;
}
  
  /**
   * WcetLoop pass (LoopPass because runs over each loop once)
   */
  struct WcetLoop: public LoopPass {
    static char ID;
    WcetLoop() : LoopPass(ID){}
    /**
     * method that runs over each loop present in the IR code
     * in this case we return the wcet time of a loop if the 
     * loop has the "wcet.loop" attribute
     * 
     */  
    bool runOnLoop(Loop *L, LPPassManager &LPM) override {
      int time = getWcetTime(L);
      if(time >= 0){errs() << "Time argument: " << time << "\n";}
      return false;
    }
  };

}//end of anpnymous namespace 


// the following instructions are used to register the pass with
// the legacy passManager, notice that no already existing pipeline
// has being used to integrate that pass
char WcetLoop::ID = 0;

static RegisterPass<WcetLoop> X("wcetloop", "Wcet Loop Pass",
                             false /* Only looks at CFG */,
                             false /* Analysis Pass */);

