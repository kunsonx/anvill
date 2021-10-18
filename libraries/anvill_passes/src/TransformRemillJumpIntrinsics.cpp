/*
 * Copyright (c) 2021 Trail of Bits, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "TransformRemillJumpIntrinsics.h"

#include <anvill/Analysis/CrossReferenceResolver.h>
#include <anvill/Analysis/Utils.h>
#include <anvill/Lifters/EntityLifter.h>
#include <anvill/Transforms.h>
#include <glog/logging.h>
#include <llvm/ADT/Triple.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Local.h>
#include <remill/BC/ABI.h>
#include <remill/BC/Compat/ScalarTransforms.h>
#include <remill/BC/Util.h>

#include <utility>
#include <vector>

#include "Utils.h"


namespace anvill {
namespace {

const std::string kRemillJumpIntrinsicName = "__remill_jump";
const std::string kRemillFunctionReturnIntrinsicName =
    "__remill_function_return";

// Find the remill intrinsic in module. If it is missing create one
// with the given function type
static llvm::Function *FindIntrinsic(llvm::Module *module,
                                     llvm::FunctionType *type,
                                     const std::string &name) {
  auto function = module->getFunction(name);
  if (!function) {
    function = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                                      name, module);
  }

  CHECK(nullptr != function) << "Unable to get intrinsic: " << name;
  function->addFnAttr(llvm::Attribute::NoDuplicate);
  function->addFnAttr(llvm::Attribute::NoUnwind);
  function->addFnAttr(llvm::Attribute::OptimizeNone);
  function->addFnAttr(llvm::Attribute::NoInline);
  function->removeFnAttr(llvm::Attribute::NoReturn);
  function->removeFnAttr(llvm::Attribute::UWTable);
  function->removeFnAttr(llvm::Attribute::AlwaysInline);
  return function;
}

// Find the call site of the given function and add them to vector
// if `pred(call)` is true
template <typename T>
std::vector<llvm::CallBase *> FindFunctionCalls(llvm::Function &func, T pred) {
  std::vector<llvm::CallBase *> found;
  for (auto &inst : llvm::instructions(func)) {
    if (auto call = llvm::dyn_cast<llvm::CallBase>(&inst); call && pred(call)) {
      found.push_back(call);
    }
  }
  return found;
}


}  // namespace


// Returns `true` if `val` is a possible return address
ReturnAddressResult
TransformRemillJumpIntrinsics::QueryReturnAddress(llvm::Module *module,
                                                  llvm::Value *val) const {

  if (IsReturnAddress(module, val)) {
    return kReturnAddressProgramCounter;

  } else if (auto pti = llvm::dyn_cast<llvm::PtrToIntOperator>(val)) {
    return QueryReturnAddress(module, pti->getOperand(0));

    // Sometimes optimizations result in really crazy looking constant expressions
    // related to `__anvill_ra`, full of shifts, zexts, etc. We try to detect
    // this situation by initializing a "magic" address associated with
    // `__anvill_ra`, and then if we find this magic value on something that
    // references `__anvill_ra`, then we conclude that all those manipulations
    // in the constant expression are actually not important.
  } else if (auto xr = xref_resolver_.TryResolveReferenceWithClearedCache(val);
             xr.is_valid && xr.references_return_address &&
             xr.u.address == xref_resolver_.MagicReturnAddressValue()) {
    return kReturnAddressProgramCounter;
  }

  return kUnclassifiableProgramCounter;
}

// Dispatch to the proper memory replacement function given a function call.
bool TransformRemillJumpIntrinsics::TransformJumpIntrinsic(
    llvm::CallBase *call) {
  const auto func = call->getCalledFunction();
  const auto module = func->getParent();
  bool func_replaced = false;

  const auto called_func = call->getCalledFunction();
  if (called_func && called_func->getName() == kRemillJumpIntrinsicName) {
    auto func_type = call->getCalledFunction()->getFunctionType();
    auto intrinsic =
        FindIntrinsic(module, func_type, kRemillFunctionReturnIntrinsicName);

    // undef state pointer argument
    auto state_ptr_arg = call->getArgOperand(remill::kStatePointerArgNum);
    auto undef_val = llvm::UndefValue::get(state_ptr_arg->getType());
    CopyMetadataTo(state_ptr_arg, undef_val);
    call->setArgOperand(remill::kStatePointerArgNum, undef_val);

    call->setCalledOperand(intrinsic);

    // if the called function has no uses delete them
    if (called_func->use_empty()) {
      called_func->eraseFromParent();
    }

    func_replaced = true;
  }

  return func_replaced;
}


// Try to identify the patterns of `__remill_function_call` that we can
// remove.
llvm::PreservedAnalyses
TransformRemillJumpIntrinsics::run(llvm::Function &F,
                                   llvm::FunctionAnalysisManager &AM) {
  const auto module = F.getParent();
  const auto &dl = module->getDataLayout();
  auto calls = FindFunctionCalls(F, [&](llvm::CallBase *call) -> bool {
    const auto func = call->getCalledFunction();
    if (!func || func->getName() != kRemillJumpIntrinsicName) {
      return false;
    }

    const auto ret_addr =
        call->getArgOperand(remill::kPCArgNum)->stripPointerCastsAndAliases();
    switch (QueryReturnAddress(module, ret_addr)) {
      case kReturnAddressProgramCounter: return true;
      case kUnclassifiableProgramCounter: return false;
    }
  });

  auto ret = false;
  for (auto call : calls) {
    ret = TransformJumpIntrinsic(call) || ret;
  }

  if (ret) {

    // Run private function passes if the jump instrinsics
    // are replaced
    llvm::FunctionPassManager fpm;

    fpm.addPass(llvm::DCEPass());
    fpm.addPass(llvm::SROA());
    fpm.addPass(llvm::SimplifyCFGPass());
    fpm.addPass(llvm::InstCombinePass());
    fpm.run(F, AM);
  }

  return ConvertBoolToPreserved(ret);
}


// The pass transforms bitcode to replace the calls to `__remill_jump` into
// `__remill_function_return` if a value returned by `llvm.returnaddress`, or
// casted from `__anvill_ra`, reaches to its `PC` argument.
//
// The transform is written to fix the bitcode generated for aarch32 architecture
// where multiple instructions semantic can be used to return from the function
// and they might be categorized as (conditional/unconditional) indirect jumps
//
// It identifies the possible cases where a return instruction is lifted as
// indirect jump and fixes the intrinsics for them. The pass should be run before
// `RemoveRemillFunctionReturns` and as late as possible in the list
void AddTransformRemillJumpIntrinsics(llvm::FunctionPassManager &fpm,
                                      const EntityLifter &lifter) {
  fpm.addPass(TransformRemillJumpIntrinsics(lifter));
}

}  // namespace anvill