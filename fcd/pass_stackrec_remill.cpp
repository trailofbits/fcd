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

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>

#include <iostream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "remill/BC/Util.h"

#include "fcd/pass_stackrec_remill.h"

namespace fcd {

namespace {

static const char *sPrefix;
static CallingConvention *sCallConv;

static std::vector<llvm::BinaryOperator *> FindStackAddrOps(
    llvm::Argument *sp) {
  std::vector<llvm::BinaryOperator *> result;
  for (auto user : sp->users()) {
    if (auto op = llvm::dyn_cast<llvm::BinaryOperator>(user)) {
      auto idx =
          op->getOperand(0) == sp ? op->getOperand(1) : op->getOperand(0);
      if (llvm::isa<llvm::ConstantInt>(idx)) {
        switch (op->getOpcode()) {
          case llvm::Instruction::Add:
          case llvm::Instruction::Sub:
            result.push_back(op);
            break;
          default:
            break;
        }
      }
    }
  }
  return result;
}

static int64_t GetStackObjIdx(llvm::Argument *sp, llvm::BinaryOperator *op) {
  auto val = op->getOperand(0) == sp ? op->getOperand(1) : op->getOperand(0);
  auto idx = llvm::dyn_cast<llvm::ConstantInt>(val);
  CHECK(idx) << "Non-SP operand has to be a constant integer.";
  switch (op->getOpcode()) {
    case llvm::Instruction::Add:
      return idx->getSExtValue();
      break;
    case llvm::Instruction::Sub:
      return -idx->getSExtValue();
      break;
    default:
      CHECK(false) << "Invalid stack index binary operation.";
      return 0;
      break;
  }
}

static std::vector<llvm::IntToPtrInst *> FindStackAddrCasts(
    llvm::BinaryOperator *op, unsigned pmem_addr_space) {
  std::vector<llvm::IntToPtrInst *> result;
  for (auto user : op->users()) {
    if (auto int2ptr = llvm::dyn_cast<llvm::IntToPtrInst>(user)) {
      if (int2ptr->getAddressSpace() == pmem_addr_space) {
        result.push_back(int2ptr);
      }
    }
  }
  return result;
}

static llvm::Function *DeclareParametrizedFunc(
    llvm::Function *func, std::vector<llvm::Type *> &args) {
  std::stringstream name;
  name << sPrefix << func->getName().str();

  auto type = llvm::FunctionType::get(func->getReturnType(), args, false);
  auto new_func = llvm::dyn_cast<llvm::Function>(
      func->getParent()->getOrInsertFunction(name.str(), type));

  CHECK(new_func != nullptr);

  return new_func;
}

static llvm::Argument *GetStackPointerArg(llvm::Function *func) {
  auto sp_name = sCallConv->StackPointerVarName();
  if (func->arg_size() > 0) {
    for (auto &arg : func->args()) {
      if (arg.getName() == sp_name) {
        return &arg;
      }
    }
  }
  return nullptr;
}

static void CloneFunctionInto(llvm::Function *source_func,
                              llvm::Function *dest_func,
                              remill::ValueMap &value_map) {
  auto sp = GetStackPointerArg(source_func);
  // Lower SP argument to a local var
  llvm::IRBuilder<> ir(&*source_func->getEntryBlock().begin());
  std::stringstream loc_sp_name;
  loc_sp_name << "loc_" << sp->getName().str();
  auto loc_sp =
      ir.CreateLoad(ir.CreateAlloca(sp->getType(), nullptr, loc_sp_name.str()));
  sp->replaceAllUsesWith(loc_sp);
  // Prepare value map
  auto new_args = dest_func->arg_begin();
  for (auto &old_arg : source_func->args()) {
    if (&old_arg == sp) {
      continue;
    }
    new_args->setName(old_arg.getName());
    value_map[&old_arg] = &*new_args;
    ++new_args;
  }
  // Clone
  remill::CloneFunctionInto(source_func, dest_func, value_map);
}

static void FindStackObjects(
    llvm::Argument *sp, unsigned pmem_addr_space,
    std::unordered_map<int64_t, llvm::BinaryOperator *> &addrs,
    std::unordered_map<llvm::IntToPtrInst *, int64_t> &ptrs,
    std::unordered_map<int64_t, llvm::Type *> &objs) {
  for (auto op : FindStackAddrOps(sp)) {
    auto idx = GetStackObjIdx(sp, op);
    for (auto cast : FindStackAddrCasts(op, pmem_addr_space)) {
      auto ptr = llvm::dyn_cast<llvm::PointerType>(cast->getDestTy());
      CHECK(ptr) << "Could not determine type for stack object at offset "
                 << idx;
      auto new_ty = ptr->getElementType();
      if (auto &old_ty = objs[idx]) {
        if (old_ty->getPrimitiveSizeInBits() < new_ty->getPrimitiveSizeInBits())
          old_ty = new_ty;
      } else {
        objs[idx] = new_ty;
      }
      ptrs[cast] = idx;
    }
    addrs[idx] = op;
  }
}

static void PromoteStackObjsToVars(
    llvm::Argument *sp,
    std::unordered_map<int64_t, llvm::BinaryOperator *> &addrs,
    std::unordered_map<llvm::IntToPtrInst *, int64_t> &ptrs,
    std::unordered_map<int64_t, llvm::Type *> &objs,
    std::unordered_map<llvm::AllocaInst *, int64_t> &vars) {
  auto func = sp->getParent();

  llvm::IRBuilder<> ir(&*func->getEntryBlock().begin());

  for (auto obj : objs) {
    auto idx = obj.first;
    std::stringstream ss;
    ss << sp->getName().str() << idx;
    auto var = ir.CreateAlloca(obj.second, nullptr, ss.str());
    auto ref = ir.CreatePtrToInt(var, sp->getType());
    for (auto ptr : ptrs) {
      if (ptr.second == idx) {
        ptr.first->replaceAllUsesWith(var);
      }
    }
    addrs[idx]->replaceAllUsesWith(ref);
    vars[var] = idx;
  }
}

static std::vector<llvm::AllocaInst *> FindStackParams(
    llvm::CallInst *call,
    std::unordered_map<llvm::AllocaInst *, int64_t> &vars) {
  auto func = call->getCalledFunction();

  auto call_iter =
      llvm::BasicBlock::reverse_iterator(llvm::BasicBlock::iterator(call));

  auto call_range =
      llvm::make_range(std::next(call_iter), call->getParent()->rend());

  std::map<int64_t, llvm::AllocaInst *> func_vars;

  for (auto &inst : call_range) {
    if (auto cl = llvm::dyn_cast<llvm::CallInst>(&inst)) {
      if (GetStackPointerArg(cl->getCalledFunction())) {
        break;
      }
    }
    if (auto st = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
      auto ptr = st->getPointerOperand();
      if (auto var = llvm::dyn_cast<llvm::AllocaInst>(ptr)) {
        auto var_it = vars.find(var);
        if (var_it != vars.end()) {
          func_vars[var_it->second] = var_it->first;
        }
      }
    }
  }

  if (func_vars.empty()) {
    return {};
  }

  auto sp_top_arg = call->getArgOperand(GetStackPointerArg(func)->getArgNo());
  auto sp_top_int = llvm::dyn_cast<llvm::PtrToIntInst>(sp_top_arg);
  CHECK(sp_top_int != nullptr);
  auto sp_top_var = llvm::dyn_cast<llvm::AllocaInst>(sp_top_int->getOperand(0));
  CHECK(sp_top_var != nullptr);
  auto sp_top_it = func_vars.find(vars[sp_top_var]);

  std::vector<llvm::AllocaInst *> result;

  for (auto it = std::next(sp_top_it); it != func_vars.end(); ++it) {
    auto pv = std::prev(it);
    auto var_type = it->second->getAllocatedType();
    auto idx_size = std::abs(pv->first - it->first) * 8;
    auto var_size = var_type->getPrimitiveSizeInBits();
    if (idx_size == var_size) {
      result.push_back(it->second);
    } else {
      break;
    }
  }
  return result;
}

// Replaces a CallInst with a new one that has stack passed
// params added at the end of the parameter list.
// Note: This deletes the invalidates the `call` pointer.
static llvm::CallInst *LoadStackParamsToCall(
    llvm::CallInst *call,
    std::unordered_map<llvm::AllocaInst *, int64_t> &vars) {
  llvm::IRBuilder<> ir(call);

  std::vector<llvm::Value *> params;

  auto func = call->getCalledFunction();
  auto &sp_use = call->getArgOperandUse(GetStackPointerArg(func)->getArgNo());

  for (auto &use : call->arg_operands()) {
    if (use != sp_use) params.push_back(use.get());
  }

  for (auto var : FindStackParams(call, vars)) {
    params.push_back(ir.CreateLoad(var));
  }

  auto new_call = ir.CreateCall(func, params);
  call->replaceAllUsesWith(new_call);
  call->eraseFromParent();

  return new_call;
}

// Creates stores to local vars from function arguments and
// takes names from the local vars to the args themselves.
static void StoreStackParamsToLocals(
    llvm::Function *func, llvm::Argument *sp,
    std::map<int64_t, llvm::AllocaInst *> &stack_params) {
  llvm::IRBuilder<> ir(func->getContext());

  auto stack_idx = sp->getType()->getPrimitiveSizeInBits() / 8;
  for (auto &arg : func->args()) {
    if (arg.hasName()) {
      continue;
    }

    auto stack_param = stack_params.find(stack_idx);
    if (stack_param != stack_params.end()) {
      auto var = stack_param->second;
      ir.SetInsertPoint(var->getNextNode());
      ir.CreateStore(&arg, var);
      arg.takeName(var);
    } else {
      std::stringstream ss;
      ss << sp->getName().str() << stack_idx;
      arg.setName(ss.str());
    }

    stack_idx += arg.getType()->getPrimitiveSizeInBits() / 8;
  }
}

}  // namespace

char RemillStackRecovery::ID = 0;

RemillStackRecovery::RemillStackRecovery(void)
    : ModulePass(RemillStackRecovery::ID),
      cc(CallingConvention(remill::GetTargetArch()->DefaultCallingConv())) {
  sPrefix = cPrefix;
  sCallConv = &cc;
}

void RemillStackRecovery::getAnalysisUsage(llvm::AnalysisUsage &usage) const {}

bool RemillStackRecovery::runOnModule(llvm::Module &module) {
  std::unordered_map<llvm::AllocaInst *, int64_t> vars;

  std::vector<llvm::Function *> old_funcs;

  for (auto &func : module) {
    auto sp = GetStackPointerArg(&func);
    if (sp != nullptr && sp->hasNUsesOrMore(1) && !func.empty()) {
      // Find stack objects (stack variables and function parameters
      // passed via stack) by analyzing accesses to memory addresses
      // derived from the stack pointer. Addresses, pointer operands
      // derived from the addresses and the types of stack objects
      // are saved to `unordered_map`s for later use. The `int_64t`
      // keys and values are offsets from the stack pointer.
      std::unordered_map<int64_t, llvm::BinaryOperator *> addrs;
      std::unordered_map<llvm::IntToPtrInst *, int64_t> ptrs;
      std::unordered_map<int64_t, llvm::Type *> objs;
      FindStackObjects(sp, pmem_addr_space, addrs, ptrs, objs);

      // Promotes the stack objects found in `func` to local variables
      // using `alloca` instructions.
      PromoteStackObjsToVars(sp, addrs, ptrs, objs, vars);

      old_funcs.push_back(&func);
    }
  }

  std::vector<llvm::Function *> new_funcs;

  for (auto func : old_funcs) {
    // Load stack paramaters into new calls to func and find
    // a 'canonical' call among them. A canonical call is a
    // call that will be used to determine the new prototype
    // of func.
    llvm::CallInst *canonical = nullptr;
    for (auto call : remill::CallersOf(func)) {
      auto new_call = LoadStackParamsToCall(call, vars);
      if (!canonical) {
        canonical = new_call;
      }
      if (canonical->getNumArgOperands() > new_call->getNumArgOperands()) {
        canonical = new_call;
      }
    }
    // Find all stack objects in `func` that correspond to
    // function parameters passed via stack.
    std::map<int64_t, llvm::AllocaInst *> stack_params;
    for (auto var : vars) {
      if (var.first->getParent()->getParent() == func && var.second > 0) {
        stack_params[var.second] = var.first;
      }
    }
    // Determine argument types for the new function prototype.
    // This uses the canoncical call computed above. If no
    // canonical call could be computed, use the types of stack
    // parameters used inside `func`.
    auto sp_arg = GetStackPointerArg(func);
    std::vector<llvm::Type *> arg_types;
    if (canonical) {
      for (auto &arg : canonical->arg_operands()) {
        arg_types.push_back(arg->getType());
      }
    } else {
      for (auto &arg : func->args()) {
        if (&arg != sp_arg) {
          arg_types.push_back(arg.getType());
        }
      }
      for (auto param : stack_params) {
        arg_types.push_back(param.second->getAllocatedType());
      }
    }
    // Declare a new function that will replace `func`
    auto new_func = DeclareParametrizedFunc(func, arg_types);
    // Clone the contents of `func` into the new function.
    remill::ValueMap val_map;
    CloneFunctionInto(func, new_func, val_map);
    // Make values in `stack_params` point to the new function.
    for (auto &item : stack_params) {
      auto var = llvm::dyn_cast<llvm::AllocaInst>(val_map[item.second]);
      CHECK(var != nullptr);
      item.second = var;
    }

    StoreStackParamsToLocals(new_func, sp_arg, stack_params);
    // Change the called function in new calls from the old
    // function to the new one. The call parameter list
    // and the prototype of the called function should now
    // be consistent.
    for (auto call : remill::CallersOf(func)) {
      call->setCalledFunction(new_func);
    }

    new_funcs.push_back(new_func);
  }
  // Take the names from old functions and delete them.
  auto new_func = new_funcs.begin();
  for (auto old_func : old_funcs) {
    (*new_func)->takeName(old_func);
    old_func->replaceAllUsesWith(llvm::UndefValue::get(old_func->getType()));
    old_func->eraseFromParent();
    ++new_func;
  }

  return true;
}  // namespace fcd

llvm::ModulePass *createRemillStackRecoveryPass(void) {
  return new RemillStackRecovery;
}

static llvm::RegisterPass<RemillStackRecovery> remill_stackrec(
    "remill_stackrec", "Remill's Stack Frame Recovery", true, false);

}  // namespace fcd

// using namespace llvm;
// using RemillStackRecovery = fcd::RemillStackRecovery;
// INITIALIZE_PASS(RemillStackRecovery, "remill_stackrec",
//                 "Remill's Stack Frame Recovery", true, false)
