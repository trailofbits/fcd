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

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "fcd/ast_remill/NestedCondProp.h"
#include "fcd/ast_remill/Z3ConvVisitor.h"

namespace fcd {

char NestedCondProp::ID = 0;

NestedCondProp::NestedCondProp(clang::CompilerInstance &ins,
                               fcd::IRToASTVisitor &ast_gen)
    : ModulePass(NestedCondProp::ID),
      ast_ctx(&ins.getASTContext()),
      ast_gen(&ast_gen),
      z3_ctx(new z3::context()),
      z3_gen(new fcd::Z3ConvVisitor(ast_ctx, z3_ctx.get())) {}

bool NestedCondProp::VisitIfStmt(clang::IfStmt *ifstmt) {
  // DLOG(INFO) << "VisitIfStmt";
  auto cond = ifstmt->getCond();
  auto then = ifstmt->getThen();
  // Retrieve a parent `clang::IfStmt` condition
  // and remove it from `cond` if it's present.
  auto iter = parent_conds.find(ifstmt);
  if (iter != parent_conds.end()) {
    auto child_expr = z3_gen->GetOrCreateZ3Expr(cond);
    auto parent_expr = z3_gen->GetOrCreateZ3Expr(iter->second);
    z3::expr_vector src(*z3_ctx);
    z3::expr_vector dst(*z3_ctx);
    src.push_back(parent_expr);
    dst.push_back(z3_ctx->bool_val(true));
    auto sub = child_expr.substitute(src, dst).simplify();
    ifstmt->setCond(z3_gen->GetOrCreateCExpr(sub));
  }
  // Determine whether `cond` is a constant expression
  if (!cond->isIntegerConstantExpr(*ast_ctx)) {
    // `cond` is not a constant expression and we propagate it
    // to `clang::IfStmt` nodes in it's `then` branch.
    if (auto comp = llvm::dyn_cast<clang::CompoundStmt>(then)) {
      for (auto stmt : comp->body()) {
        if (auto child = llvm::dyn_cast<clang::IfStmt>(stmt)) {
          parent_conds[child] = cond;
        }
      }
    } else {
      LOG(FATAL) << "Then branch must be a clang::CompoundStmt!";
    }
  }
  return true;
}

bool NestedCondProp::runOnModule(llvm::Module &module) {
  LOG(INFO) << "Propagating nested conditions";
  TraverseDecl(ast_ctx->getTranslationUnitDecl());
  return true;
}

llvm::ModulePass *createNestedCondPropPass(clang::CompilerInstance &ins,
                                           fcd::IRToASTVisitor &gen) {
  return new NestedCondProp(ins, gen);
}
}  // namespace fcd