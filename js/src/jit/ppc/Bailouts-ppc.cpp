/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ppc/Bailouts-ppc.h"

#include "jscntxt.h"
#include "jscompartment.h"
#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/Bailouts.h"

using namespace js;
using namespace js::jit;

BailoutFrameInfo::BailoutFrameInfo(const JitActivationIterator& activations,
                                   BailoutStack* bailout)
  : machine_()
{
    // PPC FIX: Copy register values into our own storage (savedRegs_/savedFpregs_)
    // and point MachineState at those copies. This avoids two issues:
    // 1) PPC Darwin ABI large struct return corrupts hidden return pointer
    // 2) Callee frame setup can overwrite the bailout stack on PPC
    {
        uint8_t* bailoutBase = (uint8_t*)bailout;
        Registers::RegisterContent* gpRegs =
            (Registers::RegisterContent*)(bailoutBase + BailoutStack::offsetOfRegs());
        FloatRegisters::RegisterContent* fpRegs =
            (FloatRegisters::RegisterContent*)(bailoutBase + BailoutStack::offsetOfFpRegs());
        // Copy values into our own storage
        for (unsigned i = 0; i < Registers::Total; i++)
            savedRegs_[i].r = gpRegs[i].r;
        for (unsigned i = 0; i < FloatRegisters::TotalPhys; i++)
            savedFpregs_[i] = fpRegs[i];
        // Point MachineState at our local copies
        for (unsigned i = 0; i < Registers::Total; i++)
            machine_.setRegisterLocation(Register::FromCode(i), &savedRegs_[i].r);
        for (unsigned i = 0; i < FloatRegisters::TotalPhys; i++) {
            machine_.setRegisterLocation(FloatRegister(i, FloatRegister::Single), &savedFpregs_[i]);
            machine_.setRegisterLocation(FloatRegister(i, FloatRegister::Double), &savedFpregs_[i]);
        }
    }

    uint8_t* sp = bailout->parentStackPointer();
    framePointer_ = sp + bailout->frameSize();
    topFrameSize_ = framePointer_ - sp;


    // Safety check: validate calleeToken before dereferencing
    CalleeToken ct = ((JitFrameLayout*) framePointer_)->calleeToken();
    uintptr_t ctVal = (uintptr_t)ct;
    if (ctVal >= 0xFFFFFF80 || ctVal < 0x10000) {
        // Corrupted JitFrameLayout. The Ion frame overflowed into the
        // parent frame's JitFrameLayout. We need to skip past ALL frames
        // up to the entry frame so no frame iterator hits the corruption.
        //
        // r14 = savedSP from enterJIT -- the entry frame's stack pointer.
        // Read it from the saved registers in the BailoutStack.
        uint8_t* bailoutBase = (uint8_t*)bailout;
        Registers::RegisterContent* gpRegs =
            (Registers::RegisterContent*)(bailoutBase + BailoutStack::offsetOfRegs());
        uintptr_t entrySP = gpRegs[14].r;
        uint8_t* parentSP = bailout->parentStackPointer();

        fprintf(stderr, "PPC-BAILOUT: corrupted ct=0x%x fp=%p entrySP=0x%x parentSP=%p\n",
                (unsigned)ctVal, (void*)framePointer_, (unsigned)entrySP, (void*)parentSP);
        fflush(stderr);

        if (entrySP > (uintptr_t)parentSP && entrySP < (uintptr_t)parentSP + 65536) {
            // Patch the BailoutStack frameSize so GenerateBailoutThunk skips
            // all the way to the entry frame SP, bypassing the corrupted layout.
            uintptr_t* frameSizePtr = (uintptr_t*)(bailoutBase + BailoutStack::offsetOfFrameSize());
            *frameSizePtr = entrySP - (uintptr_t)parentSP;
            fprintf(stderr, "PPC-BAILOUT: patched frameSize to %u (skip to entry)\n",
                    (unsigned)(entrySP - (uintptr_t)parentSP));
            fflush(stderr);
        }

        topIonScript_ = nullptr;
        topFrameSize_ = 0;
        snapshotOffset_ = 0;
        activation_ = nullptr;
        return;
    }
    JSScript* script = ScriptFromCalleeToken(ct);
    topIonScript_ = script->ionScript();


    attachOnJitActivation(activations);

    if (bailout->frameClass() == FrameSizeClass::None()) {
        snapshotOffset_ = bailout->snapshotOffset();
        return;
    }

    // Compute the snapshot offset from the bailout ID.
    JitActivation* activation = activations.activation()->asJit();
    JSRuntime* rt = activation->compartment()->runtimeFromMainThread();
    JitCode* code = rt->jitRuntime()->getBailoutTable(bailout->frameClass());
    uintptr_t tableOffset = bailout->tableOffset();
    uintptr_t tableStart = reinterpret_cast<uintptr_t>(code->raw());

    MOZ_ASSERT(tableOffset >= tableStart &&
               tableOffset < tableStart + code->instructionsSize());
    MOZ_ASSERT((tableOffset - tableStart) % BAILOUT_TABLE_ENTRY_SIZE == 0);

    uint32_t bailoutId = ((tableOffset - tableStart) / BAILOUT_TABLE_ENTRY_SIZE) - 1;
    MOZ_ASSERT(bailoutId < BAILOUT_TABLE_SIZE);

    snapshotOffset_ = topIonScript_->bailoutToSnapshot(bailoutId);
}

BailoutFrameInfo::BailoutFrameInfo(const JitActivationIterator& activations,
                                   InvalidationBailoutStack* bailout)
  : machine_()
{
    // PPC FIX: Copy register values into our own storage (savedRegs_/savedFpregs_)
    // and point MachineState at those copies. Avoids bailout stack being
    // overwritten by callee frames on PPC Darwin ABI.
    {
        uint8_t* bailoutBase = (uint8_t*)bailout;
        Registers::RegisterContent* gpRegs =
            (Registers::RegisterContent*)(bailoutBase + InvalidationBailoutStack::offsetOfRegs());
        FloatRegisters::RegisterContent* fpRegs =
            (FloatRegisters::RegisterContent*)(bailoutBase + InvalidationBailoutStack::offsetOfFpRegs());
        // Copy values into our own storage
        for (unsigned i = 0; i < Registers::Total; i++)
            savedRegs_[i].r = gpRegs[i].r;
        for (unsigned i = 0; i < FloatRegisters::TotalPhys; i++)
            savedFpregs_[i] = fpRegs[i];
        // Point MachineState at our local copies
        for (unsigned i = 0; i < Registers::Total; i++)
            machine_.setRegisterLocation(Register::FromCode(i), &savedRegs_[i].r);
        for (unsigned i = 0; i < FloatRegisters::TotalPhys; i++) {
            machine_.setRegisterLocation(FloatRegister(i, FloatRegister::Single), &savedFpregs_[i]);
            machine_.setRegisterLocation(FloatRegister(i, FloatRegister::Double), &savedFpregs_[i]);
        }
    }

    framePointer_ = (uint8_t*)bailout->fp();
    topFrameSize_ = framePointer_ - bailout->sp();
    topIonScript_ = bailout->ionScript();
    attachOnJitActivation(activations);

    uint8_t* returnAddressToFp_ = bailout->osiPointReturnAddress();
    const OsiIndex* osiIndex = topIonScript_->getOsiIndex(returnAddressToFp_);
    snapshotOffset_ = osiIndex->snapshotOffset();
}
