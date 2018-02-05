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

#ifndef FCD_CODEGEN_TRANSLATION_CONTEXT_REMILL_H_
#define FCD_CODEGEN_TRANSLATION_CONTEXT_REMILL_H_

#include <llvm/IR/Module.h>

#include "remill/BC/Lifter.h"

namespace fcd {

class TranslationContext {
private:
  // remill::InstructionLifter lifter;
  std::unique_ptr<llvm::Module> module;
public:
  TranslationContext(llvm::LLVMContext *context);
  ~TranslationContext();

  llvm::Function *CreateFunction(uint64_t addr);
  
  std::unique_ptr<llvm::Module> GetModule() {
    return module;
  }
};

} // namespace fcd
#endif // FCD_CODEGEN_TRANSLATION_CONTEXT_REMILL_H_