/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ppc_SharedICHelpers_ppc_h
#define jit_ppc_SharedICHelpers_ppc_h

#include "jit/BaselineFrame.h"
#include "jit/SharedICRegisters.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/SharedIC.h"

namespace js {
namespace jit {

// Distance from sp to the top Value inside an IC stub (no return address on
// the stack on PPC, similar to MIPS).
static const size_t ICStackValueOffset = 0;

struct BaselineStubFrame {
    uintptr_t savedFrame;
    uintptr_t savedStub;
    uintptr_t returnAddress;
    uintptr_t descriptor;
};

static const uint32_t STUB_FRAME_SIZE = sizeof(BaselineStubFrame);
static const uint32_t STUB_FRAME_SAVED_STUB_OFFSET = offsetof(BaselineStubFrame, savedStub);

// Size of the baseline stub frame.

// Registers used in RegExpMatcher instruction (do not use ReturnReg).

// Registers used in RegExpTester instruction (do not use ReturnReg).


// PPC uses LR for return address, not a GPR on stack. Similar to MIPS ra.
// EmitRestoreTailCallReg and EmitRepushTailCallReg are no-ops because LR
// is always available.

inline void
EmitRestoreTailCallReg(MacroAssembler& masm)
{
    // No-op: PPC LR always holds return address
}

inline void
EmitRepushTailCallReg(MacroAssembler& masm)
{
    // No-op
}

inline void
EmitCallIC(CodeOffset* patchOffset, MacroAssembler& masm)
{

    // Load ICEntry address into ICStubReg via a patchable lis+ori pair.
    // The baseline compiler will patch this with the real ICEntry address.
    CodeOffset offset = masm.movWithPatch(ImmWord(-1), ICStubReg);
    *patchOffset = offset;

    // Load the first stub pointer from the ICEntry.
    masm.loadPtr(Address(ICStubReg, ICEntry::offsetOfFirstStub()), ICStubReg);

    // Load stub code pointer from the stub.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), ScratchRegister);


    // Call the stub code.
    masm.ma_mtctr(ScratchRegister);
    masm.ma_bctrl();

    // DEBUG: Check if r29 (BaselineFrameReg) was corrupted by IC stub.
    // r29 and r1 should be on the same stack (same 16MB region).
    // XOR their upper 8 bits — if they differ, r29 was corrupted.
    {
        Label ok;
        // xor r12, r29, r1 — find differing bits
        masm.as_xor(ScratchRegister, BaselineFrameReg, StackPointer);
        // srwi r12, r12, 24 — check if upper 8 bits differ
        masm.as_rlwinm(ScratchRegister, ScratchRegister, 8, 24, 31);
        masm.ma_b(ScratchRegister, Imm32(0), &ok, Assembler::Equal);
        masm.breakpoint();
        masm.bind(&ok);
    }

}

inline void
EmitEnterTypeMonitorIC(MacroAssembler& masm,
                       size_t monitorStubOffset = ICMonitoredStub::offsetOfFirstMonitorStub())
{
    // Load first monitor stub
    masm.loadPtr(Address(ICStubReg, monitorStubOffset), ICStubReg);

    // Load stub code and jump to it
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), ScratchRegister);


    masm.ma_mtctr(ScratchRegister);
    masm.ma_bctr();
}

inline void
EmitReturnFromIC(MacroAssembler& masm)
{
    masm.ma_blr();
}

inline void
EmitChangeICReturnAddress(MacroAssembler& masm, Register reg)
{
    masm.ma_mtlr(reg);
}

inline void
EmitBaselineTailCallVM(JitCode* target, MacroAssembler& masm, uint32_t argSize)
{
    Register scratch = R2.scratchReg();

    // Compute frame size.
    masm.movePtr(BaselineFrameReg, scratch);
    masm.addPtr(Imm32(BaselineFrame::FramePointerOffset), scratch);
    masm.subPtr(BaselineStackReg, scratch);

    // Store frame size without VMFunction arguments for GC marking.
    masm.subPtr(Imm32(argSize), scratch);
    masm.store32(scratch, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));
    masm.addPtr(Imm32(argSize), scratch);

    // Push frame descriptor and perform the tail call.
    // On PPC, the return address is in LR. The VMWrapper expects it on the stack.
    masm.as_mflr(r0);
    masm.makeFrameDescriptor(scratch, JitFrame_BaselineJS, ExitFrameLayout::Size());
    masm.subPtr(Imm32(sizeof(CommonFrameLayout)), StackPointer);
    masm.storePtr(scratch, Address(StackPointer, CommonFrameLayout::offsetOfDescriptor()));
    masm.storePtr(r0, Address(StackPointer, CommonFrameLayout::offsetOfReturnAddress()));

    masm.branch(target);
}

inline void
EmitIonTailCallVM(JitCode* target, MacroAssembler& masm, uint32_t stackSize)
{
    Register scratch = R2.scratchReg();

    masm.loadPtr(Address(StackPointer, stackSize), scratch);
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch);
    masm.addPtr(Imm32(stackSize + JitStubFrameLayout::Size() - sizeof(intptr_t)), scratch);

    // Push frame descriptor and perform the tail call.
    masm.as_mflr(r0);
    masm.makeFrameDescriptor(scratch, JitFrame_IonJS, ExitFrameLayout::Size());
    masm.push(scratch);
    masm.push(r0);
    masm.branch(target);
}

inline void
EmitBaselineCreateStubFrameDescriptor(MacroAssembler& masm, Register reg, uint32_t headerSize)
{
    // Compute stub frame size. We have to add two pointers: the stub reg and
    // previous frame pointer pushed by EmitEnterStubFrame.
    masm.movePtr(BaselineFrameReg, reg);
    masm.addPtr(Imm32(sizeof(intptr_t) * 2), reg);
    masm.subPtr(BaselineStackReg, reg);

    masm.makeFrameDescriptor(reg, JitFrame_BaselineStub, headerSize);
}

inline void
EmitBaselineCallVM(JitCode* target, MacroAssembler& masm)
{
    Register scratch = R2.scratchReg();
    EmitBaselineCreateStubFrameDescriptor(masm, scratch, ExitFrameLayout::Size());
    masm.push(scratch);
    masm.call(target);
}

inline void
EmitIonCallVM(JitCode* target, size_t stackSlots, MacroAssembler& masm)
{
    uint32_t descriptor = MakeFrameDescriptor(masm.framePushed(), JitFrame_IonStub,
                                              ExitFrameLayout::Size());
    masm.Push(Imm32(descriptor));
    masm.callJit(target);

    // Remove rest of the frame left on the stack. We remove the return address
    // which is implicitly popped when returning.
    size_t framePop = sizeof(ExitFrameLayout) - sizeof(void*);

    // Pop arguments from framePushed.
    masm.implicitPop(stackSlots * sizeof(void*) + framePop);
}

inline void
EmitBaselineEnterStubFrame(MacroAssembler& masm, Register scratch)
{
    MOZ_ASSERT(scratch != ICTailCallReg);

    // Compute frame size.
    masm.movePtr(BaselineFrameReg, scratch);
    masm.addPtr(Imm32(BaselineFrame::FramePointerOffset), scratch);
    masm.subPtr(BaselineStackReg, scratch);

    masm.store32(scratch, Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFrameSize()));

    // Push frame descriptor and return address.
    // On PPC, the return address is in LR — move it to r0.
    masm.as_mflr(r0);
    masm.makeFrameDescriptor(scratch, JitFrame_BaselineJS, BaselineStubFrameLayout::Size());
    masm.subPtr(Imm32(STUB_FRAME_SIZE), StackPointer);
    masm.storePtr(scratch, Address(StackPointer, offsetof(BaselineStubFrame, descriptor)));
    masm.storePtr(r0, Address(StackPointer, offsetof(BaselineStubFrame, returnAddress)));

    // Save old frame pointer and stub reg.
    masm.storePtr(ICStubReg, Address(StackPointer, offsetof(BaselineStubFrame, savedStub)));
    masm.storePtr(BaselineFrameReg, Address(StackPointer, offsetof(BaselineStubFrame, savedFrame)));
    masm.movePtr(BaselineStackReg, BaselineFrameReg);
}

inline void
EmitIonEnterStubFrame(MacroAssembler& masm, Register scratch)
{
    // On PPC, the return address is in LR. Push it to the stack
    // (using push, not Push, since it's part of the previous frame).
    masm.as_mflr(r0);
    masm.push(r0);

    masm.Push(ICStubReg);
}

inline void
EmitBaselineLeaveStubFrame(MacroAssembler& masm, bool calledIntoIon = false)
{
    // Ion frames do not save and restore the frame pointer. If we called
    // into Ion, we have to restore the stack pointer from the frame descriptor.
    // If we performed a VM call, the descriptor has been popped already so
    // in that case we use the frame pointer.
    if (calledIntoIon) {
        masm.pop(ScratchRegister);
        masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), ScratchRegister);
        masm.addPtr(ScratchRegister, BaselineStackReg);
    } else {
        masm.movePtr(BaselineFrameReg, BaselineStackReg);
    }

    masm.loadPtr(Address(StackPointer, offsetof(BaselineStubFrame, savedFrame)),
                 BaselineFrameReg);
    masm.loadPtr(Address(StackPointer, offsetof(BaselineStubFrame, savedStub)),
                 ICStubReg);

    // Load the return address and restore LR.
    masm.loadPtr(Address(StackPointer, offsetof(BaselineStubFrame, returnAddress)),
                 r0);
    masm.as_mtlr(r0);

    // Discard the stub frame.
    masm.addPtr(Imm32(STUB_FRAME_SIZE), StackPointer);
}

inline void
EmitIonLeaveStubFrame(MacroAssembler& masm)
{
    masm.Pop(ICStubReg);
    // Restore return address from stack back to LR.
    masm.pop(r0);
    masm.as_mtlr(r0);
}

inline void
EmitStowICValues(MacroAssembler& masm, int values)
{
    MOZ_ASSERT(values >= 0 && values <= 2);
    switch (values) {
      case 1:
        // Push R0 (type + payload)
        masm.pushValue(R0);
        break;
      case 2:
        masm.pushValue(R0);
        masm.pushValue(R1);
        break;
    }
}

inline void
EmitUnstowICValues(MacroAssembler& masm, int values, bool discard = false)
{
    MOZ_ASSERT(values >= 0 && values <= 2);
    if (discard) {
        masm.addToStackPtr(Imm32(values * sizeof(Value)));
        return;
    }
    switch (values) {
      case 1:
        masm.popValue(R0);
        break;
      case 2:
        masm.popValue(R1);
        masm.popValue(R0);
        break;
    }
}

inline void
EmitCallTypeUpdateIC(MacroAssembler& masm, JitCode* code, uint32_t objectOffset)
{
    // R0 contains the value that needs to be typechecked.
    // The object we're updating is a boxed Value on the stack, at offset
    // objectOffset from SP, excluding the saved registers below.

    // Save ICStubReg and LR to stack. On PPC, LR is the tail call register.
    masm.subPtr(Imm32(2 * sizeof(intptr_t)), StackPointer);
    masm.storePtr(ICStubReg, Address(StackPointer, sizeof(intptr_t)));
    masm.ma_mflr(ScratchRegister);
    masm.storePtr(ScratchRegister, Address(StackPointer, 0));

    // Load first update stub from the IC chain.
    masm.loadPtr(Address(ICStubReg, ICUpdatedStub::offsetOfFirstUpdateStub()),
                 ICStubReg);

    // Load stub code pointer and call it.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), R2.scratchReg());
    masm.call(R2.scratchReg());

    // Restore ICStubReg and LR.
    masm.loadPtr(Address(StackPointer, 0), ScratchRegister);
    masm.ma_mtlr(ScratchRegister);
    masm.loadPtr(Address(StackPointer, sizeof(intptr_t)), ICStubReg);
    masm.addPtr(Imm32(2 * sizeof(intptr_t)), StackPointer);

    // The update IC stores 0 or 1 in R1.scratchReg() reflecting success.
    Label success;
    masm.branch32(Assembler::Equal, R1.scratchReg(), Imm32(1), &success);

    // IC chain failed — call the update fallback function via VM wrapper.
    EmitBaselineEnterStubFrame(masm, R1.scratchReg());

    // Load the object value from the stack (past the stub frame).
    masm.loadValue(Address(BaselineStackReg, STUB_FRAME_SIZE + objectOffset), R1);

    masm.Push(R0);
    masm.Push(R1);
    masm.Push(ICStubReg);

    // Push BaselineFrame*.
    masm.loadPtr(Address(BaselineFrameReg, 0), R0.scratchReg());
    masm.pushBaselineFramePtr(R0.scratchReg(), R0.scratchReg());

    EmitBaselineCallVM(code, masm);
    EmitBaselineLeaveStubFrame(masm);

    masm.bind(&success);
}

template <typename AddrType>
inline void
EmitPreBarrier(MacroAssembler& masm, const AddrType& addr, MIRType type)
{
    // PPC uses LR, so we need to save/restore it around the pre-barrier
    masm.ma_mflr(ScratchRegister);
    masm.push(ScratchRegister);

    masm.patchableCallPreBarrier(addr, type);

    masm.pop(ScratchRegister);
    masm.ma_mtlr(ScratchRegister);
}

inline void
EmitStubGuardFailure(MacroAssembler& masm)
{
    // NOTE: This routine assumes that the stub guard code left the stack in
    // the same state it was in when it was entered.

    // Load next stub into ICStubReg.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfNext()), ICStubReg);

    // Load stubcode pointer from the new ICStubReg.
    masm.loadPtr(Address(ICStubReg, ICStub::offsetOfStubCode()), R2.scratchReg());


    // Return address is already in LR, just jump to the next stubcode.
    masm.ma_mtctr(R2.scratchReg());
    masm.ma_bctr();
}

} // namespace jit
} // namespace js

#endif /* jit_ppc_SharedICHelpers_ppc_h */
