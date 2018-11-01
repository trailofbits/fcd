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

#ifndef FCD_AST_DEADSTMTELIM_H_
#define FCD_AST_DEADSTMTELIM_H_

#include <llvm/IR/Module.h>

#include "fcd/ast_remill/IRToASTVisitor.h"
#include "fcd/ast_remill/TransformVisitor.h"
#include "fcd/ast_remill/Util.h"

namespace fcd {

class DeadStmtElim : public llvm::ModulePass,
                     public TransformVisitor<DeadStmtElim> {
 private:
  clang::ASTContext *ast_ctx;
  fcd::IRToASTVisitor *ast_gen;

 public:
  static char ID;

  DeadStmtElim(clang::CompilerInstance &ins, fcd::IRToASTVisitor &ast_gen);

  bool VisitIfStmt(clang::IfStmt *ifstmt);
  bool VisitCompoundStmt(clang::CompoundStmt *compound);

  bool runOnModule(llvm::Module &module) override;
};

llvm::ModulePass *createDeadStmtElimPass(clang::CompilerInstance &ins,
                                         fcd::IRToASTVisitor &ast_gen);
}  // namespace fcd

namespace llvm {
void initializeDeadStmtElimPass(PassRegistry &);
}

#endif  // FCD_AST_DEADSTMTELIM_H_
