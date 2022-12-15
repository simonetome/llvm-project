add_llvm_library( LLVMWcet MODULE 
  Wcet.cpp
  
  PLUGIN_TOOL
  opt
  )
