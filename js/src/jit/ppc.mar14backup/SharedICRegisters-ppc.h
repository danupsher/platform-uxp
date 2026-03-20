/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ppc_SharedICRegisters_ppc_h
#define jit_ppc_SharedICRegisters_ppc_h

#include "jit/ppc/Assembler-ppc.h"
#include "jit/Registers.h"

namespace js {
namespace jit {

// Baseline frame pointer — callee-saved register
static constexpr Register BaselineFrameReg = r29;
static constexpr Register BaselineStackReg = StackPointer;

// IC value operands — nunbox32: (type, payload)
// R0 is used for the primary IC value
static constexpr ValueOperand R0(r5, r4);   // type=r5, payload=r4
// R1 is used for secondary IC value
static constexpr ValueOperand R1(r15, r14); // type=r15, payload=r14 (callee-saved)
// R2 is the scratch IC value
static constexpr ValueOperand R2(r17, r16); // type=r17, payload=r16 (callee-saved)

// IC tail call register — return address
// PPC uses LR, but for IC purposes we save/restore to a register
static constexpr Register ICTailCallReg = r0;

// IC stub register — holds pointer to current IC stub
static constexpr Register ICStubReg = r18;

// Extract temp registers — not needed for nunbox32
static constexpr Register ExtractTemp0{Registers::r11};
static constexpr Register ExtractTemp1{Registers::r12};

// Baseline second scratch register
static constexpr Register BaselineSecondScratchReg = SecondScratchRegister;

// Float registers for IC use
static constexpr FloatRegister FloatReg0 = {FloatRegisters::f1, FloatRegister::Double};
static constexpr FloatRegister FloatReg1 = {FloatRegisters::f2, FloatRegister::Double};

} // namespace jit
} // namespace js

#endif /* jit_ppc_SharedICRegisters_ppc_h */
