/*
 * Copyright (c) 2019-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#include "InstructionFolderPass.h"

#include <glog/logging.h>

#include <unordered_map>
#include <unordered_set>

namespace anvill {

namespace {
//
// These maps select the correct fold operation for the given instruction
// types.
//
// The first callback is kInstructionFolderMap[instr_type] which (for now) can
// handle Select and PHI nodes.
//
// Inside the function folder, an additional callback is used, depending on the
// type of the second instruction being folded. The two maps are:
//  - SelectInst: kSelectInstructionFolderMap
//  - PHINode: kPHINodeFolderMap
//

using InstructionFolder = bool (InstructionFolderPass::PassFunctionState::*)(
    InstructionFolderPass::InstructionList &, llvm::Instruction *);

// clang-format off
const std::unordered_map<std::uint32_t, InstructionFolder> kInstructionFolderMap = {
  { llvm::Instruction::Select, &InstructionFolderPass::PassFunctionState::FoldSelectInstruction },
  { llvm::Instruction::PHI, &InstructionFolderPass::PassFunctionState::FoldPHINode },
};

// clang-format on

// Case handlers for `SelectInst` instructions
using SelectInstructionFolder = bool (*)(llvm::Instruction *&output,
                                         llvm::Instruction *, llvm::Value *,
                                         llvm::Value *, llvm::Value *,
                                         llvm::Instruction *);

// clang-format off
const std::unordered_map<std::uint32_t, SelectInstructionFolder> kSelectInstructionFolderMap = {

  // Binary operators
  { llvm::Instruction::Add, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::FAdd, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::Sub, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::FSub, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::Mul, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::FMul, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::UDiv, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::SDiv, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp},
  { llvm::Instruction::FDiv, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::URem, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::SRem, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::FRem, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::Shl, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::LShr, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp},
  { llvm::Instruction::AShr, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::And, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::Or, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },
  { llvm::Instruction::Xor, InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp },

  // Cast operators
  { llvm::Instruction::Trunc, InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst },
  { llvm::Instruction::ZExt, InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst },
  { llvm::Instruction::SExt, InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst },
  { llvm::Instruction::FPToUI, InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst },
  { llvm::Instruction::FPToSI, InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst},
  { llvm::Instruction::UIToFP, InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst },
  { llvm::Instruction::SIToFP,InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst },
  { llvm::Instruction::FPTrunc, InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst },
  { llvm::Instruction::FPExt, InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst },
  { llvm::Instruction::PtrToInt,InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst },
  { llvm::Instruction::IntToPtr, InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst },
  { llvm::Instruction::BitCast, InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst },

  // Memory operators
  { llvm::Instruction::GetElementPtr, InstructionFolderPass::PassFunctionState::FoldSelectWithGEPInst },
};

// clang-format on

// Case handlers for `PHINode` instructions
using PHINodeInstructionFolder =
    bool (InstructionFolderPass::PassFunctionState::*)(
        llvm::Instruction *&output, llvm::Instruction *,
        InstructionFolderPass::IncomingValueList &, llvm::Instruction *);

// clang-format off
const std::unordered_map<std::uint32_t, PHINodeInstructionFolder> kPHINodeFolderMap = {

  // Binary operators
  { llvm::Instruction::Add, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::FAdd, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::Sub, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::FSub, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::Mul, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::FMul, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::UDiv, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::SDiv, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::FDiv, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::URem, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::SRem, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::FRem, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::Shl, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::LShr, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::AShr, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::And, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::Or, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },
  { llvm::Instruction::Xor, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp },

  // Cast operators
  { llvm::Instruction::Trunc, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithCastInst },
  { llvm::Instruction::ZExt, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithCastInst },
  { llvm::Instruction::SExt, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithCastInst },
  { llvm::Instruction::FPToUI, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithCastInst },
  { llvm::Instruction::FPToSI, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithCastInst },
  { llvm::Instruction::UIToFP, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithCastInst },
  { llvm::Instruction::SIToFP, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithCastInst },
  { llvm::Instruction::FPTrunc, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithCastInst },
  { llvm::Instruction::FPExt, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithCastInst },


  // NOTE(akshayk): A phi node folding with cast instructions like IntToPtr
  //                PtrToInt, and BitCast may enter into an infinite loop if
  //                the folding instruction bounce between each other. Disable
  //                them in the map and revisit when we have the fix.
#if 0
  { llvm::Instruction::PtrToInt, &InstructionFolderPass::FoldPHINodeWithCastInst },
  { llvm::Instruction::IntToPtr, &InstructionFolderPass::FoldPHINodeWithCastInst },
  { llvm::Instruction::BitCast, &InstructionFolderPass::FoldPHINodeWithCastInst },
#endif

  // Memory operators
  { llvm::Instruction::GetElementPtr, &InstructionFolderPass::PassFunctionState::FoldPHINodeWithGEPInst },
};

// clang-format on

}  // namespace

bool InstructionFolderPass::Run(llvm::Function &function,
                                llvm::FunctionAnalysisManager &fam) {
  if (function.isDeclaration()) {
    return false;
  }

  const auto &dt = fam.getResult<llvm::DominatorTreeAnalysis>(function);
  auto function_state =
      std::make_unique<InstructionFolderPass::PassFunctionState>(dt);
  // Create an initial queue of possible candidates
  auto next_worklist =
      SelectInstructions<llvm::SelectInst, llvm::PHINode>(function);

  std::unordered_set<llvm::Instruction *> visited_instructions;

  // Go through the list of discovered instructions
  InstructionList worklist;
  bool function_changed{false};

  for (auto gas = 0u; gas < 8u && !next_worklist.empty(); ++gas) {
    worklist.swap(next_worklist);
    next_worklist.clear();

    for (auto instr : worklist) {

      // Keep track of the instructions we have visited so we don't
      // loop forever
      if (visited_instructions.count(instr) != 0U) {
        continue;
      }

      visited_instructions.insert(instr);

      // Attempt to combine this instruction; the folder function also
      // drops all the instructions that are no longer needed but will
      // keep `instr` alive for now
      auto instr_folder_it = kInstructionFolderMap.find(instr->getOpcode());
      if (instr_folder_it != kInstructionFolderMap.end()) {
        const auto instr_folder = instr_folder_it->second;
        if ((function_state.get()->*instr_folder)(next_worklist, instr)) {
          function_changed = true;
        }
      }
    }
  }

  // Go through the visited instructions and drop the ones we no
  // longer need. Doing the cleanup now ensures we do not risk
  // new instructions polluting the unordered_set by ending up
  // allocated at a memory location we have already seen
  for (auto &instr : visited_instructions) {
    if (instr->use_empty()) {
      instr->eraseFromParent();
      function_changed = true;
    }
  }

  return function_changed;
}

llvm::StringRef InstructionFolderPass::name(void) {
  return llvm::StringRef("InstructionFolderPass");
}


void InstructionFolderPass::PerformInstructionReplacements(
    const InstructionReplacementList &replacement_list) {
  for (const auto &repl : replacement_list) {
    repl.replacement_instr->copyMetadata(*repl.original_instr);
    repl.original_instr->replaceAllUsesWith(repl.replacement_instr);
    repl.original_instr->eraseFromParent();
  }
}

// If there is cyclic dependency incoming values on phi node; It can't be folded. The
// utility function checks such cases.
static inline bool
IsPHINodeFoldable(llvm::Instruction *instr,
                  InstructionFolderPass::IncomingValueList &incoming_values) {
  for (auto &incoming_value : incoming_values) {
    if (llvm::isa<llvm::PHINode>(incoming_value.value)) {
      return false;
    }
    for (auto user : instr->users()) {
      if (user == incoming_value.value) {
        return false;
      }
    }
  }
  return true;
}


bool InstructionFolderPass::CollectAndValidateGEPIndexes(
    std::vector<llvm::Value *> &index_list,
    llvm::Instruction *phi_or_select_instr, llvm::Instruction *gep_instr) {

  index_list.clear();

  // Acquire all the indices and verify them:
  // 1. If they are instructions, then all the indices in the GEP
  //    that are preceding our PHI/Select use must NOT reside in the same
  //    basic block as the GEP (otherwise we won't have them when
  //    we move the GetElementPtrInst inside the incoming basic block)
  // 2. All the indices that are following the PHI/Select use must be constants
  auto instr = llvm::dyn_cast<llvm::GetElementPtrInst>(gep_instr);
  bool phi_or_select_index_found{false};

  for (auto &index_use : instr->indices()) {
    auto index = index_use.get();
    index_list.push_back(index);

    // If this is our PHI/Select node, update the verification stage and skip
    // any check
    auto index_as_instr = llvm::dyn_cast<llvm::Instruction>(index);
    if (index_as_instr == phi_or_select_instr) {
      phi_or_select_index_found = true;
      continue;
    }

    if (!phi_or_select_index_found) {

      // We have not met the PHI/Select index yet, make sure that this
      // index is still reachable if we move the GEP
      if (index_as_instr != nullptr &&
          index_as_instr->getParent() == gep_instr->getParent()) {
        return false;
      }

    } else {

      // We are after the PHI/Select index; make sure that this value is a
      // constant
      if (!llvm::isa<llvm::Constant>(index)) {
        return false;
      }
    }
  }

  return true;
}

bool InstructionFolderPass::PassFunctionState::FoldSelectWithBinaryOp(
    llvm::Instruction *&output, llvm::Instruction *select_instr,
    llvm::Value *condition, llvm::Value *true_value, llvm::Value *false_value,
    llvm::Instruction *binary_op_instr) {

  // This binary operator is using our `select` instruction. Take the
  // operand on the opposite side
  auto operand = binary_op_instr->getOperand(0U);
  if (operand == select_instr) {
    operand = binary_op_instr->getOperand(1U);
  }

  // In order to be able to combine these two instructions, the
  // other operand needs to be a constant
  if (!llvm::isa<llvm::Constant>(operand)) {
    return false;
  }

  // Finally, rewrite the instructions
  llvm::IRBuilder<> builder(binary_op_instr);

  auto opcode =
      static_cast<llvm::Instruction::BinaryOps>(binary_op_instr->getOpcode());

  auto new_true_value = builder.CreateBinOp(opcode, true_value, operand);
  CopyMetadataTo(binary_op_instr, new_true_value);
  auto new_false_value = builder.CreateBinOp(opcode, false_value, operand);
  CopyMetadataTo(binary_op_instr, new_false_value);

  auto replacement =
      builder.CreateSelect(condition, new_true_value, new_false_value);
  CopyMetadataTo(select_instr, replacement);

  output = llvm::dyn_cast<llvm::Instruction>(replacement);
  return true;
}

bool InstructionFolderPass::PassFunctionState::FoldPHINodeWithBinaryOp(
    llvm::Instruction *&output, llvm::Instruction *phi_node,
    InstructionFolderPass::IncomingValueList &incoming_values,
    llvm::Instruction *binary_op_instr) {

  // This binary operator is using our `PHINode` instruction. Take the
  // operand on the opposite side
  auto operand = binary_op_instr->getOperand(0U);
  if (operand == phi_node) {
    operand = binary_op_instr->getOperand(1U);
  }

  // In order to be able to combine these two instructions, the
  // other operand needs to be a constant
  if (!llvm::isa<llvm::Constant>(operand)) {
    return false;
  }

  // Get the binary operator opcode for later
  auto opcode =
      llvm::dyn_cast<llvm::BinaryOperator>(binary_op_instr)->getOpcode();

  // Go through each incoming value, and try to push the binary operator
  // on the other side of the PHI node
  IncomingValueList new_incoming_values;

  for (auto &incoming_value : incoming_values) {

    // Set the builder inside the incoming block
    llvm::IRBuilder<> builder(incoming_value.basic_block->getTerminator());

    IncomingValue new_incoming_value;
    new_incoming_value.value =
        builder.CreateBinOp(opcode, incoming_value.value, operand);
    CopyMetadataTo(binary_op_instr, new_incoming_value.value);

    new_incoming_value.basic_block = incoming_value.basic_block;
    new_incoming_values.push_back(std::move(new_incoming_value));
  }

  // Move back to the current block, then rewrite the phi node
  llvm::IRBuilder<> builder(phi_node);

  auto new_phi_node =
      builder.CreatePHI(phi_node->getType(), new_incoming_values.size());
  new_phi_node->copyMetadata(*phi_node);

  for (auto &new_incoming_value : new_incoming_values) {
    new_phi_node->addIncoming(new_incoming_value.value,
                              new_incoming_value.basic_block);
  }

  DCHECK(BasicBlockIsSane(phi_node));

  output = llvm::dyn_cast<llvm::Instruction>(new_phi_node);
  return true;
}

bool InstructionFolderPass::PassFunctionState::FoldSelectWithCastInst(
    llvm::Instruction *&output, llvm::Instruction *select_instr,
    llvm::Value *condition, llvm::Value *true_value, llvm::Value *false_value,
    llvm::Instruction *cast_instr) {

  llvm::Instruction::CastOps cast_opcode{};
  llvm::Type *destination_type{};

  {
    auto instr = llvm::dyn_cast<llvm::CastInst>(cast_instr);

    cast_opcode = instr->getOpcode();
    destination_type = instr->getDestTy();
  }

  // For vector select check if the condition element type is correct
  // and the number of elements in the condition type matches if the
  // destination type.
  auto should_fold = [](llvm::Type *condition_type,
                        llvm::Type *destination_type) {
    if (auto cond_vt = llvm::dyn_cast<llvm::VectorType>(condition_type)) {
      if (cond_vt->getElementType() !=
          llvm::Type::getInt1Ty(condition_type->getContext())) {
        return false;
      }

      if (auto dest_vt = llvm::dyn_cast<llvm::VectorType>(destination_type)) {
        if (dest_vt->getElementCount() == cond_vt->getElementCount()) {
          return true;
        }
      }
      return false;
    }

    return true;
  };

  if (!should_fold(condition->getType(), destination_type)) {
    return false;
  }

  llvm::IRBuilder<> builder(cast_instr);

  auto new_true_value =
      builder.CreateCast(cast_opcode, true_value, destination_type);
  CopyMetadataTo(cast_instr, new_true_value);

  auto new_false_value =
      builder.CreateCast(cast_opcode, false_value, destination_type);
  CopyMetadataTo(cast_instr, new_false_value);


  auto replacement =
      builder.CreateSelect(condition, new_true_value, new_false_value);
  CopyMetadataTo(select_instr, replacement);

  output = llvm::dyn_cast<llvm::Instruction>(replacement);
  return true;
}

bool InstructionFolderPass::PassFunctionState::FoldPHINodeWithCastInst(
    llvm::Instruction *&output, llvm::Instruction *phi_node,
    InstructionFolderPass::IncomingValueList &incoming_values,
    llvm::Instruction *cast_instr) {

  llvm::Instruction::CastOps cast_opcode{};
  llvm::Type *destination_type{};

  {
    auto instr = llvm::dyn_cast<llvm::CastInst>(cast_instr);

    cast_opcode = instr->getOpcode();
    destination_type = instr->getDestTy();
  }

  // Go through each incoming value, and try to push the cast operator
  // on the other side of the PHI node
  IncomingValueList new_incoming_values;

  for (auto &incoming_value : incoming_values) {

    // Set the builder inside the incoming block
    llvm::IRBuilder<> builder(incoming_value.basic_block->getTerminator());

    IncomingValue new_incoming_value;
    new_incoming_value.value =
        builder.CreateCast(cast_opcode, incoming_value.value, destination_type);
    CopyMetadataTo(cast_instr, new_incoming_value.value);

    new_incoming_value.basic_block = incoming_value.basic_block;
    new_incoming_values.push_back(std::move(new_incoming_value));
  }

  // Move back to the current block, then rewrite the phi node; since
  // we changed the type on the other side, make sure to update it here
  // as well
  llvm::IRBuilder<> builder(phi_node);

  auto new_phi_node =
      builder.CreatePHI(destination_type, new_incoming_values.size());
  new_phi_node->copyMetadata(*phi_node);

  for (auto &new_incoming_value : new_incoming_values) {
    new_phi_node->addIncoming(new_incoming_value.value,
                              new_incoming_value.basic_block);
  }

  DCHECK(BasicBlockIsSane(phi_node));

  output = llvm::dyn_cast<llvm::Instruction>(new_phi_node);
  return true;
}


bool InstructionFolderPass::PassFunctionState::FoldSelectWithGEPInst(
    llvm::Instruction *&output, llvm::Instruction *select_instr,
    llvm::Value *condition, llvm::Value *true_value, llvm::Value *false_value,
    llvm::Instruction *gep_instr) {

  std::vector<llvm::Value *> index_list;
  if (!CollectAndValidateGEPIndexes(index_list, select_instr, gep_instr)) {
    return false;
  }

  llvm::Value *new_true_value = nullptr;
  llvm::Value *new_false_value = nullptr;
  llvm::IRBuilder<> builder(gep_instr);

  for (auto &index : index_list) {
    if (index == select_instr) {
      index = true_value;
      new_true_value = builder.CreateGEP(gep_instr->getOperand(0), index_list);
      CopyMetadataTo(gep_instr, new_true_value);
      index = false_value;
      new_false_value = builder.CreateGEP(gep_instr->getOperand(0), index_list);
      CopyMetadataTo(gep_instr, new_false_value);
      break;
    }
  }

  if (!new_true_value) {
    return false;
  }

  auto replacement =
      builder.CreateSelect(condition, new_true_value, new_false_value);
  CopyMetadataTo(select_instr, replacement);

  output = llvm::dyn_cast<llvm::Instruction>(replacement);
  return true;
}

bool InstructionFolderPass::PassFunctionState::FoldPHINodeWithGEPInst(
    llvm::Instruction *&output, llvm::Instruction *phi_node,
    InstructionFolderPass::IncomingValueList &incoming_values,
    llvm::Instruction *gep_instr) {

  std::vector<llvm::Value *> index_list;
  if (!CollectAndValidateGEPIndexes(index_list, phi_node, gep_instr)) {
    return false;
  }

  // If the GEP instruction is in a different block than the PHI node then
  // we can't replace the GEP with a PHI of a bunch of GEPs because then
  // we can't prove that this new PHI dominates all uses of the GEP.
  llvm::BasicBlock *const curr_block = phi_node->getParent();
  if (gep_instr->getParent() != curr_block) {
    return false;
  }

  const auto base_ptr = gep_instr->getOperand(0);

  std::unordered_map<llvm::Value *,
                     std::unordered_map<llvm::BasicBlock *, llvm::Value *>>
      value_map;

  for (llvm::Use &op : gep_instr->operands()) {
    auto v = op.get();
    auto cv = llvm::dyn_cast<llvm::Constant>(v);
    auto av = llvm::dyn_cast<llvm::Argument>(v);

    // Map the constants/arguments as being available in all predecessor blocks.
    if (cv || av) {
      auto &vals = value_map[v];
      for (auto &incoming_val : incoming_values) {
        vals[incoming_val.basic_block] = v;
      }
      continue;
    }

    // Check the dominator tree.
    if (auto iv = llvm::dyn_cast<llvm::Instruction>(v)) {
      if (iv->getParent() == curr_block) {
        continue;  // Don't bother checking.
      }

      auto all_dominated = true;
      for (auto &incoming_val : incoming_values) {
        if (!dt.dominates(iv, incoming_val.basic_block->getTerminator())) {
          all_dominated = false;
          break;
        }
      }

      // If the base pointer/index of this GEP dominates all terminators of
      // the incoming blocks of this PHI, then we can safely use the value in
      // all predecessor blocks just before the terminator.
      if (all_dominated) {
        auto &vals = value_map[v];
        for (auto &incoming_val : incoming_values) {
          vals[incoming_val.basic_block] = v;
        }
      }
    }
  }

  // The dominator tree analysis above will fail for PHI nodes in this block.
  // It's possible that a GEP node is of the form:
  //
  //      %base = phi [base1, pred1], [base2, pred2]
  //      %index = phi [index1, pred1], [index2, pred2]
  //      %gep = gep %base, 0, %index
  //
  // In this case, we want our value map to discover that `index` has a mapped
  // value for each predecessor block, specifically, `index1` and `index2`.
  for (auto &inst : *curr_block) {
    if (auto phi_in_block = llvm::dyn_cast<llvm::PHINode>(&inst)) {
      auto &vals = value_map[phi_in_block];
      auto num_incoming_blocks = phi_in_block->getNumIncomingValues();
      for (auto i = 0u; i < num_incoming_blocks; ++i) {
        auto iv = phi_in_block->getIncomingValue(i);
        auto ib = phi_in_block->getIncomingBlock(i);
        vals[ib] = iv;
      }
    } else {
      break;
    }
  }

  // Now check that we can hoist this GEP out. This requires that we have
  // something in the value map for all operands of the GEP.
  for (llvm::Use &op : gep_instr->operands()) {
    llvm::Value *const v = op.get();
    if (value_map.find(v) == value_map.end()) {
      return false;
    }
  }

  // Go through each incoming value and move the `GetElementPtrInst`
  // instruction on each incoming basic block
  IncomingValueList new_incoming_values;

  std::vector<llvm::Value *> mapped_index_list;
  mapped_index_list.reserve(index_list.size());

  for (auto &incoming_value : incoming_values) {

    // Apply the value map to the index list.
    mapped_index_list.clear();
    for (auto iv : index_list) {
      mapped_index_list.push_back(value_map[iv][incoming_value.basic_block]);
    }

    // Set the builder inside the incoming block
    llvm::IRBuilder<> builder(incoming_value.basic_block->getTerminator());

    IncomingValue new_incoming_value;
    new_incoming_value.basic_block = incoming_value.basic_block;
    new_incoming_value.value = builder.CreateGEP(
        value_map[base_ptr][incoming_value.basic_block], mapped_index_list);
    CopyMetadataTo(gep_instr, new_incoming_value.value);

    new_incoming_values.push_back(std::move(new_incoming_value));
  }

  // Move back to the current block, then rewrite the phi node; on this
  // side, we have to use the type returned by the GEP instruction
  llvm::IRBuilder<> builder(phi_node);

  auto new_phi_node =
      builder.CreatePHI(gep_instr->getType(), new_incoming_values.size());
  new_phi_node->copyMetadata(*phi_node);

  for (auto &new_incoming_value : new_incoming_values) {
    new_phi_node->addIncoming(new_incoming_value.value,
                              new_incoming_value.basic_block);
  }

  output = llvm::dyn_cast<llvm::Instruction>(new_phi_node);
  return true;
}


bool InstructionFolderPass::PassFunctionState::FoldPHINode(
    InstructionFolderPass::InstructionList &output, llvm::Instruction *instr) {

  // Extract the incoming values
  IncomingValueList incoming_value_list;

  {

    // FoldPHINode is automatically called for PHINode types
    // (see kInstructionFolderMap)
    auto phi_node = llvm::dyn_cast<llvm::PHINode>(instr);

    auto incoming_value_count = phi_node->getNumIncomingValues();
    for (auto i = 0U; i < incoming_value_count; ++i) {
      IncomingValue incoming_value;

      incoming_value.value = phi_node->getIncomingValue(i);
      incoming_value.basic_block = phi_node->getIncomingBlock(i);

      incoming_value_list.push_back(std::move(incoming_value));
    }
  }

  if (!IsPHINodeFoldable(instr, incoming_value_list)) {
    return false;
  }

  // Postpone the replacement and cleanup at the end of the
  // rewrite to avoid possible issues with PHI nodes or
  // iterator invalidation
  InstructionReplacementList inst_replacement_list;

  // Go through all the users of this `phi` instruction
  for (auto user : instr->users()) {
    InstructionReplacement repl;
    repl.original_instr = llvm::dyn_cast<llvm::Instruction>(user);

    // Search for a function that knows how to handle this case
    auto instr_folder_it =
        kPHINodeFolderMap.find(repl.original_instr->getOpcode());
    if (instr_folder_it == kPHINodeFolderMap.end()) {
      continue;
    }

    const auto instr_folder = instr_folder_it->second;
    if (!(this->*instr_folder)(repl.replacement_instr, instr,
                               incoming_value_list, repl.original_instr)) {
      continue;
    }

    output.push_back(repl.replacement_instr);
    inst_replacement_list.push_back(std::move(repl));
  }

  // Finally, drop all the instructions we no longer need; the `PHINode`
  // instruction will be deleted later by the caller
  auto function_changed = !inst_replacement_list.empty();
  PerformInstructionReplacements(inst_replacement_list);

  for (auto &incoming_value : incoming_value_list) {
    auto value_as_instr =
        llvm::dyn_cast<llvm::Instruction>(incoming_value.value);

    if (value_as_instr == nullptr) {
      continue;
    }

    if (value_as_instr->use_empty()) {
      value_as_instr->eraseFromParent();
    }
  }

  return function_changed;
}

bool InstructionFolderPass::PassFunctionState::FoldSelectInstruction(
    InstructionFolderPass::InstructionList &output, llvm::Instruction *instr) {

  // Extract the condition and the two operands for later
  llvm::Value *condition{nullptr};
  llvm::Value *true_value{nullptr};
  llvm::Value *false_value{nullptr};

  {

    // FoldSelectInstruction is automatically called for SelectInst
    // types (see kInstructionFolderMap)
    auto select_instr = llvm::dyn_cast<llvm::SelectInst>(instr);

    condition = select_instr->getCondition();
    true_value = select_instr->getTrueValue();
    false_value = select_instr->getFalseValue();
  }

  // Postpone the replacement and cleanup at the end of the
  // rewrite to avoid possible issues with PHI nodes or
  // iterator invalidation
  InstructionReplacementList inst_replacement_list;

  // Go through all the users of this `select` instruction
  for (llvm::User *user : instr->users()) {
    InstructionReplacement repl;
    repl.original_instr = llvm::dyn_cast<llvm::Instruction>(user);

    // Search for a function that knows how to handle this case
    auto instr_folder_it =
        kSelectInstructionFolderMap.find(repl.original_instr->getOpcode());

    if (instr_folder_it == kSelectInstructionFolderMap.end()) {
      continue;
    }

    const auto &instr_folder = instr_folder_it->second;
    if (!instr_folder(repl.replacement_instr, instr, condition, true_value,
                      false_value, repl.original_instr)) {
      continue;
    }

    output.push_back(repl.replacement_instr);
    inst_replacement_list.push_back(std::move(repl));
  }

  // Finally, drop all the instructions we no longer need; the `SelectInst`
  // instruction will be deleted later by the caller
  auto function_changed = !inst_replacement_list.empty();
  PerformInstructionReplacements(inst_replacement_list);

  return function_changed;
}

void AddInstructionFolderPass(llvm::FunctionPassManager &fpm) {
  fpm.addPass(InstructionFolderPass());
}
}  // namespace anvill
