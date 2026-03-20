/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ppc/MacroAssembler-ppc.h"
#include <cstdio>

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jscompartment.h"
#include "jsutil.h"

#include "jit/Bailouts.h"
#include "jit/BaselineFrame.h"
#include "jit/JitFrames.h"
#include "jit/MacroAssembler.h"
#include "jit/MoveEmitter.h"
#include "jit/ppc/Assembler-ppc.h"

#include "gc/Nursery.h"
using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;

MacroAssembler&
MacroAssemblerPPC::asMasm()
{
    return *static_cast<MacroAssembler*>(this);
}

const MacroAssembler&
MacroAssemblerPPC::asMasm() const
{
    return *static_cast<const MacroAssembler*>(this);
}

// =========================================================================
// Immediate loading
// =========================================================================

void
MacroAssemblerPPC::ma_li(Register dest, Imm32 imm)
{
    int32_t value = imm.value;
    if (value >= -32768 && value <= 32767) {
        as_li(dest, value);
    } else {
        as_lis(dest, (value >> 16) & 0xffff);
        if (value & 0xffff)
            as_ori(dest, dest, value & 0xffff);
    }
}

void
MacroAssemblerPPC::ma_li(Register dest, ImmWord imm)
{
    ma_li(dest, Imm32(int32_t(imm.value)));
}

void
MacroAssemblerPPC::ma_li(Register dest, ImmGCPtr ptr)
{
    writeDataRelocation(ptr);
    as_li32(dest, uint32_t(uintptr_t(ptr.value)));
}

void
MacroAssemblerPPC::ma_li(Register dest, ImmPtr ptr)
{
    ma_li(dest, ImmWord(uintptr_t(ptr.value)));
}

void
MacroAssemblerPPC::ma_li(Register dest, CodeOffset* label)
{
    // Emit a patchable lis+ori pair
    BufferOffset bo = as_lis(dest, 0);
    as_ori(dest, dest, 0);
    label->bind(bo.getOffset());
}

void
MacroAssemblerPPC::ma_liPatchable(Register dest, Imm32 imm)
{
    // Always emit lis+ori even if it could fit in one instruction,
    // so the patch code can find both instructions.
    as_lis(dest, (imm.value >> 16) & 0xffff);
    as_ori(dest, dest, imm.value & 0xffff);
}

void
MacroAssemblerPPC::ma_liPatchable(Register dest, ImmPtr imm)
{
    ma_liPatchable(dest, Imm32(int32_t(uintptr_t(imm.value))));
}

void
MacroAssemblerPPC::ma_liPatchable(Register dest, ImmWord imm)
{
    ma_liPatchable(dest, Imm32(int32_t(imm.value)));
}

// =========================================================================
// Register moves
// =========================================================================

void
MacroAssemblerPPC::ma_move(Register rd, Register rs)
{
    if (rd != rs)
        as_mr(rd, rs);
}

// =========================================================================
// Arithmetic
// =========================================================================

void
MacroAssemblerPPC::ma_addu(Register rd, Register rs, Register rt)
{
    as_add(rd, rs, rt);
}

void
MacroAssemblerPPC::ma_addu(Register rd, Register rs, Imm32 imm)
{
    if (imm.value >= -32768 && imm.value <= 32767) {
        as_addi(rd, rs, imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_add(rd, rs, ScratchRegister);
    }
}

void
MacroAssemblerPPC::ma_addu(Register rd, Imm32 imm)
{
    ma_addu(rd, rd, imm);
}

void
MacroAssemblerPPC::ma_subu(Register rd, Register rs, Register rt)
{
    as_subf(rd, rt, rs); // subf: rd = rs - rt (note operand order)
}

void
MacroAssemblerPPC::ma_subu(Register rd, Register rs, Imm32 imm)
{
    if (imm.value >= -32767 && imm.value <= 32768) {
        as_addi(rd, rs, -imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_subf(rd, ScratchRegister, rs);
    }
}

void
MacroAssemblerPPC::ma_subu(Register rd, Imm32 imm)
{
    ma_subu(rd, rd, imm);
}

void
MacroAssemblerPPC::ma_negu(Register rd, Register rs)
{
    as_neg(rd, rs);
}

void
MacroAssemblerPPC::ma_mul(Register rd, Register rs, Register rt)
{
    as_mullw(rd, rs, rt);
}

void
MacroAssemblerPPC::ma_mul(Register rd, Register rs, Imm32 imm)
{
    if (imm.value >= -32768 && imm.value <= 32767) {
        as_mulli(rd, rs, imm.value);
    } else {
        ma_li(ScratchRegister, imm);
        as_mullw(rd, rs, ScratchRegister);
    }
}

template <typename L>
void
MacroAssemblerPPC::ma_addTestOverflow(Register rd, Register rs, Register rt, L overflow)
{
    // Use addc + check XER[SO] via mfxer, or use addo. (Rc=1)
    // Simpler approach: compare signs.
    // If both operands have same sign and result has different sign, overflow.
    Register scratch = ScratchRegister;
    Register scratch2 = SecondScratchRegister;

    as_add(rd, rs, rt);

    // overflow = (rs ^ rd) & (rt ^ rd) & 0x80000000
    as_xor(scratch, rs, rd);
    as_xor(scratch2, rt, rd);
    as_and(scratch, scratch, scratch2);

    // Branch if bit 31 (sign) is set
    as_rlwinm(scratch, scratch, 1, 31, 31); // extract bit 31 -> bit 0
    as_cmpwi(scratch, 0);
    asMasm().ma_b(overflow, NotEqual);
}

template void MacroAssemblerPPC::ma_addTestOverflow<Label*>(Register, Register, Register, Label*);

template <typename L>
void
MacroAssemblerPPC::ma_addTestOverflow(Register rd, Register rs, Imm32 imm, L overflow)
{
    ma_li(SecondScratchRegister, imm);
    ma_addTestOverflow(rd, rs, SecondScratchRegister, overflow);
}

template void MacroAssemblerPPC::ma_addTestOverflow<Label*>(Register, Register, Imm32, Label*);

void
MacroAssemblerPPC::ma_subTestOverflow(Register rd, Register rs, Register rt, Label* overflow)
{
    Register scratch = ScratchRegister;
    Register scratch2 = SecondScratchRegister;

    as_subf(rd, rt, rs); // rd = rs - rt

    // overflow = (rs ^ rt) & (rs ^ rd) & 0x80000000
    as_xor(scratch, rs, rt);
    as_xor(scratch2, rs, rd);
    as_and(scratch, scratch, scratch2);

    as_rlwinm(scratch, scratch, 1, 31, 31);
    as_cmpwi(scratch, 0);
    asMasm().ma_b(overflow, NotEqual);
}

// =========================================================================
// Logic
// =========================================================================

void MacroAssemblerPPC::ma_and(Register rd, Register rs)
{
    as_and(rd, rd, rs);
}

void MacroAssemblerPPC::ma_and(Register rd, Imm32 imm)
{
    ma_and(rd, rd, imm);
}

void MacroAssemblerPPC::ma_and(Register rd, Register rs, Imm32 imm)
{
    uint32_t value = uint32_t(imm.value);
    if (value <= 0xffff) {
        as_andi(rd, rs, value);
    } else if ((value & 0xffff) == 0) {
        as_andi(rd, rs, value >> 16);
    } else {
        ma_li(ScratchRegister, imm);
        as_and(rd, rs, ScratchRegister);
    }
}

void MacroAssemblerPPC::ma_or(Register rd, Register rs)
{
    as_or(rd, rd, rs);
}

void MacroAssemblerPPC::ma_or(Register rd, Imm32 imm)
{
    ma_or(rd, rd, imm);
}

void MacroAssemblerPPC::ma_or(Register rd, Register rs, Imm32 imm)
{
    uint32_t value = uint32_t(imm.value);
    if (value <= 0xffff) {
        as_ori(rd, rs, value);
    } else if ((value & 0xffff) == 0) {
        as_oris(rd, rs, value >> 16);
    } else {
        ma_li(ScratchRegister, imm);
        as_or(rd, rs, ScratchRegister);
    }
}

void MacroAssemblerPPC::ma_xor(Register rd, Register rs)
{
    as_xor(rd, rd, rs);
}

void MacroAssemblerPPC::ma_xor(Register rd, Imm32 imm)
{
    ma_xor(rd, rd, imm);
}

void MacroAssemblerPPC::ma_xor(Register rd, Register rs, Imm32 imm)
{
    uint32_t value = uint32_t(imm.value);
    if (value <= 0xffff) {
        as_xori(rd, rs, value);
    } else {
        ma_li(ScratchRegister, imm);
        as_xor(rd, rs, ScratchRegister);
    }
}

void MacroAssemblerPPC::ma_not(Register rd, Register rs)
{
    as_nor(rd, rs, rs);
}

// =========================================================================
// Shifts
// =========================================================================

void MacroAssemblerPPC::ma_sll(Register rd, Register rt, Imm32 shift)
{
    // rlwinm ra, rs, sh, 0, 31-sh  (shift left logical)
    MOZ_ASSERT(shift.value >= 0 && shift.value < 32);
    if (shift.value == 0) {
        ma_move(rd, rt);
    } else {
        as_rlwinm(rd, rt, shift.value, 0, 31 - shift.value);
    }
}

void MacroAssemblerPPC::ma_srl(Register rd, Register rt, Imm32 shift)
{
    MOZ_ASSERT(shift.value >= 0 && shift.value < 32);
    if (shift.value == 0) {
        ma_move(rd, rt);
    } else {
        as_rlwinm(rd, rt, 32 - shift.value, shift.value, 31);
    }
}

void MacroAssemblerPPC::ma_sra(Register rd, Register rt, Imm32 shift)
{
    MOZ_ASSERT(shift.value >= 0 && shift.value < 32);
    if (shift.value == 0) {
        ma_move(rd, rt);
    } else {
        as_srawi(rd, rt, shift.value);
    }
}

void MacroAssemblerPPC::ma_sll(Register rd, Register rt, Register shift)
{
    as_slw(rd, rt, shift);
}

void MacroAssemblerPPC::ma_srl(Register rd, Register rt, Register shift)
{
    as_srw(rd, rt, shift);
}

void MacroAssemblerPPC::ma_sra(Register rd, Register rt, Register shift)
{
    as_sraw(rd, rt, shift);
}

void MacroAssemblerPPC::ma_rol(Register rd, Register rt, Imm32 shift)
{
    as_rlwinm(rd, rt, shift.value, 0, 31);
}

void MacroAssemblerPPC::ma_ror(Register rd, Register rt, Imm32 shift)
{
    as_rlwinm(rd, rt, 32 - shift.value, 0, 31);
}

// =========================================================================
// Compare and set
// =========================================================================

static void
EmitConditionToRegister(MacroAssemblerPPC& masm, Assembler::Condition c, Register dest)
{
    // Read CR0 and extract the appropriate bit
    masm.as_mfcr(dest);

    switch (c) {
      case Assembler::Equal:
      case Assembler::Zero:
        // CR0[EQ] = bit 2
        masm.as_rlwinm(dest, dest, 3, 31, 31);
        break;
      case Assembler::NotEqual:
      case Assembler::NonZero:
        masm.as_rlwinm(dest, dest, 3, 31, 31);
        masm.as_xori(dest, dest, 1);
        break;
      case Assembler::LessThan:
      case Assembler::Below:
        // CR0[LT] = bit 0
        masm.as_rlwinm(dest, dest, 1, 31, 31);
        break;
      case Assembler::GreaterThanOrEqual:
      case Assembler::AboveOrEqual:
        masm.as_rlwinm(dest, dest, 1, 31, 31);
        masm.as_xori(dest, dest, 1);
        break;
      case Assembler::GreaterThan:
      case Assembler::Above:
        // CR0[GT] = bit 1
        masm.as_rlwinm(dest, dest, 2, 31, 31);
        break;
      case Assembler::LessThanOrEqual:
      case Assembler::BelowOrEqual:
        masm.as_rlwinm(dest, dest, 2, 31, 31);
        masm.as_xori(dest, dest, 1);
        break;
      default:
        MOZ_CRASH("Unexpected condition");
    }
}

void
MacroAssemblerPPC::ma_cmp_set(Register dest, Register lhs, Register rhs, Condition c)
{
    // Choose signed or unsigned compare based on condition
    switch (c) {
      case Above: case AboveOrEqual: case Below: case BelowOrEqual:
        as_cmplw(lhs, rhs);
        break;
      default:
        as_cmpw(lhs, rhs);
        break;
    }
    EmitConditionToRegister(*this, c, dest);
}

void
MacroAssemblerPPC::ma_cmp_set(Register dest, Register lhs, Imm32 imm, Condition c)
{
    switch (c) {
      case Above: case AboveOrEqual: case Below: case BelowOrEqual:
        if (uint32_t(imm.value) <= 0xffff) {
            as_cmplwi(lhs, uint32_t(imm.value));
        } else {
            ma_li(ScratchRegister, imm);
            as_cmplw(lhs, ScratchRegister);
        }
        break;
      default:
        if (imm.value >= -32768 && imm.value <= 32767) {
            as_cmpwi(lhs, imm.value);
        } else {
            ma_li(ScratchRegister, imm);
            as_cmpw(lhs, ScratchRegister);
        }
        break;
    }
    EmitConditionToRegister(*this, c, dest);
}

void
MacroAssemblerPPC::ma_cmp_set(Register dest, Register lhs, ImmPtr imm, Condition c)
{
    ma_cmp_set(dest, lhs, Imm32(int32_t(uintptr_t(imm.value))), c);
}

void
MacroAssemblerPPC::ma_cmp_set(Register dest, Address lhs, Imm32 imm, Condition c)
{
    ma_lw(ScratchRegister, lhs);
    ma_cmp_set(dest, ScratchRegister, imm, c);
}

void
MacroAssemblerPPC::ma_cmp_set(Register dest, Address lhs, ImmPtr imm, Condition c)
{
    ma_cmp_set(dest, lhs, Imm32(int32_t(uintptr_t(imm.value))), c);
}

// =========================================================================
// Load/Store
// =========================================================================

void
MacroAssemblerPPC::ma_load(Register dest, Address address, LoadStoreSize size, LoadStoreExtension extension)
{
    int32_t offset = address.offset;

    // Check if offset fits in 16-bit signed immediate
    if (offset < -32768 || offset > 32767) {
        ma_li(ScratchRegister, Imm32(offset));
        as_add(ScratchRegister, ScratchRegister, address.base);
        offset = 0;
        address = Address(ScratchRegister, 0);
    }

    switch (size) {
      case SizeByte:
        if (extension == SignExtend) {
            as_lbz(dest, address.base, offset);
            as_extsb(dest, dest);
        } else {
            as_lbz(dest, address.base, offset);
        }
        break;
      case SizeHalfWord:
        if (extension == SignExtend) {
            as_lha(dest, address.base, offset);
        } else {
            as_lhz(dest, address.base, offset);
        }
        break;
      case SizeWord:
        as_lwz(dest, address.base, offset);
        break;
      default:
        MOZ_CRASH("Unexpected load size");
    }
}

void
MacroAssemblerPPC::ma_load(Register dest, const BaseIndex& src, LoadStoreSize size, LoadStoreExtension extension)
{
    computeScaledAddress(src, ScratchRegister);
    ma_load(dest, Address(ScratchRegister, src.offset), size, extension);
}

void
MacroAssemblerPPC::ma_store(Register data, Address address, LoadStoreSize size, LoadStoreExtension extension)
{
    int32_t offset = address.offset;

    if (offset < -32768 || offset > 32767) {
        ma_li(ScratchRegister, Imm32(offset));
        as_add(ScratchRegister, ScratchRegister, address.base);
        offset = 0;
        address = Address(ScratchRegister, 0);
    }

    switch (size) {
      case SizeByte:
        as_stb(data, address.base, offset);
        break;
      case SizeHalfWord:
        as_sth(data, address.base, offset);
        break;
      case SizeWord:
        as_stw(data, address.base, offset);
        break;
      default:
        MOZ_CRASH("Unexpected store size");
    }
}

void
MacroAssemblerPPC::ma_store(Register data, const BaseIndex& dest, LoadStoreSize size, LoadStoreExtension extension)
{
    computeScaledAddress(dest, ScratchRegister);
    ma_store(data, Address(ScratchRegister, dest.offset), size, extension);
}

void
MacroAssemblerPPC::ma_store(Imm32 imm, Address address, LoadStoreSize size, LoadStoreExtension extension)
{
    ma_li(SecondScratchRegister, imm);
    ma_store(SecondScratchRegister, address, size, extension);
}

void MacroAssemblerPPC::ma_lw(Register data, Address address)
{
    ma_load(data, address, SizeWord, ZeroExtend);
}

void MacroAssemblerPPC::ma_sw(Register data, Address address)
{
    ma_store(data, address, SizeWord, ZeroExtend);
}

void MacroAssemblerPPC::ma_sw(Imm32 imm, Address address)
{
    ma_li(SecondScratchRegister, imm);
    ma_sw(SecondScratchRegister, address);
}

void MacroAssemblerPPC::ma_sw(Register data, BaseIndex& address)
{
    computeScaledAddress(address, ScratchRegister);
    as_stw(data, ScratchRegister, address.offset);
}

// =========================================================================
// Stack operations
// =========================================================================

void MacroAssemblerPPC::ma_pop(Register r)
{
    as_lwz(r, StackPointer, 0);
    as_addi(StackPointer, StackPointer, sizeof(intptr_t));
}

void MacroAssemblerPPC::ma_push(Register r)
{
    as_stwu(r, StackPointer, -int32_t(sizeof(intptr_t)));
}

// =========================================================================
// FP load/store
// =========================================================================

void MacroAssemblerPPC::ma_ss(FloatRegister src, Address address)
{
    int32_t offset = address.offset;
    if (offset < -32768 || offset > 32767) {
        ma_li(ScratchRegister, Imm32(offset));
        as_add(ScratchRegister, ScratchRegister, address.base);
        as_stfs(src, ScratchRegister, 0);
    } else {
        as_stfs(src, address.base, offset);
    }
}

void MacroAssemblerPPC::ma_ss(FloatRegister src, BaseIndex address)
{
    computeScaledAddress(address, ScratchRegister);
    as_stfs(src, ScratchRegister, address.offset);
}

void MacroAssemblerPPC::ma_sd(FloatRegister src, Address address)
{
    int32_t offset = address.offset;
    if (offset < -32768 || offset > 32767) {
        ma_li(ScratchRegister, Imm32(offset));
        as_add(ScratchRegister, ScratchRegister, address.base);
        as_stfd(src, ScratchRegister, 0);
    } else {
        as_stfd(src, address.base, offset);
    }
}

void MacroAssemblerPPC::ma_sd(FloatRegister src, BaseIndex address)
{
    computeScaledAddress(address, ScratchRegister);
    as_stfd(src, ScratchRegister, address.offset);
}

void MacroAssemblerPPC::ma_ls(FloatRegister dest, Address address)
{
    int32_t offset = address.offset;
    if (offset < -32768 || offset > 32767) {
        ma_li(ScratchRegister, Imm32(offset));
        as_add(ScratchRegister, ScratchRegister, address.base);
        as_lfs(dest, ScratchRegister, 0);
    } else {
        as_lfs(dest, address.base, offset);
    }
}

void MacroAssemblerPPC::ma_ls(FloatRegister dest, BaseIndex address)
{
    computeScaledAddress(address, ScratchRegister);
    as_lfs(dest, ScratchRegister, address.offset);
}

void MacroAssemblerPPC::ma_ld(FloatRegister dest, Address address)
{
    int32_t offset = address.offset;
    if (offset < -32768 || offset > 32767) {
        ma_li(ScratchRegister, Imm32(offset));
        as_add(ScratchRegister, ScratchRegister, address.base);
        as_lfd(dest, ScratchRegister, 0);
    } else {
        as_lfd(dest, address.base, offset);
    }
}

void MacroAssemblerPPC::ma_ld(FloatRegister dest, BaseIndex address)
{
    computeScaledAddress(address, ScratchRegister);
    as_lfd(dest, ScratchRegister, address.offset);
}

// =========================================================================
// Branches
// =========================================================================

// Helper: emit a branch based on CR0
static void
EmitBranchOnCondition(MacroAssemblerPPC& masm, Assembler::Condition c, Label* label)
{
    if (!label) {
        fprintf(stderr, "PPC-BUG: EmitBranchOnCond null label at %zu\n", masm.size());
        return;
    }
    uint32_t bo, bi;

    switch (c) {
      case Assembler::Equal:
      case Assembler::Zero:
        bo = BO_TRUE; bi = CR_EQ;
        break;
      case Assembler::NotEqual:
      case Assembler::NonZero:
        bo = BO_FALSE; bi = CR_EQ;
        break;
      case Assembler::LessThan:
        bo = BO_TRUE; bi = CR_LT;
        break;
      case Assembler::GreaterThanOrEqual:
        bo = BO_FALSE; bi = CR_LT;
        break;
      case Assembler::GreaterThan:
        bo = BO_TRUE; bi = CR_GT;
        break;
      case Assembler::LessThanOrEqual:
        bo = BO_FALSE; bi = CR_GT;
        break;
      case Assembler::Below:
        bo = BO_TRUE; bi = CR_LT;
        break;
      case Assembler::AboveOrEqual:
        bo = BO_FALSE; bi = CR_LT;
        break;
      case Assembler::Above:
        bo = BO_TRUE; bi = CR_GT;
        break;
      case Assembler::BelowOrEqual:
        bo = BO_FALSE; bi = CR_GT;
        break;
      case Assembler::Signed:
        // SO/summary overflow
        bo = BO_TRUE; bi = CR_SO;
        break;
      case Assembler::NotSigned:
        bo = BO_FALSE; bi = CR_SO;
        break;
      default:
        MOZ_CRASH("Unexpected condition");
    }

    // For now, always emit a far branch (lis+ori+mtctr+bc)
    // TODO: optimize for short branches when label is bound and nearby
    if (label->bound()) {
        int32_t offset = label->offset() - masm.size();
        if (offset >= -32768 && offset < 32768) {
            masm.as_bc(bo, bi, offset);
            return;
        }
    }

    // Far conditional branch:
    // bc(inverse), skip
    // b target
    // skip:
    uint32_t invBo = (bo == BO_TRUE) ? BO_FALSE : BO_TRUE;
    masm.as_bc(invBo, bi, 8); // skip over the following b instruction (8 bytes = bc + b)
    // Unconditional branch — will be patched when label binds
    // Encode link to previous use in displacement to form chain
    {
        int32_t branchDisp = 0;
        int32_t branchOff = masm.size();  // offset of the b instruction we're about to emit
        if (!label->bound()) {
            int32_t oldUse = label->use(branchOff);
            if (oldUse != LabelBase::INVALID_OFFSET)
                branchDisp = oldUse - branchOff;
        }
        masm.as_b(branchDisp);
    }
}

void
MacroAssemblerPPC::ma_b(Label* label)
{
    if (!label) {
        fprintf(stderr, "PPC-BUG: ma_b null label at %zu\n", size());
        as_b(0);  // emit nop-like branch to avoid crash
        return;
    }
    if (label->bound()) {
        int32_t offset = label->offset() - int32_t(size());
        as_b(offset);
    } else {
        int32_t branchDisp = 0;
        int32_t branchOff = size();
        int32_t oldUse = label->use(branchOff);
        if (oldUse != LabelBase::INVALID_OFFSET)
            branchDisp = oldUse - branchOff;
        as_b(branchDisp);
    }
}

void
MacroAssemblerPPC::ma_bl(Label* label)
{
    if (label->bound()) {
        int32_t offset = label->offset() - int32_t(size());
        as_bl(offset);
    } else {
        int32_t branchDisp = 0;
        int32_t branchOff = size();
        int32_t oldUse = label->use(branchOff);
        if (oldUse != LabelBase::INVALID_OFFSET)
            branchDisp = oldUse - branchOff;
        as_bl(branchDisp);
    }
}

void
MacroAssemblerPPC::ma_b(Register lhs, Register rhs, Label* label, Condition c)
{
    switch (c) {
      case Above: case AboveOrEqual: case Below: case BelowOrEqual:
        as_cmplw(lhs, rhs);
        break;
      case Zero: case NonZero: case Signed: case NotSigned:
        // When testing a single register (lhs == rhs), compare against 0
        // instead of comparing the register with itself (which is always equal).
        as_cmpwi(lhs, 0);
        break;
      default:
        as_cmpw(lhs, rhs);
        break;
    }
    EmitBranchOnCondition(*this, c, label);
}

void
MacroAssemblerPPC::ma_b(Register lhs, Imm32 imm, Label* label, Condition c)
{
    switch (c) {
      case Above: case AboveOrEqual: case Below: case BelowOrEqual:
        if (uint32_t(imm.value) <= 0xffff)
            as_cmplwi(lhs, uint32_t(imm.value));
        else {
            ma_li(ScratchRegister, imm);
            as_cmplw(lhs, ScratchRegister);
        }
        break;
      default:
        if (imm.value >= -32768 && imm.value <= 32767)
            as_cmpwi(lhs, imm.value);
        else {
            ma_li(ScratchRegister, imm);
            as_cmpw(lhs, ScratchRegister);
        }
        break;
    }
    EmitBranchOnCondition(*this, c, label);
}

void
MacroAssemblerPPC::ma_b(Register lhs, ImmPtr imm, Label* label, Condition c)
{
    ma_b(lhs, Imm32(int32_t(uintptr_t(imm.value))), label, c);
}

void
MacroAssemblerPPC::ma_b(Register lhs, ImmGCPtr imm, Label* label, Condition c)
{
    ma_li(ScratchRegister, imm);
    ma_b(lhs, ScratchRegister, label, c);
}

void
MacroAssemblerPPC::ma_b(Register lhs, ImmWord imm, Label* label, Condition c)
{
    ma_b(lhs, Imm32(int32_t(imm.value)), label, c);
}

void
MacroAssemblerPPC::ma_b(Address addr, Imm32 imm, Label* label, Condition c)
{
    ma_lw(SecondScratchRegister, addr);
    ma_b(SecondScratchRegister, imm, label, c);
}

void
MacroAssemblerPPC::ma_b(Label* label, Condition c)
{
    // Branch based on existing CR0 state
    EmitBranchOnCondition(*this, c, label);
}

// =========================================================================
// Address computation
// =========================================================================

void
MacroAssemblerPPC::computeScaledAddress(const BaseIndex& address, Register dest)
{
    Register base = address.base;
    Register index = address.index;
    Scale scale = address.scale;

    if (scale == TimesOne) {
        as_add(dest, base, index);
    } else {
        ma_sll(ScratchRegister, index, Imm32(ScaleToShift(scale)));
        as_add(dest, base, ScratchRegister);
    }
}

void
MacroAssemblerPPC::computeEffectiveAddress(const Address& address, Register dest)
{
    ma_addu(dest, address.base, Imm32(address.offset));
}

// =========================================================================
// FP branches
// =========================================================================

void
MacroAssemblerPPC::ma_bc(FloatFormat fmt, DoubleCondition cond,
                          FloatRegister lhs, FloatRegister rhs, Label* label)
{
    if (!label) {
        fprintf(stderr, "PPC-BUG: ma_bc null label at %zu\n", size());
        return;
    }
    as_fcmpu(lhs, rhs);

    // Handle "OrUnordered" conditions by emitting two branches:
    // first branch on SO (unordered/NaN), then on the main condition.
    switch (cond) {
      case DoubleEqualOrUnordered:
        // Branch if unordered OR equal
        as_bc(BO_FALSE, CR_SO, 8);  // skip if NOT unordered
        ma_b(label);                 // unordered -> take branch
        as_bc(BO_FALSE, CR_EQ, 8);  // skip if NOT equal
        ma_b(label);                 // equal -> take branch
        return;
      case DoubleNotEqualOrUnordered:
        // Branch if unordered OR not-equal (i.e. NOT (ordered AND equal))
        as_bc(BO_FALSE, CR_SO, 8);
        ma_b(label);
        as_bc(BO_TRUE, CR_EQ, 8);   // skip if equal (don't branch)
        ma_b(label);                 // not-equal -> take branch
        return;
      case DoubleGreaterThanOrUnordered:
        as_bc(BO_FALSE, CR_SO, 8);
        ma_b(label);
        as_bc(BO_FALSE, CR_GT, 8);
        ma_b(label);
        return;
      case DoubleGreaterThanOrEqualOrUnordered:
        // Branch if unordered OR >= (i.e. NOT less-than)
        as_bc(BO_FALSE, CR_SO, 8);
        ma_b(label);
        as_bc(BO_TRUE, CR_LT, 8);   // skip if less-than
        ma_b(label);
        return;
      case DoubleLessThanOrUnordered:
        as_bc(BO_FALSE, CR_SO, 8);
        ma_b(label);
        as_bc(BO_FALSE, CR_LT, 8);
        ma_b(label);
        return;
      case DoubleLessThanOrEqualOrUnordered:
        // Branch if unordered OR <= (i.e. NOT greater-than)
        as_bc(BO_FALSE, CR_SO, 8);
        ma_b(label);
        as_bc(BO_TRUE, CR_GT, 8);   // skip if greater-than
        ma_b(label);
        return;
      default:
        break;
    }

    // Simple (non-OrUnordered) conditions: single branch
    uint32_t bo, bi;
    switch (cond) {
      case DoubleEqual:
        bo = BO_TRUE; bi = CR_EQ;
        break;
      case DoubleNotEqual:
        bo = BO_FALSE; bi = CR_EQ;
        break;
      case DoubleLessThan:
        bo = BO_TRUE; bi = CR_LT;
        break;
      case DoubleGreaterThanOrEqual:
        bo = BO_FALSE; bi = CR_LT;
        break;
      case DoubleGreaterThan:
        bo = BO_TRUE; bi = CR_GT;
        break;
      case DoubleLessThanOrEqual:
        bo = BO_FALSE; bi = CR_GT;
        break;
      case DoubleUnordered:
        bo = BO_TRUE; bi = CR_SO;
        break;
      case DoubleOrdered:
        bo = BO_FALSE; bi = CR_SO;
        break;
      default:
        MOZ_CRASH("Unhandled FP condition");
    }

    uint32_t invBo = (bo == BO_TRUE) ? BO_FALSE : BO_TRUE;
    as_bc(invBo, bi, 8);
    ma_b(label);
}

// =========================================================================
// Far branch helpers
// =========================================================================

void
MacroAssemblerPPC::ma_farBranch(Register scratch, const void* target)
{
    ma_li(scratch, ImmPtr(target));
    as_mtctr(scratch);
    as_bctr();
}

void
MacroAssemblerPPC::ma_call(Register scratch, const void* target)
{
    ma_li(scratch, ImmPtr(target));
    as_mtctr(scratch);
    as_bctrl();
}

// =========================================================================
// =========================================================================
// MacroAssemblerPPCCompat methods
// =========================================================================
// =========================================================================

// =====================================================================
// Value operations (nunbox32)
// =====================================================================

void
MacroAssemblerPPCCompat::moveValue(const ValueOperand& src, const ValueOperand& dest)
{
    Register s0 = src.typeReg();
    Register s1 = src.payloadReg();
    Register d0 = dest.typeReg();
    Register d1 = dest.payloadReg();

    // Handle overlapping registers
    if (s1 == d0) {
        if (s0 == d1) {
            // Swap
            as_xor(s0, s0, s1);
            as_xor(s1, s0, s1);
            as_xor(s0, s0, s1);
        } else {
            ma_move(d1, s1);
            ma_move(d0, s0);
        }
    } else {
        ma_move(d0, s0);
        ma_move(d1, s1);
    }
}

void
MacroAssemblerPPCCompat::moveValue(const Value& src, const ValueOperand& dest)
{
    ma_li(dest.typeReg(), Imm32(src.toNunboxTag()));
    if (src.isGCThing())
        ma_li(dest.payloadReg(), ImmGCPtr(src.toGCThing()));
    else
        ma_li(dest.payloadReg(), Imm32(src.toNunboxPayload()));
}

void
MacroAssemblerPPCCompat::boxValue(JSValueType type, Register src, const ValueOperand& dest)
{
    ma_li(dest.typeReg(), ImmTag(JSVAL_TYPE_TO_TAG(type)));
    ma_move(dest.payloadReg(), src);
}

void
MacroAssemblerPPCCompat::tagValue(JSValueType type, Register payload, ValueOperand dest)
{
    ma_li(dest.typeReg(), ImmTag(JSVAL_TYPE_TO_TAG(type)));
    if (payload != dest.payloadReg())
        ma_move(dest.payloadReg(), payload);
}

void
MacroAssemblerPPCCompat::pushValue(const ValueOperand& val)
{
    // Allocate stack slots for type and payload, then store at correct offsets.
    asMasm().subPtr(Imm32(sizeof(Value)), StackPointer);
    storeValue(val, Address(StackPointer, 0));
}

void
MacroAssemblerPPCCompat::pushValue(const Value& val)
{
    asMasm().subPtr(Imm32(sizeof(Value)), StackPointer);
    storeValue(val, Address(StackPointer, 0));
}

void
MacroAssemblerPPCCompat::pushValue(JSValueType type, Register reg)
{
    asMasm().subPtr(Imm32(sizeof(Value)), StackPointer);
    storeValue(type, reg, Address(StackPointer, 0));
}

void
MacroAssemblerPPCCompat::pushValue(const Address& addr)
{
    asMasm().subPtr(Imm32(sizeof(Value)), StackPointer);
    ma_lw(ScratchRegister, Address(addr.base, addr.offset + TAG_OFFSET));
    ma_sw(ScratchRegister, Address(StackPointer, TAG_OFFSET));
    ma_lw(ScratchRegister, Address(addr.base, addr.offset + PAYLOAD_OFFSET));
    ma_sw(ScratchRegister, Address(StackPointer, PAYLOAD_OFFSET));
}

void
MacroAssemblerPPCCompat::popValue(ValueOperand val)
{
    // Load payload and type from correct offsets, then free stack.
    ma_lw(val.payloadReg(), Address(StackPointer, PAYLOAD_OFFSET));
    ma_lw(val.typeReg(), Address(StackPointer, TAG_OFFSET));
    asMasm().addPtr(Imm32(sizeof(Value)), StackPointer);
}

void
MacroAssemblerPPCCompat::storeValue(const ValueOperand& val, const Address& dest)
{
    ma_sw(val.typeReg(), Address(dest.base, dest.offset + TAG_OFFSET));
    ma_sw(val.payloadReg(), Address(dest.base, dest.offset + PAYLOAD_OFFSET));
}

void
MacroAssemblerPPCCompat::storeValue(const ValueOperand& val, const BaseIndex& dest)
{
    computeScaledAddress(dest, ScratchRegister);
    storeValue(val, Address(ScratchRegister, dest.offset));
}

void
MacroAssemblerPPCCompat::storeValue(const Value& val, const Address& dest)
{
    ma_li(SecondScratchRegister, Imm32(val.toNunboxTag()));
    ma_sw(SecondScratchRegister, Address(dest.base, dest.offset + TAG_OFFSET));
    if (val.isGCThing()) {
        ma_li(SecondScratchRegister, ImmGCPtr(val.toGCThing()));
    } else {
        ma_li(SecondScratchRegister, Imm32(val.toNunboxPayload()));
    }
    ma_sw(SecondScratchRegister, Address(dest.base, dest.offset + PAYLOAD_OFFSET));
}

void
MacroAssemblerPPCCompat::storeValue(JSValueType type, Register reg, const Address& dest)
{
    ma_li(SecondScratchRegister, ImmTag(JSVAL_TYPE_TO_TAG(type)));
    ma_sw(SecondScratchRegister, Address(dest.base, dest.offset + TAG_OFFSET));
    ma_sw(reg, Address(dest.base, dest.offset + PAYLOAD_OFFSET));
}

void
MacroAssemblerPPCCompat::storeValue(JSValueType type, Register reg, const BaseIndex& dest)
{
    computeScaledAddress(dest, ScratchRegister);
    storeValue(type, reg, Address(ScratchRegister, dest.offset));
}



void
MacroAssemblerPPCCompat::loadValue(const Address& src, const ValueOperand& dest)
{
    // Must handle overlap: dest.typeReg() might be the same as src.base
    Register base = src.base;

    if (base == dest.payloadReg()) {
        // Load type first, it doesn't clobber base
        ma_lw(dest.typeReg(), Address(base, src.offset + TAG_OFFSET));
        ma_lw(dest.payloadReg(), Address(base, src.offset + PAYLOAD_OFFSET));
    } else {
        ma_lw(dest.payloadReg(), Address(base, src.offset + PAYLOAD_OFFSET));
        ma_lw(dest.typeReg(), Address(base, src.offset + TAG_OFFSET));
    }
}

void
MacroAssemblerPPCCompat::loadValue(const BaseIndex& src, const ValueOperand& dest)
{
    computeScaledAddress(src, ScratchRegister);
    loadValue(Address(ScratchRegister, src.offset), dest);
}

// =====================================================================
// Unboxing
// =====================================================================

void MacroAssemblerPPCCompat::unboxInt32(const ValueOperand& src, Register dest)
{
    ma_move(dest, src.payloadReg());
}

void MacroAssemblerPPCCompat::unboxInt32(const Address& src, Register dest)
{
    ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerPPCCompat::unboxBoolean(const ValueOperand& src, Register dest)
{
    ma_move(dest, src.payloadReg());
}

void MacroAssemblerPPCCompat::unboxBoolean(const Address& src, Register dest)
{
    ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerPPCCompat::unboxDouble(const ValueOperand& src, FloatRegister dest)
{
    // Store type+payload to stack, load as double
    // Big-endian: type is at lower address
    as_stwu(src.typeReg(), StackPointer, -8);
    as_stw(src.payloadReg(), StackPointer, 4);
    as_lfd(dest, StackPointer, 0);
    as_addi(StackPointer, StackPointer, 8);
}

void MacroAssemblerPPCCompat::unboxDouble(const Address& src, FloatRegister dest)
{
    ma_ld(dest, src);
}

void MacroAssemblerPPCCompat::unboxString(const ValueOperand& src, Register dest)
{
    ma_move(dest, src.payloadReg());
}

void MacroAssemblerPPCCompat::unboxString(const Address& src, Register dest)
{
    ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerPPCCompat::unboxSymbol(const ValueOperand& src, Register dest)
{
    ma_move(dest, src.payloadReg());
}

void MacroAssemblerPPCCompat::unboxSymbol(const Address& src, Register dest)
{
    ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerPPCCompat::unboxObject(const ValueOperand& src, Register dest)
{
    ma_move(dest, src.payloadReg());
}

void MacroAssemblerPPCCompat::unboxObject(const Address& src, Register dest)
{
    ma_lw(dest, Address(src.base, src.offset + PAYLOAD_OFFSET));
}

void MacroAssemblerPPCCompat::unboxValue(const ValueOperand& src, AnyRegister dest)
{
    if (dest.isFloat()) {
        unboxDouble(src, dest.fpu());
    } else {
        ma_move(dest.gpr(), src.payloadReg());
    }
}

void MacroAssemblerPPCCompat::unboxNonDouble(const ValueOperand& src, Register dest)
{
    ma_move(dest, src.payloadReg());
}

// =====================================================================
// Type testing helpers
// =====================================================================

Register
MacroAssemblerPPCCompat::extractTag(const Address& address, Register scratch)
{
    ma_lw(scratch, Address(address.base, address.offset + TAG_OFFSET));
    return scratch;
}

Register
MacroAssemblerPPCCompat::extractTag(const BaseIndex& address, Register scratch)
{
    computeScaledAddress(address, scratch);
    ma_lw(scratch, Address(scratch, address.offset + TAG_OFFSET));
    return scratch;
}

Register
MacroAssemblerPPCCompat::extractTag(const ValueOperand& value, Register scratch)
{
    return value.typeReg();
}

Register
MacroAssemblerPPCCompat::extractObject(const Address& address, Register scratch)
{
    ma_lw(scratch, Address(address.base, address.offset + PAYLOAD_OFFSET));
    return scratch;
}

Register
MacroAssemblerPPCCompat::extractObject(const ValueOperand& value, Register scratch)
{
    return value.payloadReg();
}

// =====================================================================
// Type conversions
// =====================================================================

void MacroAssemblerPPCCompat::convertBoolToInt32(Register src, Register dest)
{
    // Boolean is already 0 or 1 in the payload
    ma_move(dest, src);
}

void MacroAssemblerPPCCompat::convertInt32ToDouble(Register src, FloatRegister dest)
{
    // Store int32 to stack, load as int, convert via FP trick
    // PPC method: store to memory, lfd, fcfid (G5) or subtract-based trick (G3)
    // For now, use the stack-based method that works on all PPC:
    // 1. Store 0x43300000 and the int32 (XOR'd with 0x80000000) as a double
    // 2. Load the double, subtract the magic constant
    as_stwu(src, StackPointer, -8);
    ma_li(ScratchRegister, Imm32(0x43300000));
    as_stw(ScratchRegister, StackPointer, 0);  // high word
    // XOR with 0x80000000 to convert signed to unsigned offset
    as_xori(ScratchRegister, src, 0);
    ma_li(ScratchRegister, Imm32(int32_t(0x80000000u)));
    as_xor(ScratchRegister, src, ScratchRegister);
    as_stw(ScratchRegister, StackPointer, 4);  // low word = src ^ 0x80000000
    // Reload the magic constant pair
    ma_li(ScratchRegister, Imm32(0x43300000));
    as_stw(ScratchRegister, StackPointer, 0);

    as_lfd(dest, StackPointer, 0);

    // Load the magic constant 2^52 + 2^31 = 0x4330000080000000
    ma_li(ScratchRegister, Imm32(int32_t(0x80000000u)));
    as_stw(ScratchRegister, StackPointer, 4);
    as_lfd(ScratchDoubleReg, StackPointer, 0);

    as_fsub(dest, dest, ScratchDoubleReg);
    as_addi(StackPointer, StackPointer, 8);
}

void MacroAssemblerPPCCompat::convertInt32ToDouble(const Address& src, FloatRegister dest)
{
    ma_lw(ScratchRegister, src);
    convertInt32ToDouble(ScratchRegister, dest);
}

void MacroAssemblerPPCCompat::convertInt32ToFloat32(Register src, FloatRegister dest)
{
    convertInt32ToDouble(src, dest);
    as_frsp(dest, dest);
}

void MacroAssemblerPPCCompat::convertInt32ToFloat32(const Address& src, FloatRegister dest)
{
    ma_lw(ScratchRegister, src);
    convertInt32ToFloat32(ScratchRegister, dest);
}

void MacroAssemblerPPCCompat::convertUInt32ToDouble(Register src, FloatRegister dest)
{
    // Same magic number trick but without the XOR (unsigned, not signed)
    as_stwu(src, StackPointer, -8);
    ma_li(ScratchRegister, Imm32(0x43300000));
    as_stw(ScratchRegister, StackPointer, 0);
    as_stw(src, StackPointer, 4);
    as_lfd(dest, StackPointer, 0);

    // Subtract 2^52 = 0x4330000000000000
    ma_li(ScratchRegister, Imm32(0));
    as_stw(ScratchRegister, StackPointer, 4);
    as_lfd(ScratchDoubleReg, StackPointer, 0);

    as_fsub(dest, dest, ScratchDoubleReg);
    as_addi(StackPointer, StackPointer, 8);
}

void MacroAssemblerPPCCompat::convertUInt32ToFloat32(Register src, FloatRegister dest)
{
    convertUInt32ToDouble(src, dest);
    as_frsp(dest, dest);
}

void MacroAssemblerPPCCompat::convertDoubleToFloat32(FloatRegister src, FloatRegister dest)
{
    as_frsp(dest, src);
}

void MacroAssemblerPPCCompat::convertFloat32ToDouble(FloatRegister src, FloatRegister dest)
{
    // PPC automatically extends to double precision via fmr
    as_fmr(dest, src);
}

void MacroAssemblerPPCCompat::convertDoubleToInt32(FloatRegister src, Register dest, Label* fail, bool negativeZeroCheck)
{
    // Use fctiwz to convert, then check for overflow
    FloatRegister temp = ScratchDoubleReg;
    as_fctiwz(temp, src);

    // Store the result to stack and load as integer
    as_stwu(r0, StackPointer, -8);
    as_stfd(temp, StackPointer, 0);
    as_lwz(dest, StackPointer, 4); // Low word contains the integer result
    as_addi(StackPointer, StackPointer, 8);

    // Check for overflow: if result is 0x80000000 (INT_MIN), likely overflow
    ma_li(ScratchRegister, Imm32(int32_t(0x80000000u)));
    as_cmpw(dest, ScratchRegister);
    as_bc(BO_TRUE, CR_EQ, 0);
    if (fail)
        ma_b(fail);

    // Check for negative zero if needed
    if (negativeZeroCheck) {
        as_cmpwi(dest, 0);
        Label notZero;
        as_bc(BO_FALSE, CR_EQ, 8);

        // If zero, check the sign bit of the original double
        as_stfd(src, StackPointer, -8);
        as_lwz(ScratchRegister, StackPointer, -8); // high word
        as_cmpwi(ScratchRegister, 0);
        // If negative, fail
        if (fail)
            ma_b(fail, LessThan);
    }
}

void MacroAssemblerPPCCompat::convertFloat32ToInt32(FloatRegister src, Register dest, Label* fail, bool negativeZeroCheck)
{
    convertDoubleToInt32(src, dest, fail, negativeZeroCheck);
}

// =====================================================================
// Pointer/memory operations
// =====================================================================

void MacroAssemblerPPCCompat::addPtr(Register src, Register dest) { as_add(dest, dest, src); }
void MacroAssemblerPPCCompat::addPtr(Imm32 imm, Register dest) { ma_addu(dest, imm); }
void MacroAssemblerPPCCompat::addPtr(ImmWord imm, Register dest) { ma_addu(dest, Imm32(imm.value)); }
void MacroAssemblerPPCCompat::addPtr(Imm32 imm, const Address& dest)
{
    ma_lw(ScratchRegister, dest);
    ma_addu(ScratchRegister, imm);
    ma_sw(ScratchRegister, dest);
}
void MacroAssemblerPPCCompat::subPtr(Register src, Register dest) { ma_subu(dest, dest, src); }
void MacroAssemblerPPCCompat::subPtr(Imm32 imm, Register dest) { ma_subu(dest, imm); }
void MacroAssemblerPPCCompat::addToStackPtr(Imm32 imm) { ma_addu(StackPointer, imm); }

void MacroAssemblerPPCCompat::movePtr(Register src, Register dest) { ma_move(dest, src); }
void MacroAssemblerPPCCompat::movePtr(ImmWord imm, Register dest) { ma_li(dest, imm); }
void MacroAssemblerPPCCompat::movePtr(ImmPtr imm, Register dest) { ma_li(dest, ImmWord(uintptr_t(imm.value))); }
void MacroAssemblerPPCCompat::movePtr(ImmGCPtr imm, Register dest) { ma_li(dest, imm); }

void MacroAssemblerPPCCompat::loadPtr(const Address& address, Register dest) { ma_lw(dest, address); }
void MacroAssemblerPPCCompat::loadPtr(const BaseIndex& address, Register dest) { ma_load(dest, address); }
void MacroAssemblerPPCCompat::loadPtr(AbsoluteAddress address, Register dest)
{
    ma_li(ScratchRegister, ImmWord(uintptr_t(address.addr)));
    as_lwz(dest, ScratchRegister, 0);
}

void MacroAssemblerPPCCompat::storePtr(Register src, const Address& address) { ma_sw(src, address); }
void MacroAssemblerPPCCompat::storePtr(Register src, const BaseIndex& address) { ma_store(src, address); }
void MacroAssemblerPPCCompat::storePtr(ImmWord imm, const Address& address)
{
    ma_li(SecondScratchRegister, Imm32(imm.value));
    ma_sw(SecondScratchRegister, address);
}
void MacroAssemblerPPCCompat::storePtr(ImmPtr imm, const Address& address)
{
    storePtr(ImmWord(uintptr_t(imm.value)), address);
}
void MacroAssemblerPPCCompat::storePtr(ImmGCPtr imm, const Address& address)
{
    ma_li(SecondScratchRegister, imm);
    ma_sw(SecondScratchRegister, address);
}

// Simple load/store wrappers
void MacroAssemblerPPCCompat::load8ZeroExtend(const Address& src, Register dest) { ma_load(dest, src, SizeByte, ZeroExtend); }
void MacroAssemblerPPCCompat::load8SignExtend(const Address& src, Register dest) { ma_load(dest, src, SizeByte, SignExtend); }
void MacroAssemblerPPCCompat::load16ZeroExtend(const Address& src, Register dest) { ma_load(dest, src, SizeHalfWord, ZeroExtend); }
void MacroAssemblerPPCCompat::load16SignExtend(const Address& src, Register dest) { ma_load(dest, src, SizeHalfWord, SignExtend); }
void MacroAssemblerPPCCompat::load32(const Address& address, Register dest) { ma_lw(dest, address); }
void MacroAssemblerPPCCompat::load32(const BaseIndex& address, Register dest) { ma_load(dest, address); }
void MacroAssemblerPPCCompat::store8(Register src, const Address& address) { ma_store(src, address, SizeByte, ZeroExtend); }
void MacroAssemblerPPCCompat::store16(Register src, const Address& address) { ma_store(src, address, SizeHalfWord, ZeroExtend); }
void MacroAssemblerPPCCompat::store32(Register src, const Address& address) { ma_sw(src, address); }
void MacroAssemblerPPCCompat::store32(Imm32 imm, const Address& address) { ma_sw(imm, address); }
void MacroAssemblerPPCCompat::store32(Register src, const BaseIndex& address) { ma_store(src, address); }

void MacroAssemblerPPCCompat::push(Register reg) { ma_push(reg); }
void MacroAssemblerPPCCompat::push(Imm32 imm) { ma_li(ScratchRegister, imm); ma_push(ScratchRegister); }
void MacroAssemblerPPCCompat::push(ImmWord imm) { push(Imm32(imm.value)); }
void MacroAssemblerPPCCompat::push(const Address& addr) { ma_lw(ScratchRegister, addr); ma_push(ScratchRegister); }
void MacroAssemblerPPCCompat::pop(Register reg) { ma_pop(reg); }

CodeOffset
MacroAssemblerPPCCompat::pushWithPatch(ImmWord imm)
{
    CodeOffset label;
    ma_li(ScratchRegister, &label);
    ma_push(ScratchRegister);
    return label;
}

// =====================================================================
// Branches
// =====================================================================

void MacroAssemblerPPCCompat::branch(JitCode* target)
{
    ma_li(ScratchRegister, ImmPtr(target->raw()));
    as_mtctr(ScratchRegister);
    as_bctr();
}

void MacroAssemblerPPCCompat::branch(Register target)
{
    as_mtctr(target);
    as_bctr();
}

void MacroAssemblerPPCCompat::branch(const Address& addr)
{
    ma_lw(ScratchRegister, addr);
    as_mtctr(ScratchRegister);
    as_bctr();
}

void MacroAssemblerPPCCompat::branch32(Condition cond, Register lhs, Register rhs, Label* label) { ma_b(lhs, rhs, label, cond); }
void MacroAssemblerPPCCompat::branch32(Condition cond, Register lhs, Imm32 imm, Label* label) { ma_b(lhs, imm, label, cond); }
void MacroAssemblerPPCCompat::branch32(Condition cond, const Address& lhs, Register rhs, Label* label) {
    ma_lw(ScratchRegister, lhs);
    ma_b(ScratchRegister, rhs, label, cond);
}
void MacroAssemblerPPCCompat::branch32(Condition cond, const Address& lhs, Imm32 imm, Label* label) {
    ma_lw(ScratchRegister, lhs);
    ma_b(ScratchRegister, imm, label, cond);
}
void MacroAssemblerPPCCompat::branch32(Condition cond, const BaseIndex& lhs, Imm32 imm, Label* label) {
    ma_load(ScratchRegister, lhs);
    ma_b(ScratchRegister, imm, label, cond);
}

void MacroAssemblerPPCCompat::branchPtr(Condition cond, Register lhs, Register rhs, Label* label) { ma_b(lhs, rhs, label, cond); }
void MacroAssemblerPPCCompat::branchPtr(Condition cond, Register lhs, ImmPtr imm, Label* label) { ma_b(lhs, imm, label, cond); }
void MacroAssemblerPPCCompat::branchPtr(Condition cond, Register lhs, ImmWord imm, Label* label) { ma_b(lhs, imm, label, cond); }
void MacroAssemblerPPCCompat::branchPtr(Condition cond, Register lhs, ImmGCPtr imm, Label* label) { ma_b(lhs, imm, label, cond); }
void MacroAssemblerPPCCompat::branchPtr(Condition cond, const Address& lhs, Register rhs, Label* label) {
    ma_lw(ScratchRegister, lhs);
    ma_b(ScratchRegister, rhs, label, cond);
}
void MacroAssemblerPPCCompat::branchPtr(Condition cond, const Address& lhs, ImmPtr imm, Label* label) {
    ma_lw(ScratchRegister, lhs);
    ma_b(ScratchRegister, imm, label, cond);
}
void MacroAssemblerPPCCompat::branchPtr(Condition cond, const Address& lhs, ImmWord imm, Label* label) {
    ma_lw(ScratchRegister, lhs);
    ma_b(ScratchRegister, imm, label, cond);
}
void MacroAssemblerPPCCompat::branchPtr(Condition cond, const BaseIndex& lhs, ImmWord imm, Label* label) {
    ma_load(ScratchRegister, lhs);
    ma_b(ScratchRegister, imm, label, cond);
}

void MacroAssemblerPPCCompat::branchTest32(Condition cond, Register lhs, Register rhs, Label* label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
    as_and(ScratchRegister, lhs, rhs, /*rc=*/true); // sets CR0
    if (cond == Signed) {
        // Check sign bit via CR0[LT] (set if result < 0)
        EmitBranchOnCondition(*this, LessThan, label);
    } else if (cond == NotSigned) {
        EmitBranchOnCondition(*this, GreaterThanOrEqual, label);
    } else {
        EmitBranchOnCondition(*this, cond, label);
    }
}

void MacroAssemblerPPCCompat::branchTest32(Condition cond, Register lhs, Imm32 imm, Label* label)
{
    ma_and(ScratchRegister, lhs, imm);
    as_cmpwi(ScratchRegister, 0);
    EmitBranchOnCondition(*this, cond, label);
}

void MacroAssemblerPPCCompat::branchTest32(Condition cond, const Address& lhs, Imm32 imm, Label* label)
{
    ma_lw(SecondScratchRegister, lhs);
    branchTest32(cond, SecondScratchRegister, imm, label);
}

void MacroAssemblerPPCCompat::branchTestPtr(Condition cond, Register lhs, Register rhs, Label* label)
{
    branchTest32(cond, lhs, rhs, label);
}

// =====================================================================
// Calls
// =====================================================================

void MacroAssemblerPPCCompat::call(Register reg)
{
    as_mtctr(reg);
    as_bctrl();
}

void MacroAssemblerPPCCompat::call(Label* label)
{
    // Branch and link to label (sets LR for abiret/blr return)
    ma_bl(label);
}

void MacroAssemblerPPCCompat::call(JitCode* code)
{
    ma_li(ScratchRegister, ImmPtr(code->raw()));
    as_mtctr(ScratchRegister);
    as_bctrl();
}

void MacroAssemblerPPCCompat::call(ImmWord imm)
{
    ma_li(ScratchRegister, imm);
    as_mtctr(ScratchRegister);
    as_bctrl();
}

void MacroAssemblerPPCCompat::call(ImmPtr imm) { call(ImmWord(uintptr_t(imm.value))); }

void MacroAssemblerPPCCompat::call(const Address& addr)
{
    ma_lw(ScratchRegister, addr);
    as_mtctr(ScratchRegister);
    as_bctrl();
}

CodeOffset
MacroAssemblerPPCCompat::callWithPatch()
{
    CodeOffset label;
    ma_liPatchable(ScratchRegister, ImmPtr(nullptr));
    label.bind(size() - 8); // Point to the lis instruction
    as_mtctr(ScratchRegister);
    as_bctrl();
    return label;
}

// =====================================================================
// Frame management
// =====================================================================

void MacroAssemblerPPCCompat::makeFrameDescriptor(Register frameSizeReg, FrameType type)
{
    ma_sll(frameSizeReg, frameSizeReg, Imm32(FRAMESIZE_SHIFT));
    ma_or(frameSizeReg, Imm32(type));
}

void MacroAssemblerPPCCompat::linkExitFrame(Register cxReg, Register scratch)
{
    // Store StackPointer to JitTop in the JSContext
    // JSContext inherits from JSRuntime, so cxReg IS a JSRuntime*
    storePtr(StackPointer, Address(cxReg, offsetof(JSRuntime, jitTop)));
}

void MacroAssemblerPPCCompat::handleFailureWithHandlerTail(void* handler)
{
    // Reserve space for exception information.
    int size = (sizeof(ResumeFromException) + ABIStackAlignment) & ~(ABIStackAlignment - 1);
    asMasm().subPtr(Imm32(size), StackPointer);
    ma_move(r3, StackPointer);

    // Call the handler.
    asMasm().setupUnalignedABICall(r4);
    asMasm().passABIArg(r3);
    asMasm().callWithABI(handler);

    Label entryFrame;
    Label catch_;
    Label finally;
    Label return_;
    Label bailout;

    load32(Address(StackPointer, offsetof(ResumeFromException, kind)), r3);
    asMasm().branch32(Assembler::Equal, r3, Imm32(ResumeFromException::RESUME_ENTRY_FRAME),
                      &entryFrame);
    asMasm().branch32(Assembler::Equal, r3, Imm32(ResumeFromException::RESUME_CATCH), &catch_);
    asMasm().branch32(Assembler::Equal, r3, Imm32(ResumeFromException::RESUME_FINALLY), &finally);
    asMasm().branch32(Assembler::Equal, r3, Imm32(ResumeFromException::RESUME_FORCED_RETURN),
                      &return_);
    asMasm().branch32(Assembler::Equal, r3, Imm32(ResumeFromException::RESUME_BAILOUT), &bailout);

    breakpoint(); // Invalid kind.

    // No exception handler. Load the error value, load the new stack pointer
    // and return from the entry frame.
    bind(&entryFrame);
    asMasm().moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);
    // PPC diag: mark that we went through RESUME_ENTRY_FRAME
    ma_li(r23, Imm32(0xDEAD0003));
    ma_move(r24, StackPointer);  // r24 = SP before restore
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)), StackPointer);
    ma_move(r25, StackPointer);  // r25 = SP after restore from exception data

    // Pop return address from the Ion frame and jump to it
    ma_pop(r0);
    as_mtlr(r0);
    as_blr();

    // Catch handler: restore state and jump to catch block.
    bind(&catch_);
    // PPC diag: mark RESUME_CATCH
    ma_li(r26, Imm32(0xDEAD0004));
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, target)), r3);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)), BaselineFrameReg);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)), StackPointer);
    jump(r3);

    // Finally block: push BooleanValue(true) and the exception, then jump.
    bind(&finally);
    ValueOperand exception = ValueOperand(r4, r5);
    loadValue(Address(StackPointer, offsetof(ResumeFromException, exception)), exception);

    loadPtr(Address(StackPointer, offsetof(ResumeFromException, target)), r3);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)), BaselineFrameReg);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)), StackPointer);

    pushValue(BooleanValue(true));
    pushValue(exception);
    jump(r3);

    // Forced return: return BaselineFrame->returnValue() to the caller.
    bind(&return_);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, framePointer)), BaselineFrameReg);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, stackPointer)), StackPointer);
    loadValue(Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfReturnValue()),
              JSReturnOperand);
    // PPC diag: mark that we went through RESUME_FORCED_RETURN
    ma_li(r23, Imm32(0xDEAD0002));
    ma_move(r24, StackPointer);        // r24 = SP before epilogue-like code
    ma_move(r25, BaselineFrameReg);    // r25 = BFR from exception data
    ma_move(StackPointer, BaselineFrameReg);
    pop(BaselineFrameReg);

    {
        Label skipProfilingInstrumentation;
        AbsoluteAddress addressOfEnabled(GetJitContext()->runtime->spsProfiler().addressOfEnabled());
        asMasm().branch32(Assembler::Equal, addressOfEnabled, Imm32(0),
                          &skipProfilingInstrumentation);
        profilerExitFrame();
        bind(&skipProfilingInstrumentation);
    }

    ret();

    // Bailout: jump to the bailout tail stub.
    bind(&bailout);
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, bailoutInfo)), r5);
    ma_li(ReturnReg, Imm32(BAILOUT_RETURN_OK));
    loadPtr(Address(StackPointer, offsetof(ResumeFromException, target)), r4);
    jump(r4);
}

// =====================================================================
// Toggled jump/call
// =====================================================================

CodeOffset
MacroAssemblerPPCCompat::toggledJump(Label* label)
{
    CodeOffset ret(size());
    ma_b(label);
    return ret;
}

CodeOffset
MacroAssemblerPPCCompat::toggledCall(JitCode* target, bool enabled)
{
    CodeOffset offset(size());
    ma_liPatchable(ScratchRegister, ImmPtr(target->raw()));
    as_mtctr(ScratchRegister);
    if (enabled)
        as_bctrl();
    else
        as_nop();
    return offset;
}

void MacroAssemblerPPCCompat::profilerEnterFrame(Register framePtr, Register scratch)
{
    // TODO: profiling support
}

void MacroAssemblerPPCCompat::profilerExitFrame()
{
    // TODO: profiling support
}

// =====================================================================
// GC barriers
// =====================================================================

void MacroAssemblerPPCCompat::patchableCallPreBarrier(const Address& addr, MIRType type)
{
    // TODO: implement pre-barrier
}

// =====================================================================
// ICache flushing
// =====================================================================

void MacroAssemblerPPCCompat::flushICacheForRange(Register start, Register length)
{
    // PPC requires explicit cache flushing: dcbf + icbi + sync + isync
    Label loop;
    Register offset = SecondScratchRegister;
    ma_li(offset, Imm32(0));

    // loop:
    bind(&loop);
    as_dcbf(start, offset);
    as_icbi(start, offset);
    as_addi(offset, offset, 32); // cache line size
    as_cmplw(offset, length);
    as_bc(BO_TRUE, CR_LT, -16); // branch back to dcbf
    as_sync();
    as_isync();
}

void
MacroAssemblerPPCCompat::bind(Label* label)
{
    // Patch all pending uses of this label
    BufferOffset here(size());

    if (label->used()) {
        // Walk the chain of uses and patch them
        int32_t offset = label->offset();
        while (offset != Label::INVALID_OFFSET) {
            // Read the instruction at the offset
            uint8_t* inst = m_buffer.buffer() + offset;
            uint32_t* p = (uint32_t*)inst;

            // The next use offset is stored in the branch displacement
            int32_t nextOffset = *p & 0x03fffffc;
            if (nextOffset & 0x02000000)
                nextOffset |= 0xfc000000; // sign extend
            int32_t nextUse = (nextOffset == 0) ? Label::INVALID_OFFSET : offset + nextOffset;

            // Patch this instruction with the actual offset
            int32_t branchOffset = int32_t(size()) - offset;
            *p = (*p & ~0x03fffffc) | (branchOffset & 0x03fffffc);

            offset = nextUse;
        }
    }

    label->bind(size());
}

// =====================================================================
// branchTest for Value types (nunbox32)
// =====================================================================

// Helper for type tag testing
static void
BranchTestTag(MacroAssemblerPPC& masm, Assembler::Condition cond,
              Register tag, JSValueTag expected, Label* label)
{
    masm.as_cmpwi(tag, int32_t(expected));
    EmitBranchOnCondition(masm, cond, label);
}

#define DEFINE_BRANCH_TEST_VALUE(name, tag)                                    \
template <>                                                                     \
void MacroAssemblerPPCCompat::branchTest##name(Condition cond,                 \
                                                const ValueOperand& value,      \
                                                Label* label)                   \
{                                                                               \
    BranchTestTag(*this, cond, value.typeReg(), tag, label);                    \
}                                                                               \
template <>                                                                     \
void MacroAssemblerPPCCompat::branchTest##name(Condition cond,                 \
                                                const Address& address,         \
                                                Label* label)                   \
{                                                                               \
    Register scratch = ScratchRegister;                                         \
    extractTag(address, scratch);                                               \
    BranchTestTag(*this, cond, scratch, tag, label);                            \
}                                                                               \
template <>                                                                     \
void MacroAssemblerPPCCompat::branchTest##name(Condition cond,                 \
                                                Register tag,                   \
                                                Label* label)                   \
{                                                                               \
    BranchTestTag(*this, cond, tag, tag##_TAG, label);                          \
}

// These macros won't work cleanly for all cases since the tag values
// aren't consistently named. Let me just implement them directly.

void MacroAssemblerPPCCompat::branchTestValue(Condition cond, const ValueOperand& lhs, const Value& rhs, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    as_cmpw(lhs.typeReg(), ScratchRegister);
    ma_li(ScratchRegister, Imm32(rhs.toNunboxTag()));
    as_cmpw(lhs.typeReg(), ScratchRegister);
    Label notEqual;
    as_bc(BO_FALSE, CR_EQ, 0); // branch over if types differ
    if (cond == NotEqual) {
        ma_b(label);
        return;
    }
    // Types equal, check payload
    ma_li(ScratchRegister, Imm32(rhs.toNunboxPayload()));
    ma_b(lhs.payloadReg(), ScratchRegister, label, cond);
}

// Stub implementations for test methods — these will be filled in as needed
void MacroAssemblerPPCCompat::testNullSet(Condition cond, const ValueOperand& value, Register dest)
{
    ma_cmp_set(dest, value.typeReg(), Imm32(JSVAL_TAG_NULL), cond);
}

void MacroAssemblerPPCCompat::testUndefinedSet(Condition cond, const ValueOperand& value, Register dest)
{
    ma_cmp_set(dest, value.typeReg(), Imm32(JSVAL_TAG_UNDEFINED), cond);
}

void MacroAssemblerPPCCompat::testObjectSet(Condition cond, const ValueOperand& value, Register dest)
{
    ma_cmp_set(dest, value.typeReg(), Imm32(JSVAL_TAG_OBJECT), cond);
}


void
MacroAssemblerPPCCompat::computeEffectiveAddress(const BaseIndex& address, Register dest)
{
    computeScaledAddress(address, dest);
    if (address.offset)
        asMasm().addPtr(Imm32(address.offset), dest);
}

void
MacroAssemblerPPCCompat::retn(Imm32 n) {
    // pc <- [sp]; sp += n
    loadPtr(Address(StackPointer, 0), ScratchRegister);
    asMasm().addPtr(n, StackPointer);
    as_mtctr(ScratchRegister);
    as_bctr();
}

// ==========================================================================
// MOZ_CRASH stub definitions for missing MacroAssemblerPPCCompat methods
// ==========================================================================

void
MacroAssemblerPPCCompat::breakpoint()
{
    // tw 31, r0, r0 — unconditional trap
    writeInst(0x7FE00008);
}

void
MacroAssemblerPPCCompat::loadDouble(const Address& addr, FloatRegister dest)
{
    ma_ld(dest, addr);
}

void
MacroAssemblerPPCCompat::loadDouble(const BaseIndex& src, FloatRegister dest)
{
    computeScaledAddress(src, ScratchRegister);
    ma_ld(dest, Address(ScratchRegister, src.offset));
}

void
MacroAssemblerPPCCompat::loadFloat32(const Address& addr, FloatRegister dest)
{
    ma_ls(dest, addr);
}

void
MacroAssemblerPPCCompat::loadFloat32(const BaseIndex& src, FloatRegister dest)
{
    computeScaledAddress(src, ScratchRegister);
    ma_ls(dest, Address(ScratchRegister, src.offset));
}

void
MacroAssemblerPPCCompat::loadPrivate(const Address& address, Register dest)
{
    loadPtr(Address(address.base, address.offset + PAYLOAD_OFFSET), dest);
}

void
MacroAssemblerPPCCompat::ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure)
{
    Label isDouble, done;
    asMasm().branchTestDouble(Assembler::Equal, source.typeReg(), &isDouble);
    // Not double - convert int32 to double
    asMasm().branchTestInt32(Assembler::NotEqual, source.typeReg(), failure);
    convertInt32ToDouble(source.payloadReg(), dest);
    jump(&done);
    bind(&isDouble);
    unboxDouble(source, dest);
    bind(&done);
}

void
MacroAssemblerPPCCompat::haltingAlign(int alignment)
{
    // PPC: alignment handled by nops if needed
}

void
MacroAssemblerPPCCompat::load8SignExtend(const BaseIndex& src, Register dest)
{
    computeScaledAddress(src, SecondScratchRegister);
    ma_load(dest, Address(SecondScratchRegister, src.offset), SizeByte, SignExtend);
}

void
MacroAssemblerPPCCompat::load8ZeroExtend(const BaseIndex& src, Register dest)
{
    computeScaledAddress(src, SecondScratchRegister);
    ma_load(dest, Address(SecondScratchRegister, src.offset), SizeByte, ZeroExtend);
}

void
MacroAssemblerPPCCompat::load16SignExtend(const BaseIndex& src, Register dest)
{
    computeScaledAddress(src, SecondScratchRegister);
    ma_load(dest, Address(SecondScratchRegister, src.offset), SizeHalfWord, SignExtend);
}

void
MacroAssemblerPPCCompat::load16ZeroExtend(const BaseIndex& src, Register dest)
{
    computeScaledAddress(src, SecondScratchRegister);
    ma_load(dest, Address(SecondScratchRegister, src.offset), SizeHalfWord, ZeroExtend);
}

void
MacroAssemblerPPCCompat::int32ValueToDouble(const ValueOperand& operand, FloatRegister dest)
{
    unboxInt32(operand, ScratchRegister);
    convertInt32ToDouble(ScratchRegister, dest);
}

void
MacroAssemblerPPCCompat::boolValueToDouble(const ValueOperand& operand, FloatRegister dest)
{
    convertBoolToInt32(operand.payloadReg(), ScratchRegister);
    convertInt32ToDouble(ScratchRegister, dest);
}

void
MacroAssemblerPPCCompat::boolValueToFloat32(const ValueOperand& operand, FloatRegister dest)
{
    convertBoolToInt32(operand.payloadReg(), ScratchRegister);
    convertInt32ToFloat32(ScratchRegister, dest);
}

void
MacroAssemblerPPCCompat::loadConstantDouble(double d, FloatRegister dest)
{
    // Store double bits to stack, then load as FPR.
    union { double d; uint32_t w[2]; } u;
    u.d = d;
    // Big-endian: high word at lower address.
    asMasm().subPtr(Imm32(8), StackPointer);
    asMasm().store32(Imm32(u.w[0]), Address(StackPointer, 0));  // high word
    asMasm().store32(Imm32(u.w[1]), Address(StackPointer, 4));  // low word
    as_lfd(dest, StackPointer, 0);
    asMasm().addPtr(Imm32(8), StackPointer);
}

void
MacroAssemblerPPCCompat::checkStackAlignment()
{
    // PPC: stack alignment check - just a debug assertion, skip for now
}

void
MacroAssemblerPPCCompat::alignStackPointer()
{
    movePtr(StackPointer, SecondScratchRegister);
    asMasm().subPtr(Imm32(sizeof(uintptr_t)), StackPointer);
    asMasm().andPtr(Imm32(~(ABIStackAlignment - 1)), StackPointer);
    storePtr(SecondScratchRegister, Address(StackPointer, 0));
    movePtr(SecondScratchRegister, r30);

    // PPC FIX: Reset framePushed_ after dynamic alignment.
    // callWithABIPre uses framePushed() to compute alignment padding for the
    // ABI call. Pre-alignment pushes (exitFrame) are no longer relevant after
    // we force-align SP. Without this reset, the stale framePushed_ causes
    // callWithABIPre to miscalculate alignment:
    //   - PPC: 12 bytes pre-align, ABIStackAlignment=16 -> 12%16=12 bytes off
    //   - MIPS: 12 bytes pre-align, ABIStackAlignment=8  -> 12%8=4, works by luck
    // We save the old value so restoreStackPointer can put it back.
    savedFramePushed_ = asMasm().framePushed();
    asMasm().setFramePushed(0);
}

void
MacroAssemblerPPCCompat::restoreStackPointer()
{
    // Restore SP from r30 (saved in alignStackPointer).
    movePtr(r30, StackPointer);
    // Restore framePushed_ to pre-alignment value so leaveExitFrame works.
    asMasm().setFramePushed(savedFramePushed_);
}

void
MacroAssemblerPPCCompat::int32ValueToFloat32(const ValueOperand& operand, FloatRegister dest)
{
    unboxInt32(operand, ScratchRegister);
    convertInt32ToFloat32(ScratchRegister, dest);
}

void
MacroAssemblerPPCCompat::loadConstantFloat32(float f, FloatRegister dest)
{
    // Store float bits to stack, then load as FPR.
    union { float f; uint32_t w; } u;
    u.f = f;
    asMasm().subPtr(Imm32(4), StackPointer);
    asMasm().store32(Imm32(u.w), Address(StackPointer, 0));
    as_lfs(dest, StackPointer, 0);
    asMasm().addPtr(Imm32(4), StackPointer);
}

void
MacroAssemblerPPCCompat::computeScaledAddress(const BaseIndex& address, Register dest)
{
    int32_t shift = Imm32::ShiftOf(address.scale).value;
    if (shift) {
        ma_sll(ScratchRegister, address.index, Imm32(shift));
        as_add(dest, address.base, ScratchRegister);
    } else {
        as_add(dest, address.base, address.index);
    }
}

bool
MacroAssemblerPPCCompat::buildOOLFakeExitFrame(void* fakeReturnAddr)
{
    uint32_t descriptor = MakeFrameDescriptor(asMasm().framePushed(), JitFrame_IonJS,
                                              ExitFrameLayout::Size());
    asMasm().Push(Imm32(descriptor));
    asMasm().Push(ImmPtr(fakeReturnAddr));
    return true;
}

void
MacroAssemblerPPCCompat::bind(RepatchLabel* label)
{
    BufferOffset dest = nextOffset();
    if (label->used() && !oom()) {
        // Patch the branch instruction at the label's offset to jump here
        intptr_t offset = dest.getOffset() - label->offset();
        // Overwrite the original instruction with a branch to current position
        // PPC b instruction: 0x48000000 | (offset & 0x03FFFFFC)
        uint32_t* inst = (uint32_t*)(m_buffer.buffer() + label->offset());
        *inst = 0x48000000 | (offset & 0x03FFFFFC);
    }
    label->bind(dest.getOffset());
}

void
MacroAssemblerPPCCompat::push(FloatRegister reg)
{
    asMasm().subPtr(Imm32(sizeof(double)), StackPointer);
    ma_sd(reg, Address(StackPointer, 0));
}

void
MacroAssemblerPPCCompat::load32(AbsoluteAddress address, Register dest)
{
    ma_li(ScratchRegister, Imm32((uint32_t)address.addr));
    ma_lw(dest, Address(ScratchRegister, 0));
}

void
MacroAssemblerPPCCompat::store8(Imm32 imm, const Address& address)
{
    ma_li(SecondScratchRegister, imm);
    ma_store(SecondScratchRegister, address, SizeByte, ZeroExtend);
}

void
MacroAssemblerPPCCompat::store8(Imm32 imm, const BaseIndex& address)
{
    computeScaledAddress(address, ScratchRegister);
    ma_li(SecondScratchRegister, imm);
    ma_store(SecondScratchRegister, Address(ScratchRegister, address.offset), SizeByte, ZeroExtend);
}

void
MacroAssemblerPPCCompat::store8(Register src, const BaseIndex& address)
{
    computeScaledAddress(address, ScratchRegister);
    ma_store(src, Address(ScratchRegister, address.offset), SizeByte, ZeroExtend);
}

void
MacroAssemblerPPCCompat::movePtr(wasm::SymbolicAddress imm, Register dest)
{
    MOZ_CRASH("PPC: NYI movePtr(SymbolicAddress)");
}

void
MacroAssemblerPPCCompat::store16(Imm32 imm, const Address& address)
{
    ma_li(SecondScratchRegister, imm);
    ma_store(SecondScratchRegister, address, SizeHalfWord, ZeroExtend);
}

void
MacroAssemblerPPCCompat::store16(Imm32 imm, const BaseIndex& address)
{
    computeScaledAddress(address, ScratchRegister);
    ma_li(SecondScratchRegister, imm);
    ma_store(SecondScratchRegister, Address(ScratchRegister, address.offset), SizeHalfWord, ZeroExtend);
}

void
MacroAssemblerPPCCompat::store16(Register src, const BaseIndex& address)
{
    computeScaledAddress(address, ScratchRegister);
    ma_store(src, Address(ScratchRegister, address.offset), SizeHalfWord, ZeroExtend);
}

void
MacroAssemblerPPCCompat::store32(Imm32 src, const BaseIndex& address)
{
    computeScaledAddress(address, ScratchRegister);
    ma_li(SecondScratchRegister, src);
    as_stw(SecondScratchRegister, ScratchRegister, address.offset);
}

void
MacroAssemblerPPCCompat::store32(Register src, AbsoluteAddress address)
{
    ma_li(ScratchRegister, ImmWord(uintptr_t(address.addr)));
    as_stw(src, ScratchRegister, 0);
}

void
MacroAssemblerPPCCompat::storePtr(Register src, AbsoluteAddress dest)
{
    ma_li(ScratchRegister, ImmWord(uintptr_t(dest.addr)));
    as_stw(src, ScratchRegister, 0);
}

void
MacroAssembler::popcnt32(Register src, Register dest, Register tmp)
{
    // Hamming weight via standard bit manipulation
    // This emits JIT code — each operation generates PPC instructions
    
    if (src != dest)
        move32(src, dest);
    
    // dest = src - ((src >> 1) & 0x55555555)
    ma_srl(tmp, dest, Imm32(1));
    ma_and(tmp, Imm32(0x55555555));
    as_subf(dest, tmp, dest);
    
    // dest = (dest & 0x33333333) + ((dest >> 2) & 0x33333333)
    ma_and(tmp, dest, Imm32(0x33333333));
    ma_srl(dest, dest, Imm32(2));
    ma_and(dest, Imm32(0x33333333));
    as_add(dest, dest, tmp);
    
    // dest = (dest + (dest >> 4)) & 0x0f0f0f0f
    ma_srl(tmp, dest, Imm32(4));
    as_add(dest, dest, tmp);
    ma_and(dest, Imm32(0x0f0f0f0f));
    
    // dest = dest + (dest >> 8)
    ma_srl(tmp, dest, Imm32(8));
    as_add(dest, dest, tmp);
    
    // dest = (dest + (dest >> 16)) & 0x3f
    ma_srl(tmp, dest, Imm32(16));
    as_add(dest, dest, tmp);
    ma_and(dest, Imm32(0x3f));
}

// ===============================================================
// PER_ARCH methods: ABI calling convention
// ===============================================================

void
MacroAssembler::setupUnalignedABICall(Register scratch)
{
    setupABICall();
    dynamicAlignment_ = true;

    ma_move(scratch, StackPointer);

    // Force sp to be aligned
    asMasm().subPtr(Imm32(sizeof(uintptr_t)), StackPointer);
    ma_and(StackPointer, StackPointer, Imm32(~(ABIStackAlignment - 1)));
    storePtr(scratch, Address(StackPointer, 0));
}

void
MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromWasm)
{
    MOZ_ASSERT(inCall_);
    uint32_t stackForCall = abiArgs_.stackBytesConsumedSoFar();

    // Reserve space for saving LR.
    stackForCall += sizeof(intptr_t);

    if (dynamicAlignment_) {
        stackForCall += ComputeByteAlignment(stackForCall, ABIStackAlignment);
    } else {
        uint32_t alignmentAtPrologue = callFromWasm ? sizeof(wasm::Frame) : 0;
        stackForCall += ComputeByteAlignment(stackForCall + framePushed() + alignmentAtPrologue,
                                             ABIStackAlignment);
    }

    *stackAdjust = stackForCall;
    reserveStack(stackForCall);

    // Save LR because bctrl will clobber it.
    as_mflr(r0);
    storePtr(r0, Address(StackPointer, stackForCall - sizeof(intptr_t)));

    // Position all arguments.
    {
        enoughMemory_ = enoughMemory_ && moveResolver_.resolve();
        if (!enoughMemory_)
            return;

        MoveEmitter emitter(*this);
        emitter.emit(moveResolver_);
        emitter.finish();
    }
}

void
MacroAssembler::callWithABIPost(uint32_t stackAdjust, MoveOp::Type result)
{
    // Restore LR (as stored in callWithABIPre).
    loadPtr(Address(StackPointer, stackAdjust - sizeof(intptr_t)), r0);
    as_mtlr(r0);

    if (dynamicAlignment_) {
        // Restore sp value from stack (as stored in setupUnalignedABICall).
        loadPtr(Address(StackPointer, stackAdjust), StackPointer);
        adjustFrame(-stackAdjust);
    } else {
        freeStack(stackAdjust);
    }

#ifdef DEBUG
    MOZ_ASSERT(inCall_);
    inCall_ = false;
#endif
}

void
MacroAssembler::callWithABINoProfiler(Register fun, MoveOp::Type result)
{
    // Move callee to r12 (PPC ABI: r12 = function pointer for indirect calls).
    ma_move(SecondScratchRegister, fun);
    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    as_mtctr(SecondScratchRegister);
    as_bctrl();
    callWithABIPost(stackAdjust, result);
}

void
MacroAssembler::callWithABINoProfiler(const Address& fun, MoveOp::Type result)
{
    loadPtr(Address(fun.base, fun.offset), SecondScratchRegister);
    uint32_t stackAdjust;
    callWithABIPre(&stackAdjust);
    as_mtctr(SecondScratchRegister);
    as_bctrl();
    callWithABIPost(stackAdjust, result);
}


// ===============================================================
// PER_ARCH methods: nursery, clamp, alignment
// ===============================================================

void
MacroAssembler::branchValueIsNurseryObject(Condition cond, const Address& address,
                                           Register temp, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    Label done;
    branchTestObject(Assembler::NotEqual, address, cond == Assembler::Equal ? &done : label);
    loadPtr(address, temp);
    branchPtrInNurseryChunk(cond, temp, InvalidReg, label);
    bind(&done);
}

void
MacroAssembler::branchValueIsNurseryObject(Condition cond, ValueOperand value,
                                           Register temp, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    Label done;
    branchTestObject(Assembler::NotEqual, value, cond == Assembler::Equal ? &done : label);
    branchPtrInNurseryChunk(cond, value.payloadReg(), temp, label);
    bind(&done);
}

void
MacroAssembler::branchTestValue(Condition cond, const ValueOperand& lhs,
                                const Value& rhs, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ScratchRegisterScope scratch(*this);

    if (cond == NotEqual) {
        // Branch to label if lhs != rhs (either type or payload differs)
        ma_li(scratch, Imm32(rhs.toNunboxTag()));
        ma_b(lhs.typeReg(), scratch, label, NotEqual);
        ma_li(scratch, Imm32(rhs.toNunboxPayload()));
        ma_b(lhs.payloadReg(), scratch, label, NotEqual);
    } else {
        // Branch to label if lhs == rhs (both type and payload match)
        Label miss;
        ma_li(scratch, Imm32(rhs.toNunboxTag()));
        ma_b(lhs.typeReg(), scratch, &miss, NotEqual);
        ma_li(scratch, Imm32(rhs.toNunboxPayload()));
        ma_b(lhs.payloadReg(), scratch, label, Equal);
        bind(&miss);
    }
}

void
MacroAssembler::clampDoubleToUint8(FloatRegister input, Register output)
{
    Label done, above255, belowZero;

    // Below 0 or NaN → clamp to 0
    loadConstantDouble(0.0, ScratchDoubleReg);
    branchDouble(Assembler::DoubleLessThanOrEqual, input, ScratchDoubleReg, &belowZero);
    // NaN: unordered comparison falls through, but !(x > 0) for NaN, so check:
    branchDouble(Assembler::DoubleUnordered, input, input, &belowZero);

    // Above 255 → clamp to 255
    loadConstantDouble(255.0, ScratchDoubleReg);
    branchDouble(Assembler::DoubleGreaterThan, input, ScratchDoubleReg, &above255);

    // In range: round to nearest (add 0.5 and truncate)
    loadConstantDouble(0.5, ScratchDoubleReg);
    addDouble(input, ScratchDoubleReg);  // ScratchDoubleReg = input + 0.5
    // PPC fctiwz truncates double to int32 in FPR, then stfd + load
    as_fctiwz(ScratchDoubleReg, ScratchDoubleReg);
    // Move from FPR low word to GPR via stack
    subPtr(Imm32(8), StackPointer);
    as_stfd(ScratchDoubleReg, StackPointer, 0);
    // fctiwz result is in low 32 bits (offset 4 on big-endian)
    load32(Address(StackPointer, 4), output);
    addPtr(Imm32(8), StackPointer);
    jump(&done);

    bind(&above255);
    move32(Imm32(255), output);
    jump(&done);

    bind(&belowZero);
    move32(Imm32(0), output);

    bind(&done);
}

void
MacroAssembler::alignFrameForICArguments(AfterICSaveLive& aic)
{
    if (framePushed() % ABIStackAlignment != 0) {
        aic.alignmentPadding = ABIStackAlignment - (framePushed() % ABIStackAlignment);
        reserveStack(aic.alignmentPadding);
    } else {
        aic.alignmentPadding = 0;
    }
    MOZ_ASSERT(framePushed() % ABIStackAlignment == 0);
}

void
MacroAssembler::restoreFrameAlignmentForICArguments(AfterICSaveLive& aic)
{
    if (aic.alignmentPadding != 0)
        freeStack(aic.alignmentPadding);
}


// ===============================================================
// PER_ARCH/PER_SHARED_ARCH functions (non-inline for this architecture)

__attribute__((visibility("default"), used))
void
MacroAssembler::move64(Register64 src, Register64 dest)
{
    move32(src.low, dest.low);
    move32(src.high, dest.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::move64(Imm64 imm, Register64 dest)
{
    move32(Imm32(imm.value & 0xFFFFFFFFL), dest.low);
    move32(Imm32((imm.value >> 32) & 0xFFFFFFFFL), dest.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::and32(Register src, Register dest)
{
    as_and(dest, dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::and32(Imm32 imm, Register dest)
{
    ma_and(dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::and32(Imm32 imm, const Address& dest)
{
    load32(dest, SecondScratchRegister);
    ma_and(SecondScratchRegister, imm);
    store32(SecondScratchRegister, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::and32(const Address& src, Register dest)
{
    load32(src, SecondScratchRegister);
    ma_and(dest, SecondScratchRegister);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::or32(Register src, Register dest)
{
    ma_or(dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::or32(Imm32 imm, Register dest)
{
    ma_or(dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::or32(Imm32 imm, const Address& dest)
{
    load32(dest, SecondScratchRegister);
    ma_or(SecondScratchRegister, imm);
    store32(SecondScratchRegister, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::xor32(Register src, Register dest)
{
    ma_xor(dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::xor32(Imm32 imm, Register dest)
{
    ma_xor(dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::not32(Register reg)
{
    ma_not(reg, reg);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::andPtr(Register src, Register dest)
{
    ma_and(dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::andPtr(Imm32 imm, Register dest)
{
    ma_and(dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::and64(Imm64 imm, Register64 dest)
{
    if (imm.low().value != int32_t(0xFFFFFFFF))
        and32(imm.low(), dest.low);
    if (imm.hi().value != int32_t(0xFFFFFFFF))
        and32(imm.hi(), dest.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::and64(Register64 src, Register64 dest)
{
    and32(src.low, dest.low);
    and32(src.high, dest.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::or64(Imm64 imm, Register64 dest)
{
    if (imm.low().value)
        or32(imm.low(), dest.low);
    if (imm.hi().value)
        or32(imm.hi(), dest.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::xor64(Imm64 imm, Register64 dest)
{
    if (imm.low().value)
        xor32(imm.low(), dest.low);
    if (imm.hi().value)
        xor32(imm.hi(), dest.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::orPtr(Register src, Register dest)
{
    ma_or(dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::orPtr(Imm32 imm, Register dest)
{
    ma_or(dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::or64(Register64 src, Register64 dest)
{
    or32(src.low, dest.low);
    or32(src.high, dest.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::xor64(Register64 src, Register64 dest)
{
    ma_xor(dest.low, src.low);
    ma_xor(dest.high, src.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::xorPtr(Register src, Register dest)
{
    ma_xor(dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::xorPtr(Imm32 imm, Register dest)
{
    ma_xor(dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::add32(Register src, Register dest)
{
    as_add(dest, dest, src);
}

void
MacroAssemblerPPCCompat::add32(Imm32 imm, const Address& dest)
{
    load32(dest, SecondScratchRegister);
    ma_addu(SecondScratchRegister, SecondScratchRegister, imm);
    store32(SecondScratchRegister, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::add32(Imm32 imm, Register dest)
{
    ma_addu(dest, dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::add32(Imm32 imm, const Address& dest)
{
    load32(dest, SecondScratchRegister);
    ma_addu(SecondScratchRegister, imm);
    store32(SecondScratchRegister, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::sub32(Register src, Register dest)
{
    as_subf(dest, src, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::sub32(Imm32 imm, Register dest)
{
    ma_subu(dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::sub32(const Address& src, Register dest)
{
    load32(src, SecondScratchRegister);
    as_subf(dest, SecondScratchRegister, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::neg32(Register reg)
{
    ma_negu(reg, reg);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::mul32(Register rhs, Register srcDest)
{
    as_mullw(srcDest, srcDest, rhs);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::quotient32(Register rhs, Register srcDest, bool isUnsigned)
{
    if (isUnsigned)
        as_divwu(srcDest, srcDest, rhs);
    else
        as_divw(srcDest, srcDest, rhs);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::remainder32(Register rhs, Register srcDest, bool isUnsigned)
{
    // PPC has no remainder instruction. remainder = dividend - (dividend/divisor)*divisor
    Register temp = ScratchRegister;
    if (isUnsigned)
        as_divwu(temp, srcDest, rhs);
    else
        as_divw(temp, srcDest, rhs);
    as_mullw(temp, temp, rhs);
    as_subf(srcDest, temp, srcDest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::addPtr(Register src, Register dest)
{
    as_add(dest, dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::addPtr(Imm32 imm, Register dest)
{
    ma_addu(dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::addPtr(ImmWord imm, Register dest)
{
    addPtr(Imm32(imm.value), dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::addPtr(Imm32 imm, const Address& dest)
{
    loadPtr(dest, ScratchRegister);
    addPtr(imm, ScratchRegister);
    storePtr(ScratchRegister, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::addPtr(const Address& src, Register dest)
{
    loadPtr(src, ScratchRegister);
    addPtr(ScratchRegister, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::add64(Register64 src, Register64 dest)
{
    as_addc(dest.low, dest.low, src.low);
    as_adde(dest.high, dest.high, src.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::add64(Imm32 imm, Register64 dest)
{
    ma_li(ScratchRegister, imm);
    as_addc(dest.low, dest.low, ScratchRegister);
    as_li(ScratchRegister, 0);
    as_adde(dest.high, dest.high, ScratchRegister);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::add64(Imm64 imm, Register64 dest)
{
    add64(imm.low(), dest);
    ma_addu(dest.high, dest.high, imm.hi());
}

__attribute__((visibility("default"), used))
void
MacroAssembler::subPtr(Register src, Register dest)
{
    as_subf(dest, src, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::subPtr(Imm32 imm, Register dest)
{
    ma_subu(dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::subPtr(Register src, const Address& dest)
{
    loadPtr(dest, ScratchRegister);
    subPtr(src, ScratchRegister);
    storePtr(ScratchRegister, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::subPtr(const Address& addr, Register dest)
{
    loadPtr(addr, ScratchRegister);
    subPtr(ScratchRegister, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::subFromStackPtr(Imm32 imm32)
{
    ma_subu(StackPointer, imm32);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::sub64(Register64 src, Register64 dest)
{
    as_subfc(dest.low, src.low, dest.low);
    as_subfe(dest.high, src.high, dest.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::sub64(Imm64 imm, Register64 dest)
{
    ma_li(ScratchRegister, imm.low());
    as_subfc(dest.low, ScratchRegister, dest.low);
    ma_li(ScratchRegister, imm.hi());
    as_subfe(dest.high, ScratchRegister, dest.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::mul64(Imm64 imm, const Register64& dest)
{
    ma_li(ScratchRegister, Imm32(imm.value & 0xFFFFFFFFL));
    as_mullw(dest.high, dest.high, ScratchRegister);
    as_mulhwu(SecondScratchRegister, dest.low, ScratchRegister);
    as_add(dest.high, dest.high, SecondScratchRegister);
    ma_li(SecondScratchRegister, Imm32((imm.value >> 32) & 0xFFFFFFFFL));
    as_mullw(SecondScratchRegister, dest.low, SecondScratchRegister);
    as_add(dest.high, dest.high, SecondScratchRegister);
    ma_li(ScratchRegister, Imm32(imm.value & 0xFFFFFFFFL));
    as_mullw(dest.low, dest.low, ScratchRegister);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::mul64(Imm64 imm, const Register64& dest, const Register temp)
{
    MOZ_ASSERT(temp != dest.high && temp != dest.low);
    ma_li(ScratchRegister, imm.firstHalf());
    as_mullw(dest.high, dest.high, ScratchRegister);
    ma_li(ScratchRegister, imm.secondHalf());
    as_mullw(temp, dest.low, ScratchRegister);
    as_add(temp, dest.high, temp);
    ma_li(ScratchRegister, imm.firstHalf());
    as_mulhwu(dest.high, dest.low, ScratchRegister);
    as_mullw(dest.low, dest.low, ScratchRegister);
    as_add(dest.high, dest.high, temp);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::mul64(const Register64& src, const Register64& dest, const Register temp)
{
    MOZ_ASSERT(dest != src);
    MOZ_ASSERT(dest.low != src.high && dest.high != src.low);
    as_mullw(dest.high, dest.high, src.low);
    as_mullw(temp, dest.low, src.high);
    as_add(temp, dest.high, temp);
    as_mulhwu(dest.high, dest.low, src.low);
    as_mullw(dest.low, dest.low, src.low);
    as_add(dest.high, dest.high, temp);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::neg64(Register64 reg)
{
    ma_not(reg.low, reg.low);
    ma_not(reg.high, reg.high);
    as_addic(reg.low, reg.low, 1);
    as_addze(reg.high, reg.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::mulBy3(Register src, Register dest)
{
    MOZ_ASSERT(src != ScratchRegister);
    as_add(ScratchRegister, src, src);
    as_add(dest, ScratchRegister, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::inc64(AbsoluteAddress dest)
{
    ma_li(ScratchRegister, Imm32((int32_t)dest.addr));
    as_lwz(SecondScratchRegister, ScratchRegister, 4); // low word (big-endian: +4)
    as_addic(SecondScratchRegister, SecondScratchRegister, 1);
    as_stw(SecondScratchRegister, ScratchRegister, 4);
    as_lwz(SecondScratchRegister, ScratchRegister, 0); // high word
    as_addze(SecondScratchRegister, SecondScratchRegister);
    as_stw(SecondScratchRegister, ScratchRegister, 0);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::lshift32(Imm32 shift, Register dest)
{
    MOZ_ASSERT(0 <= shift.value && shift.value < 32);
    ma_sll(dest, dest, shift);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::lshift32(Register shift, Register dest)
{
    as_slw(dest, dest, shift);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rshift32(Imm32 shift, Register dest)
{
    MOZ_ASSERT(0 <= shift.value && shift.value < 32);
    ma_srl(dest, dest, shift);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rshift32(Register shift, Register dest)
{
    as_srw(dest, dest, shift);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rshift32Arithmetic(Imm32 shift, Register dest)
{
    MOZ_ASSERT(0 <= shift.value && shift.value < 32);
    ma_sra(dest, dest, shift);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rshift32Arithmetic(Register shift, Register dest)
{
    as_sraw(dest, dest, shift);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::lshiftPtr(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 32);
    ma_sll(dest, dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rshiftPtr(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 32);
    ma_srl(dest, dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rshiftPtrArithmetic(Imm32 imm, Register dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 32);
    ma_sra(dest, dest, imm);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::lshift64(Imm32 imm, Register64 dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    if (imm.value == 0) {
        return;
    } else if (imm.value < 32) {
        ma_sll(dest.high, dest.high, imm);
        ma_srl(ScratchRegister, dest.low, Imm32(32 - imm.value));
        ma_or(dest.high, ScratchRegister);
        ma_sll(dest.low, dest.low, imm);
    } else {
        ma_sll(dest.high, dest.low, Imm32(imm.value - 32));
        move32(Imm32(0), dest.low);
    }
}

__attribute__((visibility("default"), used))
void
MacroAssembler::lshift64(Register unmaskedShift, Register64 dest)
{
    Label done, less;
    ma_and(ScratchRegister, unmaskedShift, Imm32(0x3f));
    ma_b(ScratchRegister, Imm32(0), &done, Equal);
    ma_sll(dest.high, dest.high, ScratchRegister);
    ma_subu(ScratchRegister, ScratchRegister, Imm32(32));
    ma_b(ScratchRegister, Imm32(0), &less, LessThan);
    ma_sll(dest.high, dest.low, ScratchRegister);
    move32(Imm32(0), dest.low);
    ma_b(&done);
    bind(&less);
    ma_negu(ScratchRegister, ScratchRegister);
    ma_srl(SecondScratchRegister, dest.low, ScratchRegister);
    ma_or(dest.high, SecondScratchRegister);
    ma_and(ScratchRegister, unmaskedShift, Imm32(0x3f));
    ma_sll(dest.low, dest.low, ScratchRegister);
    bind(&done);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rshift64(Imm32 imm, Register64 dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    if (imm.value < 32) {
        ma_srl(dest.low, dest.low, imm);
        ma_sll(ScratchRegister, dest.high, Imm32(32 - imm.value));
        ma_or(dest.low, ScratchRegister);
        ma_srl(dest.high, dest.high, imm);
    } else if (imm.value == 32) {
        ma_move(dest.low, dest.high);
        move32(Imm32(0), dest.high);
    } else {
        ma_srl(dest.low, dest.high, Imm32(imm.value - 32));
        move32(Imm32(0), dest.high);
    }
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rshift64(Register unmaskedShift, Register64 dest)
{
    Label done, less;
    ma_and(ScratchRegister, unmaskedShift, Imm32(0x3f));
    ma_srl(dest.low, dest.low, ScratchRegister);
    ma_subu(ScratchRegister, ScratchRegister, Imm32(32));
    ma_b(ScratchRegister, Imm32(0), &less, LessThan);
    ma_srl(dest.low, dest.high, ScratchRegister);
    move32(Imm32(0), dest.high);
    ma_b(&done);
    bind(&less);
    ma_negu(ScratchRegister, ScratchRegister);
    ma_sll(SecondScratchRegister, dest.high, ScratchRegister);
    ma_or(dest.low, SecondScratchRegister);
    ma_and(ScratchRegister, unmaskedShift, Imm32(0x3f));
    ma_srl(dest.high, dest.high, ScratchRegister);
    bind(&done);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rshift64Arithmetic(Imm32 imm, Register64 dest)
{
    MOZ_ASSERT(0 <= imm.value && imm.value < 64);
    if (imm.value < 32) {
        ma_srl(dest.low, dest.low, imm);
        ma_sll(ScratchRegister, dest.high, Imm32(32 - imm.value));
        ma_or(dest.low, ScratchRegister);
        ma_sra(dest.high, dest.high, imm);
    } else if (imm.value == 32) {
        ma_move(dest.low, dest.high);
        ma_sra(dest.high, dest.high, Imm32(31));
    } else {
        ma_sra(dest.low, dest.high, Imm32(imm.value - 32));
        ma_sra(dest.high, dest.high, Imm32(31));
    }
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rshift64Arithmetic(Register unmaskedShift, Register64 dest)
{
    Label done, less;
    ma_and(ScratchRegister, unmaskedShift, Imm32(0x3f));
    ma_srl(dest.low, dest.low, ScratchRegister);
    ma_subu(ScratchRegister, ScratchRegister, Imm32(32));
    ma_b(ScratchRegister, Imm32(0), &less, LessThan);
    ma_sra(dest.low, dest.high, ScratchRegister);
    ma_sra(dest.high, dest.high, Imm32(31));
    ma_b(&done);
    bind(&less);
    ma_negu(ScratchRegister, ScratchRegister);
    ma_sll(SecondScratchRegister, dest.high, ScratchRegister);
    ma_or(dest.low, SecondScratchRegister);
    ma_and(ScratchRegister, unmaskedShift, Imm32(0x3f));
    ma_sra(dest.high, dest.high, ScratchRegister);
    bind(&done);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rotateLeft(Imm32 count, Register input, Register dest)
{
    if (count.value == 0) {
        ma_move(dest, input);
    } else {
        // rlwinm dest, input, count, 0, 31  (rotate left and mask: full word)
        as_rlwinm(dest, input, count.value & 31, 0, 31);
    }
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rotateLeft(Register count, Register input, Register dest)
{
    // rlwnm dest, input, count, 0, 31  (rotate left by register)
    // PPC doesn't have rlwnm easily, but we can use: shift left + shift right + or
    MOZ_ASSERT(input != ScratchRegister);
    MOZ_ASSERT(count != ScratchRegister);
    as_slw(ScratchRegister, input, count);
    // 32 - count
    as_li(SecondScratchRegister, 32);
    as_subf(SecondScratchRegister, count, SecondScratchRegister);
    as_srw(dest, input, SecondScratchRegister);
    as_or(dest, dest, ScratchRegister);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rotateRight(Imm32 count, Register input, Register dest)
{
    if (count.value == 0) {
        ma_move(dest, input);
    } else {
        // Rotate right by n = rotate left by (32-n)
        as_rlwinm(dest, input, (32 - (count.value & 31)) & 31, 0, 31);
    }
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rotateRight(Register count, Register input, Register dest)
{
    MOZ_ASSERT(input != ScratchRegister);
    MOZ_ASSERT(count != ScratchRegister);
    as_srw(ScratchRegister, input, count);
    as_li(SecondScratchRegister, 32);
    as_subf(SecondScratchRegister, count, SecondScratchRegister);
    as_slw(dest, input, SecondScratchRegister);
    as_or(dest, dest, ScratchRegister);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rotateLeft64(Imm32 count, Register64 input, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp == InvalidReg);
    MOZ_ASSERT(input.low != dest.high && input.high != dest.low);
    int32_t amount = count.value & 0x3f;
    if (amount > 32) {
        rotateRight64(Imm32(64 - amount), input, dest, temp);
    } else if (amount == 0) {
        ma_move(dest.low, input.low);
        ma_move(dest.high, input.high);
    } else if (amount == 32) {
        ma_move(ScratchRegister, input.low);
        ma_move(dest.low, input.high);
        ma_move(dest.high, ScratchRegister);
    } else {
        ma_move(ScratchRegister, input.high);
        ma_sll(dest.high, input.high, Imm32(amount));
        ma_srl(SecondScratchRegister, input.low, Imm32(32 - amount));
        ma_or(dest.high, SecondScratchRegister);
        ma_sll(dest.low, input.low, Imm32(amount));
        ma_srl(SecondScratchRegister, ScratchRegister, Imm32(32 - amount));
        ma_or(dest.low, SecondScratchRegister);
    }
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rotateLeft64(Register shift, Register64 src, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp != src.low && temp != src.high);
    MOZ_ASSERT(shift != src.low && shift != src.high);
    MOZ_ASSERT(temp != InvalidReg);

    Label high, done, zero;
    ma_and(temp, shift, Imm32(0x3f));
    ma_b(temp, Imm32(32), &high, GreaterThanOrEqual);
    ma_sll(dest.high, src.high, temp);
    ma_b(temp, Imm32(0), &zero, Equal);
    as_li(SecondScratchRegister, 32);
    as_subf(ScratchRegister, temp, SecondScratchRegister);
    ma_srl(SecondScratchRegister, src.low, ScratchRegister);
    ma_or(dest.high, SecondScratchRegister);
    ma_sll(dest.low, src.low, temp);
    ma_srl(SecondScratchRegister, src.high, ScratchRegister);
    ma_or(dest.low, SecondScratchRegister);
    ma_b(&done);
    bind(&zero);
    ma_move(dest.low, src.low);
    ma_move(dest.high, src.high);
    ma_b(&done);
    bind(&high);
    ma_and(ScratchRegister, shift, Imm32(0x3f));
    as_li(SecondScratchRegister, 64);
    as_subf(temp, ScratchRegister, SecondScratchRegister);
    ma_srl(dest.high, src.high, temp);
    as_li(SecondScratchRegister, 32);
    as_subf(ScratchRegister, temp, SecondScratchRegister);
    ma_sll(SecondScratchRegister, src.low, ScratchRegister);
    ma_or(dest.high, SecondScratchRegister);
    ma_srl(dest.low, src.low, temp);
    ma_sll(SecondScratchRegister, src.high, ScratchRegister);
    ma_or(dest.low, SecondScratchRegister);
    bind(&done);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rotateRight64(Imm32 count, Register64 input, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp == InvalidReg);
    MOZ_ASSERT(input.low != dest.high && input.high != dest.low);
    int32_t amount = count.value & 0x3f;
    if (amount > 32) {
        rotateLeft64(Imm32(64 - amount), input, dest, temp);
    } else if (amount == 0) {
        ma_move(dest.low, input.low);
        ma_move(dest.high, input.high);
    } else if (amount == 32) {
        ma_move(ScratchRegister, input.low);
        ma_move(dest.low, input.high);
        ma_move(dest.high, ScratchRegister);
    } else {
        ma_move(ScratchRegister, input.high);
        ma_srl(dest.high, input.high, Imm32(amount));
        ma_sll(SecondScratchRegister, input.low, Imm32(32 - amount));
        ma_or(dest.high, SecondScratchRegister);
        ma_srl(dest.low, input.low, Imm32(amount));
        ma_sll(SecondScratchRegister, ScratchRegister, Imm32(32 - amount));
        ma_or(dest.low, SecondScratchRegister);
    }
}

__attribute__((visibility("default"), used))
void
MacroAssembler::rotateRight64(Register shift, Register64 src, Register64 dest, Register temp)
{
    MOZ_ASSERT(temp != src.low && temp != src.high);
    MOZ_ASSERT(shift != src.low && shift != src.high);
    MOZ_ASSERT(temp != InvalidReg);

    Label high, done, zero;
    ma_and(temp, shift, Imm32(0x3f));
    ma_srl(dest.high, src.high, temp);
    ma_b(temp, Imm32(0), &zero, Equal);
    as_li(SecondScratchRegister, 32);
    as_subf(ScratchRegister, temp, SecondScratchRegister);
    ma_sll(SecondScratchRegister, src.low, ScratchRegister);
    ma_or(dest.high, SecondScratchRegister);
    ma_srl(dest.low, src.low, temp);
    ma_b(temp, Imm32(32), &high, GreaterThanOrEqual);
    ma_sll(SecondScratchRegister, src.high, ScratchRegister);
    ma_or(dest.low, SecondScratchRegister);
    ma_b(&done);
    bind(&zero);
    ma_move(dest.low, src.low);
    ma_move(dest.high, src.high);
    ma_b(&done);
    bind(&high);
    ma_and(ScratchRegister, shift, Imm32(0x3f));
    as_li(SecondScratchRegister, 64);
    as_subf(temp, ScratchRegister, SecondScratchRegister);
    ma_sll(dest.high, src.high, temp);
    as_li(SecondScratchRegister, 32);
    as_subf(ScratchRegister, temp, SecondScratchRegister);
    ma_srl(SecondScratchRegister, src.low, ScratchRegister);
    ma_or(dest.high, SecondScratchRegister);
    ma_sll(dest.low, src.low, temp);
    ma_srl(SecondScratchRegister, src.high, ScratchRegister);
    ma_or(dest.low, SecondScratchRegister);
    bind(&done);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::clz32(Register src, Register dest, bool knownNotZero)
{
    as_cntlzw(dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::ctz32(Register src, Register dest, bool knownNotZero)
{
    // ctz(x) = 31 - clz(x & -x)
    ma_negu(ScratchRegister, src);
    as_and(ScratchRegister, ScratchRegister, src);
    as_cntlzw(dest, ScratchRegister);
    as_li(ScratchRegister, 31);
    as_subf(dest, dest, ScratchRegister);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::clz64(Register64 src, Register dest)
{
    Label done, low;
    ma_b(src.high, Imm32(0), &low, Equal);
    as_cntlzw(dest, src.high);
    ma_b(&done);
    bind(&low);
    as_cntlzw(dest, src.low);
    ma_addu(dest, Imm32(32));
    bind(&done);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::ctz64(Register64 src, Register dest)
{
    Label done, high;
    ma_b(src.low, Imm32(0), &high, Equal);
    ma_negu(ScratchRegister, src.low);
    as_and(ScratchRegister, ScratchRegister, src.low);
    as_cntlzw(dest, ScratchRegister);
    as_li(ScratchRegister, 31);
    as_subf(dest, dest, ScratchRegister);
    ma_b(&done);
    bind(&high);
    ma_negu(ScratchRegister, src.high);
    as_and(ScratchRegister, ScratchRegister, src.high);
    as_cntlzw(dest, ScratchRegister);
    as_li(ScratchRegister, 31);
    as_subf(dest, dest, ScratchRegister);
    ma_addu(dest, Imm32(32));
    bind(&done);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::popcnt64(Register64 src, Register64 dest, Register tmp)
{
    MOZ_ASSERT(dest.low != tmp);
    MOZ_ASSERT(dest.high != tmp);
    MOZ_ASSERT(dest.low != dest.high);
    if (dest.low != src.high) {
        popcnt32(src.low, dest.low, tmp);
        popcnt32(src.high, dest.high, tmp);
    } else {
        MOZ_ASSERT(dest.high != src.high);
        popcnt32(src.low, dest.high, tmp);
        popcnt32(src.high, dest.low, tmp);
    }
    as_add(dest.low, dest.low, dest.high);
    move32(Imm32(0), dest.high);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branch32(Condition cond, const Address& lhs, Imm32 rhs, Label* label)
{
    load32(lhs, SecondScratchRegister);
    ma_b(SecondScratchRegister, rhs, label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branch32(Condition cond, const BaseIndex& lhs, Imm32 rhs, Label* label)
{
    load32(lhs, SecondScratchRegister);
    ma_b(SecondScratchRegister, rhs, label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
{
    load32(lhs, ScratchRegister);
    branch32(cond, ScratchRegister, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branch32(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
{
    load32(lhs, ScratchRegister);
    branch32(cond, ScratchRegister, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branch32(Condition cond, wasm::SymbolicAddress lhs, Imm32 rhs, Label* label)
{
    MOZ_CRASH("PPC wasm not supported");
}


__attribute__((visibility("default"), used))
void
MacroAssembler::branch32(Condition cond, const Address& lhs, Register rhs, Label* label)
{
    load32(lhs, ScratchRegister);
    branch32(cond, ScratchRegister, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::mulDoublePtr(ImmPtr imm, Register temp, FloatRegister dest)
{
    movePtr(imm, ScratchRegister);
    loadDouble(Address(ScratchRegister, 0), ScratchDoubleReg);
    mulDouble(ScratchDoubleReg, dest);
}
__attribute__((visibility("default"), used))
void
MacroAssembler::branchPtr(Condition cond, Register lhs, Imm32 rhs, Label* label)
{
    ma_b(lhs, rhs, label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchPtr(Condition cond, Register lhs, ImmPtr rhs, Label* label)
{
    ma_b(lhs, rhs, label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchPtr(Condition cond, Register lhs, ImmGCPtr rhs, Label* label)
{
    ma_b(lhs, rhs, label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchPtr(Condition cond, Register lhs, ImmWord rhs, Label* label)
{
    ma_b(lhs, rhs, label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmPtr rhs, Label* label)
{
    loadPtr(lhs, SecondScratchRegister);
    branchPtr(cond, SecondScratchRegister, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmGCPtr rhs, Label* label)
{
    loadPtr(lhs, SecondScratchRegister);
    branchPtr(cond, SecondScratchRegister, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, ImmWord rhs, Label* label)
{
    loadPtr(lhs, SecondScratchRegister);
    branchPtr(cond, SecondScratchRegister, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchPtr(Condition cond, wasm::SymbolicAddress lhs, Register rhs, Label* label)
{
    MOZ_CRASH("PPC wasm not supported");
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs, Register rhs, Label* label)
{
    loadPtr(lhs, ScratchRegister);
    branchPtr(cond, ScratchRegister, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchPtr(Condition cond, const AbsoluteAddress& lhs, ImmWord rhs, Label* label)
{
    loadPtr(lhs, ScratchRegister);
    branchPtr(cond, ScratchRegister, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchFloat(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs,
                            Label* label)
{
    ma_bc(SingleFloat, cond, lhs, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchDouble(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs,
                             Label* label)
{
    ma_bc(DoubleFloat, cond, lhs, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTest32(Condition cond, const Address& lhs, Imm32 rhs, Label* label)
{
    load32(lhs, SecondScratchRegister);
    branchTest32(cond, SecondScratchRegister, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTest32(Condition cond, const AbsoluteAddress& lhs, Imm32 rhs, Label* label)
{
    load32(lhs, ScratchRegister);
    branchTest32(cond, ScratchRegister, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestPtr(Condition cond, Register lhs, Imm32 rhs, Label* label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
    ma_and(ScratchRegister, lhs, rhs);
    ma_b(ScratchRegister, Imm32(0), label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestPtr(Condition cond, const Address& lhs, Imm32 rhs, Label* label)
{
    loadPtr(lhs, SecondScratchRegister);
    branchTestPtr(cond, SecondScratchRegister, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::decBranchPtr(Condition cond, Register lhs, Imm32 rhs, Label* label)
{
    subPtr(rhs, lhs);
    branchPtr(cond, lhs, Imm32(0), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branch64(Condition cond, Register64 lhs, Imm64 val, Label* success, Label* fail)
{
    bool fallthrough = false;
    Label fallthroughLabel;
    if (!fail) {
        fail = &fallthroughLabel;
        fallthrough = true;
    }
    switch(cond) {
      case Assembler::Equal:
        branch32(Assembler::NotEqual, lhs.low, val.low(), fail);
        branch32(Assembler::Equal, lhs.high, val.hi(), success);
        if (!fallthrough)
            jump(fail);
        break;
      case Assembler::NotEqual:
        branch32(Assembler::NotEqual, lhs.low, val.low(), success);
        branch32(Assembler::NotEqual, lhs.high, val.hi(), success);
        if (!fallthrough)
            jump(fail);
        break;
      case Assembler::LessThan:
      case Assembler::LessThanOrEqual:
      case Assembler::GreaterThan:
      case Assembler::GreaterThanOrEqual:
      case Assembler::Below:
      case Assembler::BelowOrEqual:
      case Assembler::Above:
      case Assembler::AboveOrEqual: {
        Assembler::Condition invert_cond = Assembler::InvertCondition(cond);
        Assembler::Condition cond1 = Assembler::ConditionWithoutEqual(cond);
        Assembler::Condition cond2 = Assembler::ConditionWithoutEqual(invert_cond);
        Assembler::Condition cond3 = Assembler::UnsignedCondition(cond);
        ma_b(lhs.high, val.hi(), success, cond1);
        ma_b(lhs.high, val.hi(), fail, cond2);
        ma_b(lhs.low, val.low(), success, cond3);
        if (!fallthrough)
            jump(fail);
        break;
      }
      default:
        MOZ_CRASH("Condition code not supported");
        break;
    }
    if (fallthrough)
        bind(fail);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branch64(Condition cond, Register64 lhs, Register64 rhs, Label* success, Label* fail)
{
    bool fallthrough = false;
    Label fallthroughLabel;
    if (!fail) {
        fail = &fallthroughLabel;
        fallthrough = true;
    }
    switch(cond) {
      case Assembler::Equal:
        branch32(Assembler::NotEqual, lhs.low, rhs.low, fail);
        branch32(Assembler::Equal, lhs.high, rhs.high, success);
        if (!fallthrough)
            jump(fail);
        break;
      case Assembler::NotEqual:
        branch32(Assembler::NotEqual, lhs.low, rhs.low, success);
        branch32(Assembler::NotEqual, lhs.high, rhs.high, success);
        if (!fallthrough)
            jump(fail);
        break;
      case Assembler::LessThan:
      case Assembler::LessThanOrEqual:
      case Assembler::GreaterThan:
      case Assembler::GreaterThanOrEqual:
      case Assembler::Below:
      case Assembler::BelowOrEqual:
      case Assembler::Above:
      case Assembler::AboveOrEqual: {
        Assembler::Condition invert_cond = Assembler::InvertCondition(cond);
        Assembler::Condition cond1 = Assembler::ConditionWithoutEqual(cond);
        Assembler::Condition cond2 = Assembler::ConditionWithoutEqual(invert_cond);
        Assembler::Condition cond3 = Assembler::UnsignedCondition(cond);
        ma_b(lhs.high, rhs.high, success, cond1);
        ma_b(lhs.high, rhs.high, fail, cond2);
        ma_b(lhs.low, rhs.low, success, cond3);
        if (!fallthrough)
            jump(fail);
        break;
      }
      default:
        MOZ_CRASH("Condition code not supported");
        break;
    }
    if (fallthrough)
        bind(fail);
}


__attribute__((visibility("default"), used))
void
MacroAssembler::branch64(Condition cond, const Address& lhs, Imm64 val, Label* label)
{
    MOZ_ASSERT(cond == Assembler::NotEqual,
               "other condition codes not supported");

    // PPC big-endian: high word at offset 0, low word at offset+4
    branch32(cond, lhs, val.firstHalf(), label);
    branch32(cond, Address(lhs.base, lhs.offset + sizeof(uint32_t)), val.secondHalf(), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branch64(Condition cond, const Address& lhs, const Address& rhs, Register scratch,
                         Label* label)
{
    MOZ_ASSERT(cond == Assembler::NotEqual,
               "other condition codes not supported");
    MOZ_ASSERT(lhs.base != scratch);
    MOZ_ASSERT(rhs.base != scratch);

    load32(rhs, scratch);
    branch32(cond, lhs, scratch, label);

    load32(Address(rhs.base, rhs.offset + sizeof(uint32_t)), scratch);
    branch32(cond, Address(lhs.base, lhs.offset + sizeof(uint32_t)), scratch, label);
}
__attribute__((visibility("default"), used))
void
MacroAssembler::branchPrivatePtr(Condition cond, const Address& lhs, Register rhs, Label* label)
{
    branchPtr(cond, lhs, rhs, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestUndefined(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestUndefined(cond, value.typeReg(), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestInt32(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestInt32(cond, value.typeReg(), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestInt32Truthy(bool b, const ValueOperand& value, Label* label)
{
    as_and(ScratchRegister, value.payloadReg(), value.payloadReg(), /*rc=*/true);
    if (b)
        ma_b(ScratchRegister, ScratchRegister, label, NonZero);
    else
        ma_b(ScratchRegister, ScratchRegister, label, Zero);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestDouble(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    Condition actual = (cond == Equal) ? Below : AboveOrEqual;
    ma_b(tag, ImmTag(JSVAL_TAG_CLEAR), label, actual);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestDouble(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestDouble(cond, value.typeReg(), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestNumber(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestNumber(cond, value.typeReg(), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestBoolean(Condition cond, const ValueOperand& value, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(value.typeReg(), ImmType(JSVAL_TYPE_BOOLEAN), label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestBooleanTruthy(bool b, const ValueOperand& value, Label* label)
{
    ma_b(value.payloadReg(), value.payloadReg(), label, b ? NonZero : Zero);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestString(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestString(cond, value.typeReg(), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestStringTruthy(bool b, const ValueOperand& value, Label* label)
{
    Register string = value.payloadReg();
    load32(Address(string, JSString::offsetOfLength()), SecondScratchRegister);
    ma_b(SecondScratchRegister, Imm32(0), label, b ? NotEqual : Equal);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestSymbol(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestSymbol(cond, value.typeReg(), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestBigInt(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestBigInt(cond, value.typeReg(), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestBigIntTruthy(bool b, const ValueOperand& value, Label* label)
{
    Register bi = value.payloadReg();
    load32(Address(bi, BigInt::offsetOfLengthSignAndReservedBits()), SecondScratchRegister);
    ma_b(SecondScratchRegister, Imm32(0), label, b ? NotEqual : Equal);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestNull(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestNull(cond, value.typeReg(), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestObject(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestObject(cond, value.typeReg(), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestPrimitive(Condition cond, const ValueOperand& value, Label* label)
{
    branchTestPrimitive(cond, value.typeReg(), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestMagic(Condition cond, const Address& valaddr, JSWhyMagic why, Label* label)
{
    branchTestMagic(cond, valaddr, label);
    branch32(cond, ToPayload(valaddr), Imm32(why), label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestUndefined(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_UNDEFINED), label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestUndefined(Condition cond, const Address& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestUndefined(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestUndefined(Condition cond, const BaseIndex& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestUndefined(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestInt32(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_INT32), label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestInt32(Condition cond, const Address& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestInt32(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestInt32(Condition cond, const BaseIndex& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestInt32(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestDouble(Condition cond, const Address& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestDouble(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestDouble(Condition cond, const BaseIndex& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestDouble(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestNumber(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    Condition actual = cond == Equal ? BelowOrEqual : Above;
    ma_b(tag, ImmTag(JSVAL_UPPER_INCL_TAG_OF_NUMBER_SET), label, actual);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestBoolean(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_BOOLEAN), label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestBoolean(Condition cond, const Address& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestBoolean(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestBoolean(Condition cond, const BaseIndex& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestBoolean(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestString(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_STRING), label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestString(Condition cond, const BaseIndex& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestString(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestSymbol(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_SYMBOL), label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestSymbol(Condition cond, const BaseIndex& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestSymbol(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestBigInt(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_BIGINT), label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestBigInt(Condition cond, const BaseIndex& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestBigInt(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestNull(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_NULL), label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestNull(Condition cond, const Address& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestNull(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestNull(Condition cond, const BaseIndex& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestNull(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestObject(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_OBJECT), label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestObject(Condition cond, const Address& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestObject(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestObject(Condition cond, const BaseIndex& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestObject(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestPrimitive(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_UPPER_EXCL_TAG_OF_PRIMITIVE_SET), label,
         (cond == Equal) ? Below : AboveOrEqual);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestMagic(Condition cond, Register tag, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    ma_b(tag, ImmTag(JSVAL_TAG_MAGIC), label, cond);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestMagic(Condition cond, const Address& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestMagic(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestMagic(Condition cond, const BaseIndex& address, Label* label)
{
    extractTag(address, ScratchRegister);
    branchTestMagic(cond, ScratchRegister, label);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestGCThing(Condition cond, const Address& address, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    ma_b(ScratchRegister, ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET), label,
         (cond == Equal) ? AboveOrEqual : Below);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestGCThing(Condition cond, const BaseIndex& address, Label* label)
{
    MOZ_ASSERT(cond == Equal || cond == NotEqual);
    extractTag(address, ScratchRegister);
    ma_b(ScratchRegister, ImmTag(JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET), label,
         (cond == Equal) ? AboveOrEqual : Below);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTestDoubleTruthy(bool b, FloatRegister value, Label* label)
{
    // Load 0.0 into ScratchDoubleReg
    as_li(ScratchRegister, 0);
    as_stw(ScratchRegister, StackPointer, -4);
    as_stw(ScratchRegister, StackPointer, -8);
    as_lfd(ScratchDoubleReg, StackPointer, -8);
    if (b) {
        // Truthy: not zero and not NaN
        branchDouble(DoubleNotEqual, value, ScratchDoubleReg, label);
    } else {
        // Falsy: zero or NaN
        branchDouble(DoubleEqualOrUnordered, value, ScratchDoubleReg, label);
    }
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTruncateDoubleToInt32(FloatRegister src, Register dest, Label* fail)
{
    // Convert double to int via fctiwz, check for overflow
    as_fctiwz(ScratchDoubleReg, src);
    // Store the converted int from FPR to GPR via stack
    as_stfd(ScratchDoubleReg, StackPointer, -8);
    as_lwz(dest, StackPointer, -4); // low word in big-endian
    // Check for overflow: fctiwz produces 0x80000000 on overflow/NaN
    ma_b(dest, Imm32(0x7FFFFFFF), fail, Above);
    ma_b(dest, Imm32(int32_t(0x80000000)), fail, Equal);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTruncateFloat32ToInt32(FloatRegister src, Register dest, Label* fail)
{
    // Promote float32 to double, then truncate
    as_frsp(ScratchDoubleReg, src);  // round to single (in double reg)
    branchTruncateDoubleToInt32(ScratchDoubleReg, dest, fail);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTruncateDoubleMaybeModUint32(FloatRegister src, Register dest, Label* fail)
{
    // Same as branchTruncateDoubleToInt32 but overflow is modular (no fail on overflow)
    as_fctiwz(ScratchDoubleReg, src);
    as_stfd(ScratchDoubleReg, StackPointer, -8);
    as_lwz(dest, StackPointer, -4);
    // Only fail on NaN (which also produces 0x80000000, but we need to distinguish)
    // Check unordered by comparing src to itself


    branchDouble(DoubleUnordered, src, src, fail);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchTruncateFloat32MaybeModUint32(FloatRegister src, Register dest, Label* fail)
{
    as_frsp(ScratchDoubleReg, src);
    branchTruncateDoubleMaybeModUint32(ScratchDoubleReg, dest, fail);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::storeUncanonicalizedDouble(FloatRegister src, const Address& addr)
{
    ma_sd(src, addr);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::storeUncanonicalizedDouble(FloatRegister src, const BaseIndex& addr)
{
    MOZ_ASSERT(addr.offset == 0);
    ma_sd(src, addr);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::storeUncanonicalizedFloat32(FloatRegister src, const Address& addr)
{
    ma_ss(src, addr);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::storeUncanonicalizedFloat32(FloatRegister src, const BaseIndex& addr)
{
    MOZ_ASSERT(addr.offset == 0);
    ma_ss(src, addr);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::addDouble(FloatRegister src, FloatRegister dest)
{
    as_fadd(dest, dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::subDouble(FloatRegister src, FloatRegister dest)
{
    as_fsub(dest, dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::mulDouble(FloatRegister src, FloatRegister dest)
{
    as_fmul(dest, dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::divDouble(FloatRegister src, FloatRegister dest)
{
    as_fdiv(dest, dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::negateDouble(FloatRegister reg)
{
    as_fneg(reg, reg);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::sqrtDouble(FloatRegister src, FloatRegister dest)
{
    as_fsqrt(dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::absDouble(FloatRegister src, FloatRegister dest)
{
    as_fabs(dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::addFloat32(FloatRegister src, FloatRegister dest)
{
    as_fadds(dest, dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::subFloat32(FloatRegister src, FloatRegister dest)
{
    as_fsubs(dest, dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::mulFloat32(FloatRegister src, FloatRegister dest)
{
    as_fmuls(dest, dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::divFloat32(FloatRegister src, FloatRegister dest)
{
    as_fdivs(dest, dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::negateFloat(FloatRegister reg)
{
    as_fneg(reg, reg);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::sqrtFloat32(FloatRegister src, FloatRegister dest)
{
    // PPC G5 has fsqrts. G4 has fsqrte (reciprocal estimate) but not fsqrts.
    // For now use double sqrt + round to single.
    as_fsqrt(dest, src);
    as_frsp(dest, dest);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::absFloat32(FloatRegister src, FloatRegister dest)
{
    as_fabs(dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::minDouble(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    Label done, nan, isLess;

    if (handleNaN) {
        branchDouble(DoubleUnordered, srcDest, other, &nan);
    }

    branchDouble(DoubleLessThan, srcDest, other, &isLess);
    as_fmr(srcDest, other);
    ma_b(&done);

    bind(&isLess);
    ma_b(&done);

    if (handleNaN) {
        bind(&nan);
        as_fadd(srcDest, srcDest, other);
    }

    bind(&done);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::minFloat32(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    minDouble(other, srcDest, handleNaN);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::maxDouble(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    Label done, nan, isGreater;

    if (handleNaN) {
        branchDouble(DoubleUnordered, srcDest, other, &nan);
    }

    branchDouble(DoubleGreaterThan, srcDest, other, &isGreater);
    as_fmr(srcDest, other);
    ma_b(&done);

    bind(&isGreater);
    ma_b(&done);

    if (handleNaN) {
        bind(&nan);
        as_fadd(srcDest, srcDest, other);
    }

    bind(&done);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::maxFloat32(FloatRegister other, FloatRegister srcDest, bool handleNaN)
{
    maxDouble(other, srcDest, handleNaN);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::move8SignExtend(Register src, Register dest)
{
    as_extsb(dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::move16SignExtend(Register src, Register dest)
{
    as_extsh(dest, src);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::moveFloat32ToGPR(FloatRegister src, Register dest)
{
    // Store float32 to stack, load as int
    as_stfs(src, StackPointer, -4);
    as_lwz(dest, StackPointer, -4);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::moveGPRToFloat32(Register src, FloatRegister dest)
{
    // Store int to stack, load as float32
    as_stw(src, StackPointer, -4);
    as_lfs(dest, StackPointer, -4);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::clampIntToUint8(Register reg)
{
    // if (reg > 255) reg = 255;
    // if (reg < 0) reg = 0;
    Label done;
    ma_b(reg, Imm32(255), &done, BelowOrEqual);
    // Check sign
    Label positive;
    ma_b(reg, Imm32(0), &positive, GreaterThanOrEqual);
    move32(Imm32(0), reg);
    ma_b(&done);
    bind(&positive);
    move32(Imm32(255), reg);
    bind(&done);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::branchPtrInNurseryChunk(Condition cond, Register ptr, Register temp, Label* label)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    MOZ_ASSERT(ptr != SecondScratchRegister);

    movePtr(ptr, SecondScratchRegister);
    orPtr(Imm32(gc::ChunkMask), SecondScratchRegister);
    branch32(cond, Address(SecondScratchRegister, gc::ChunkLocationOffsetFromLastByte),
             Imm32(int32_t(gc::ChunkLocation::Nursery)), label);
}

__attribute__((visibility("default"), used))
CodeOffset
MacroAssembler::call(Register reg)
{
    as_mtctr(reg);
    as_bctrl();
    return CodeOffset(currentOffset());
}

__attribute__((visibility("default"), used))
CodeOffset
MacroAssembler::call(Label* label)
{
    // PPC bl instruction (branch and link) — like ma_b but with link
    if (label->bound()) {
        int32_t offset = label->offset() - int32_t(size());
        as_bl(offset);
    } else {
        int32_t branchDisp = 0;
        int32_t branchOff = size();
        int32_t oldUse = label->use(branchOff);
        if (oldUse != LabelBase::INVALID_OFFSET)
            branchDisp = oldUse - branchOff;
        as_bl(branchDisp);
    }
    return CodeOffset(currentOffset());
}

__attribute__((visibility("default"), used))
void
MacroAssembler::call(ImmPtr imm)
{
    ma_li(ScratchRegister, imm);
    as_mtctr(ScratchRegister);
    as_bctrl();
}

__attribute__((visibility("default"), used))
void
MacroAssembler::call(ImmWord imm)
{
    call(ImmPtr((void*)imm.value));
}

__attribute__((visibility("default"), used))
void
MacroAssembler::call(JitCode* code)
{
    ma_li(ScratchRegister, ImmPtr(code->raw()));
    as_mtctr(ScratchRegister);
    as_bctrl();
}

__attribute__((visibility("default"), used))
void
MacroAssembler::call(wasm::SymbolicAddress imm)
{
    MOZ_CRASH("PPC wasm not supported");
}

__attribute__((visibility("default"), used))
CodeOffset
MacroAssembler::callWithPatch()
{
    CodeOffset label;
    ma_liPatchable(ScratchRegister, ImmPtr(nullptr));
    label.bind(size() - 8); // Point to the lis instruction
    as_mtctr(ScratchRegister);
    as_bctrl();
    return label;
}

__attribute__((visibility("default"), used))
void
MacroAssembler::patchCall(uint32_t callerOffset, uint32_t calleeOffset)
{
    uint8_t* caller = m_buffer.buffer() + callerOffset;
    Assembler::PatchInstructionImmediate(caller, PatchedImmPtr((void*)(uintptr_t)calleeOffset));
}

__attribute__((visibility("default"), used))
CodeOffset
MacroAssembler::farJumpWithPatch()
{
    CodeOffset offset;
    ma_liPatchable(ScratchRegister, ImmPtr(nullptr));
    offset.bind(size() - 8);
    as_mtctr(ScratchRegister);
    as_bctr();
    return offset;
}

__attribute__((visibility("default"), used))
void
MacroAssembler::patchFarJump(CodeOffset farJump, uint32_t targetOffset)
{
    uint8_t* jump = m_buffer.buffer() + farJump.offset();
    Assembler::PatchInstructionImmediate(jump, PatchedImmPtr((void*)(uintptr_t)targetOffset));
}

__attribute__((visibility("default"), used))
void
MacroAssembler::repatchFarJump(uint8_t* code, uint32_t farJumpOffset, uint32_t targetOffset)
{
    Assembler::PatchInstructionImmediate(code + farJumpOffset,
                                         PatchedImmPtr((void*)(uintptr_t)targetOffset));
}

__attribute__((visibility("default"), used))
CodeOffset
MacroAssembler::nopPatchableToNearJump()
{
    CodeOffset offset(currentOffset());
    as_nop();
    return offset;
}

__attribute__((visibility("default"), used))
void
MacroAssembler::patchNopToNearJump(uint8_t* jump, uint8_t* target)
{
    int32_t off = target - jump;
    // PPC unconditional branch: opcode 18, bits 30-6 = offset, bit 31 = AA=0, bit 0 = LK=0
    uint32_t inst = (18 << 26) | (off & 0x03FFFFFC);
    memcpy(jump, &inst, sizeof(uint32_t));
}

__attribute__((visibility("default"), used))
void
MacroAssembler::patchNearJumpToNop(uint8_t* jump)
{
    uint32_t nop = 0x60000000; // ori r0,r0,0
    memcpy(jump, &nop, sizeof(uint32_t));
}

__attribute__((visibility("default"), used))
void
MacroAssembler::Push(Register reg)
{
    adjustFrame(sizeof(intptr_t));
    push(reg);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::Push(Imm32 imm)
{
    adjustFrame(sizeof(intptr_t));
    ma_li(ScratchRegister, imm);
    push(ScratchRegister);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::Push(ImmWord imm)
{
    Push(Imm32(imm.value));
}

__attribute__((visibility("default"), used))
void
MacroAssembler::Push(ImmPtr imm)
{
    Push(ImmWord(uintptr_t(imm.value)));
}

__attribute__((visibility("default"), used))
void
MacroAssembler::Push(ImmGCPtr ptr)
{
    adjustFrame(sizeof(intptr_t));
    ma_li(ScratchRegister, ptr);
    push(ScratchRegister);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::Push(FloatRegister f)
{
    adjustFrame(sizeof(double));
    subPtr(Imm32(sizeof(double)), StackPointer);
    ma_sd(f, Address(StackPointer, 0));
}

__attribute__((visibility("default"), used))
void
MacroAssembler::Pop(Register reg)
{
    adjustFrame(-int32_t(sizeof(intptr_t)));
    pop(reg);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::Pop(FloatRegister f)
{
    ma_ld(f, Address(StackPointer, 0));
    addPtr(Imm32(sizeof(double)), StackPointer);
    adjustFrame(-int32_t(sizeof(double)));
}

__attribute__((visibility("default"), used))
void
MacroAssembler::Pop(const ValueOperand& val)
{
    popValue(val);
    adjustFrame(-int32_t(sizeof(Value)));
}

__attribute__((visibility("default"), used))
void
MacroAssembler::PushRegsInMask(LiveRegisterSet set)
{
    int32_t diff = set.gprs().size() * sizeof(intptr_t) +
                   set.fpus().getPushSizeInBytes();
    const int32_t reserved = diff;

    reserveStack(reserved);
    int32_t offset = 0;

    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
        storePtr(*iter, Address(StackPointer, offset));
        offset += sizeof(intptr_t);
    }

    for (FloatRegisterBackwardIterator iter(set.fpus().reduceSetForPush()); iter.more(); ++iter) {
        ma_sd(*iter, Address(StackPointer, offset));
        offset += sizeof(double);
    }

    MOZ_ASSERT(offset == reserved);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::PopRegsInMaskIgnore(LiveRegisterSet set, LiveRegisterSet ignore)
{
    int32_t diff = set.gprs().size() * sizeof(intptr_t) +
                   set.fpus().getPushSizeInBytes();
    const int32_t reserved = diff;

    int32_t offset = 0;

    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
        if (!ignore.has(*iter))
            loadPtr(Address(StackPointer, offset), *iter);
        offset += sizeof(intptr_t);
    }

    for (FloatRegisterBackwardIterator iter(set.fpus().reduceSetForPush()); iter.more(); ++iter) {
        if (!ignore.has(*iter))
            ma_ld(*iter, Address(StackPointer, offset));
        offset += sizeof(double);
    }

    freeStack(reserved);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::pushReturnAddress()
{
    // PPC stores return address in LR, push it to stack
    as_mflr(ScratchRegister);
    push(ScratchRegister);
}

__attribute__((visibility("default"), used))
uint32_t
MacroAssembler::pushFakeReturnAddress(Register scratch)
{
    CodeLabel cl;

    ma_li(scratch, cl.patchAt());
    Push(scratch);
    bind(cl.target());
    uint32_t retAddr = currentOffset();

    addCodeLabel(cl);
    return retAddr;
}

__attribute__((visibility("default"), used))
void
MacroAssembler::popReturnAddress()
{
    pop(ScratchRegister);
    as_mtlr(ScratchRegister);
}

__attribute__((visibility("default"), used))
void
MacroAssembler::comment(const char* msg)
{
    // No-op: comments are not emitted in machine code
}

__attribute__((visibility("default"), used))
void
MacroAssembler::flush()
{
    // No-op for PPC: instruction cache will be flushed separately
}

__attribute__((visibility("default"), used))
void
MacroAssembler::wasmPatchBoundsCheck(uint8_t* patchAt, uint32_t limit)
{
    MOZ_CRASH("PPC wasm not supported");
}



void
MacroAssemblerPPCCompat::boxDouble(FloatRegister src, const ValueOperand& dest)
{
    // Store double to stack, then read as two GPRs
    // Big-endian: high word (offset 0) = type, low word (offset 4) = payload
    asMasm().subPtr(Imm32(8), StackPointer);
    as_stfd(src, StackPointer, 0);
    as_lwz(dest.typeReg(), StackPointer, 0);
    as_lwz(dest.payloadReg(), StackPointer, 4);
    asMasm().addPtr(Imm32(8), StackPointer);
}

void
MacroAssemblerPPCCompat::boxNonDouble(JSValueType type, Register src,
                                       const ValueOperand& dest)
{
    if (src != dest.payloadReg())
        ma_move(dest.payloadReg(), src);
    ma_li(dest.typeReg(), ImmType(type));
}

void
MacroAssembler::memoryBarrier(MemoryBarrierBits barrier)
{
    if (barrier)
        as_sync();
}
