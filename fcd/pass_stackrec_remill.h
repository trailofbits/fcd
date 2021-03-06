/*
 * Copyright (c) 2018 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FCD_PASS_STACKREC_REMILL_H_
#define FCD_PASS_STACKREC_REMILL_H_

#include <llvm/Analysis/Passes.h>
#include <llvm/IR/Module.h>

#include "fcd/callconv/callconv_remill.h"

namespace fcd {

class RemillStackRecovery : public llvm::ModulePass {
 private:
  const char *cPrefix = "stackrec_";
  const unsigned pmem_addr_space = 1;
  CallingConvention cc;

 public:
  static char ID;

  RemillStackRecovery(void);

  void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;
  bool runOnModule(llvm::Module &module) override;
};

llvm::ModulePass *createRemillStackRecoveryPass(void);
}  // namespace fcd

namespace llvm {
void initializeRemillStackRecoveryPass(PassRegistry &);
}

#endif  // FCD_PASS_STACKREC_REMILL_H_
