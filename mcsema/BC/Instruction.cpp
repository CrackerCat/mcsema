/*
 * Copyright (c) 2020 Trail of Bits, Inc.
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

#include "Instruction.h"

#include <glog/logging.h>

#include <sstream>
#include <string>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include "remill/Arch/Arch.h"
#include "remill/Arch/Instruction.h"
#include "remill/BC/Util.h"

#include "mcsema/Arch/Arch.h"
#include "mcsema/BC/Callback.h"
#include "mcsema/BC/Lift.h"
#include "mcsema/BC/Util.h"
#include "mcsema/CFG/CFG.h"


namespace mcsema {

InstructionLifter::~InstructionLifter(void) {}

InstructionLifter::InstructionLifter(const remill::IntrinsicTable *intrinsics_,
                                     TranslationContext &ctx_)
      : remill::InstructionLifter(gArch.get(), intrinsics_),
        ctx(ctx_) {}

// Lift a single instruction into a basic block.
remill::LiftStatus InstructionLifter::LiftIntoBlock(
    remill::Instruction &inst, llvm::BasicBlock *block_, bool is_delayed) {

  inst_ptr = &inst;
  block = block_;

  if (ctx.cfg_inst) {
    mem_ref = GetAddress(ctx.cfg_inst->mem);
    imm_ref = GetAddress(ctx.cfg_inst->imm);
    disp_ref = GetAddress(ctx.cfg_inst->disp);
  } else {
    mem_ref = nullptr;
    imm_ref = nullptr;
    disp_ref = nullptr;
  }

  mem_ref_used = false;
  disp_ref_used = false;
  imm_ref_used = false;

  auto status = this->remill::InstructionLifter::LiftIntoBlock(
      inst, block, is_delayed);

  // If we have semantics for the instruction, then make sure that we were
  // able to match cross-reference information to the instruction's operands.
  if (remill::kLiftedInstruction == status) {
    if (mem_ref && !mem_ref_used) {
      LOG(ERROR)
          << "Unused memory reference operand to " << std::hex
          << ctx.cfg_inst->mem->target_ea << " in instruction "
          << inst.Serialize() << std::dec;
    }

    if (imm_ref && !imm_ref_used) {
      LOG(ERROR)
          << "Unused immediate operand reference to " << std::hex
          << ctx.cfg_inst->imm->target_ea << " in instruction "
          << inst.Serialize() << std::dec;
    }

    if (disp_ref && !disp_ref_used) {
      LOG(ERROR)
          << "Unused displacement operand reference to " << std::hex
          << ctx.cfg_inst->disp->target_ea << " in instruction "
          << inst.Serialize() << std::dec;
    }
  }

  return status;
}

//// Returns `true` if a given cross-reference is self-referential. That is,
//// we'll have something like `jmp cs:EnterCriticalSection`, which references
//// `EnterCriticalSection` in the `.idata` section of a PE file. But this
//// location is our only "place" for the external `EnterCriticalSection`, so
//// we point it back at itself.
//static bool IsSelfReferential(const NativeXref *cfg_xref) {
//  if (!cfg_xref->target_segment) {
//    return false;
//  }
//
//  auto it = cfg_xref->target_segment->entries.find(cfg_xref->target_ea);
//  if (it == cfg_xref->target_segment->entries.end()) {
//    return false;
//  }
//
//  const auto &entry = it->second;
//  if (!entry.xref) {
//    return false;
//  }
//
//  return entry.xref->target_ea == entry.ea;
//}

llvm::Value *InstructionLifter::GetAddress(
    const NativeInstructionXref *cfg_xref) {
  if (!cfg_xref) {
    return nullptr;
  }

  if (cfg_xref->mask) {
    return LiftXrefInCode(cfg_xref->target_ea & cfg_xref->mask);
  } else {
    return LiftXrefInCode(cfg_xref->target_ea);
  }
}

llvm::Value *InstructionLifter::LiftImmediateOperand(
    remill::Instruction &inst, llvm::BasicBlock *block,
    llvm::Argument *arg, remill::Operand &op) {
  auto arg_type = arg->getType();
  if (imm_ref && !imm_ref_used) {
    imm_ref_used = true;

    llvm::DataLayout data_layout(gModule.get());
    auto arg_size = data_layout.getTypeSizeInBits(arg_type);

    CHECK(arg_size <= arch->address_size)
        << "Immediate operand size " << op.size << " of "
        << op.Serialize() << " in instuction " << std::hex << inst.pc
        << " is wider than the architecture pointer size ("
        << std::dec << arch->address_size << ").";

    if (arg_type != imm_ref->getType() && arg_size < arch->address_size) {
      llvm::IRBuilder<> ir(block);
      imm_ref = ir.CreateTrunc(imm_ref, arg_type);
    }

    return imm_ref;

  } else if (op.size == arch->address_size &&
             4096 <= op.imm.val) {
    auto seg = ctx.cfg_module->TryGetSegment(op.imm.val);
    LOG_IF(WARNING, seg != nullptr)
        << "Immediate operand '" << op.Serialize()
        << "' of instruction " << inst.Serialize()
        << " is a missed cross-reference candidate";
  }

  return this->remill::InstructionLifter::LiftImmediateOperand(
      inst, block, arg, op);
}

// Lift an indirect memory operand to a value.
llvm::Value *InstructionLifter::LiftAddressOperand(
    remill::Instruction &inst, llvm::BasicBlock *block,
    llvm::Argument *arg, remill::Operand &op) {

  auto &mem = op.addr;

  // A higher layer will resolve any code refs; this is a static address and
  // we want to preserve it in the register state structure.
  if (mem.IsControlFlowTarget()) {
    return this->remill::InstructionLifter::LiftAddressOperand(
        inst, block, arg, op);
  }

  if ((mem.base_reg.name.empty() && mem.index_reg.name.empty()) ||
      (mem.base_reg.name == "PC" && mem.index_reg.name.empty())) {

    if (mem_ref) {
      mem_ref_used = true;
      return mem_ref;

    } else if (disp_ref) {
      disp_ref_used = true;
      return disp_ref;
    }

  } else if ((mem.base_reg.name.empty() && mem.index_reg.name.empty()) ||
             (mem.base_reg.name == "NEXT_PC" && mem.index_reg.name.empty())) {

    if (mem_ref) {
      mem_ref_used = true;
      return mem_ref;

    } else if (disp_ref) {
      disp_ref_used = true;
      return disp_ref;
    }

  } else {

    // It's a reference located in the displacement. We'll clear out the
    // displacement, calculate the address operand stuff, then add the address
    // of the external back in. E.g. `mov rax, [extern_jump_table + rdi]`.
    if (disp_ref) {
      disp_ref_used = true;
      mem.displacement = 0;
      auto dynamic_addr = this->remill::InstructionLifter::LiftAddressOperand(
          inst, block, arg, op);
      llvm::IRBuilder<> ir(block);
      return ir.CreateAdd(dynamic_addr, disp_ref);

    } else if (mem_ref && (static_cast<uint64_t>(op.addr.displacement) ==
                           ctx.cfg_inst->mem->target_ea)) {
      LOG(ERROR)
          << "IDA probably incorrectly decoded memory operand "
          << op.Serialize() << " of instruction " << std::hex << inst.pc
          << " as an absolute memory reference when it should be treated as a "
          << "displacement memory reference.";
      mem_ref_used = true;
      mem.displacement = 0;
      auto dynamic_addr = this->remill::InstructionLifter::LiftAddressOperand(
          inst, block, arg, op);
      llvm::IRBuilder<> ir(block);
      return ir.CreateAdd(dynamic_addr, mem_ref);
    }
  }

  return this->remill::InstructionLifter::LiftAddressOperand(
      inst, block, arg, op);
}

}  // namespace mcsema
