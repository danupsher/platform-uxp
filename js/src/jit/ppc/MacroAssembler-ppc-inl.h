/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ppc_MacroAssembler_ppc_inl_h
#define jit_ppc_MacroAssembler_ppc_inl_h

#include "jit/ppc/MacroAssembler-ppc.h"

namespace js {
namespace jit {

//{{{ check_macroassembler_style

// ===============================================================
// Condition-set functions (templates — must stay in header)

template <typename T1, typename T2>
void
MacroAssembler::cmpPtrSet(Condition cond, T1 lhs, T2 rhs, Register dest)
{
    // Default: load lhs if Address, convert rhs if needed, then compare
    ma_cmp_set(dest, lhs, rhs, cond);
}

// Specializations for types not handled by ma_cmp_set
template <>
inline void
MacroAssembler::cmpPtrSet<Register, ImmWord>(Condition cond, Register lhs, ImmWord rhs, Register dest)
{
    ma_cmp_set(dest, lhs, Imm32(rhs.value), cond);
}

template <>
inline void
MacroAssembler::cmpPtrSet<Address, Register>(Condition cond, Address lhs, Register rhs, Register dest)
{
    loadPtr(lhs, SecondScratchRegister);
    ma_cmp_set(dest, SecondScratchRegister, rhs, cond);
}

template <>
inline void
MacroAssembler::cmpPtrSet<Address, ImmPtr>(Condition cond, Address lhs, ImmPtr rhs, Register dest)
{
    loadPtr(lhs, SecondScratchRegister);
    ma_cmp_set(dest, SecondScratchRegister, rhs, cond);
}

template <typename T1, typename T2>
void
MacroAssembler::cmp32Set(Condition cond, T1 lhs, T2 rhs, Register dest)
{
    ma_cmp_set(dest, lhs, rhs, cond);
}

template <>
inline void
MacroAssembler::cmp32Set<Address, Imm32>(Condition cond, Address lhs, Imm32 rhs, Register dest)
{
    load32(lhs, SecondScratchRegister);
    ma_cmp_set(dest, SecondScratchRegister, rhs, cond);
}

template <>
inline void
MacroAssembler::cmpPtrSet<Address, ImmWord>(Condition cond, Address lhs, ImmWord rhs, Register dest)
{
    loadPtr(lhs, SecondScratchRegister);
    ma_cmp_set(dest, SecondScratchRegister, Imm32(rhs.value), cond);
}

// ===============================================================
// Branch functions (templates — must stay in header)

template <class L>
void
MacroAssembler::branch32(Condition cond, Register lhs, Register rhs, L label)
{
    ma_b(lhs, rhs, label, cond);
}

template <class L>
void
MacroAssembler::branch32(Condition cond, Register lhs, Imm32 imm, L label)
{
    ma_b(lhs, imm, label, cond);
}

template <class L>
void
MacroAssembler::branchPtr(Condition cond, Register lhs, Register rhs, L label)
{
    ma_b(lhs, rhs, label, cond);
}

template <class L>
void
MacroAssembler::branchPtr(Condition cond, const Address& lhs, Register rhs, L label)
{
    loadPtr(lhs, SecondScratchRegister);
    branchPtr(cond, SecondScratchRegister, rhs, label);
}

template <typename T>
CodeOffsetJump
MacroAssembler::branchPtrWithPatch(Condition cond, Register lhs, T rhs, RepatchLabel* label)
{
    movePtr(rhs, ScratchRegister);
    Label skipJump;
    ma_b(lhs, ScratchRegister, &skipJump, InvertCondition(cond));
    CodeOffsetJump off = jumpWithPatch(label);
    bind(&skipJump);
    return off;
}

template <typename T>
CodeOffsetJump
MacroAssembler::branchPtrWithPatch(Condition cond, Address lhs, T rhs, RepatchLabel* label)
{
    loadPtr(lhs, SecondScratchRegister);
    movePtr(rhs, ScratchRegister);
    Label skipJump;
    ma_b(SecondScratchRegister, ScratchRegister, &skipJump, InvertCondition(cond));
    CodeOffsetJump off = jumpWithPatch(label);
    bind(&skipJump);
    return off;
}

// ===============================================================
// Branch test functions (templates — must stay in header)

template <class L>
void
MacroAssembler::branchTest32(Condition cond, Register lhs, Register rhs, L label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
    if (lhs == rhs) {
        ma_b(lhs, Imm32(0), label, cond);
    } else {
        as_and(ScratchRegister, lhs, rhs);
        ma_b(ScratchRegister, Imm32(0), label, cond);
    }
}

template <class L>
void
MacroAssembler::branchTest32(Condition cond, Register lhs, Imm32 rhs, L label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
    ma_and(ScratchRegister, lhs, rhs);
    ma_b(ScratchRegister, Imm32(0), label, cond);
}

template <class L>
void
MacroAssembler::branchTestPtr(Condition cond, Register lhs, Register rhs, L label)
{
    MOZ_ASSERT(cond == Zero || cond == NonZero || cond == Signed || cond == NotSigned);
    if (lhs == rhs) {
        ma_b(lhs, Imm32(0), label, cond);
    } else {
        as_and(ScratchRegister, lhs, rhs);
        ma_b(ScratchRegister, Imm32(0), label, cond);
    }
}

// ===============================================================
// Branch with arithmetic (templates — must stay in header)

template <typename T, typename L>
void
MacroAssembler::branchAdd32(Condition cond, T src, Register dest, L overflow)
{
    switch (cond) {
      case Overflow:
        ma_addTestOverflow(dest, dest, src, overflow);
        break;
      default:
        MOZ_CRASH("NYI");
    }
}

// Specialization for Register
template <>
inline void
MacroAssembler::branchSub32<Register>(Condition cond, Register src, Register dest, Label* overflow)
{
    switch (cond) {
      case Overflow:
        ma_subTestOverflow(dest, dest, src, overflow);
        break;
      case NonZero:
      case Zero:
        ma_subu(dest, dest, src);
        ma_b(dest, dest, overflow, cond);
        break;
      default:
        MOZ_CRASH("NYI");
    }
}

// Specialization for Imm32
template <>
inline void
MacroAssembler::branchSub32<Imm32>(Condition cond, Imm32 src, Register dest, Label* overflow)
{
    switch (cond) {
      case Overflow: {
        ma_li(ScratchRegister, src);
        ma_subTestOverflow(dest, dest, ScratchRegister, overflow);
        break;
      }
      case NonZero:
      case Zero:
        ma_subu(dest, dest, src);
        ma_b(dest, dest, overflow, cond);
        break;
      default:
        MOZ_CRASH("NYI");
    }
}

// ===============================================================
// Branch test 64-bit (template — must stay in header)

template <class L>
void
MacroAssembler::branchTest64(Condition cond, Register64 lhs, Register64 rhs, Register temp,
                             L label)
{
    if (cond == Assembler::Zero) {
        MOZ_ASSERT(lhs.low == rhs.low);
        MOZ_ASSERT(lhs.high == rhs.high);
        as_or(ScratchRegister, lhs.low, lhs.high);
        branchTestPtr(cond, ScratchRegister, ScratchRegister, label);
    } else {
        MOZ_CRASH("Unsupported condition");
    }
}

// ===============================================================
// Branch test magic (template — must stay in header)

template <class L>
void
MacroAssembler::branchTestMagic(Condition cond, const ValueOperand& value, L label)
{
    ma_b(value.typeReg(), ImmTag(JSVAL_TAG_MAGIC), label, cond);
}

// ===============================================================
// Store unboxed value (template — must stay in header)

template <typename T>
void
MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value, MIRType valueType,
                                  const T& dest, MIRType slotType)
{
    if (valueType == MIRType::Double) {
        storeDouble(value.reg().typedReg().fpu(), dest);
        return;
    }

    // Store the type tag if needed.
    if (valueType != slotType)
        store32(ImmType(ValueTypeFromMIRType(valueType)), ToType(dest));

    // Store the payload.
    if (value.constant())
        storeValue(value.value(), dest);
    else
        store32(value.reg().typedReg().gpr(), ToPayload(dest));
}

// ===============================================================
// wasm support (template — must stay in header)

template <class L>
void
MacroAssembler::wasmBoundsCheck(Condition cond, Register index, L label)
{
    MOZ_CRASH("PPC wasm not supported");
}

//}}} check_macroassembler_style
// ===============================================================

} // namespace jit
} // namespace js

#endif /* jit_ppc_MacroAssembler_ppc_inl_h */
