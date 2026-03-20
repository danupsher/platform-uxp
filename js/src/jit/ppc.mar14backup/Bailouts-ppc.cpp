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
    // PPC FIX: Initialize machine state directly from BailoutStack offsets
    // instead of using bailout->machine(), which has ABI issues with large
    // struct return on PPC Darwin (hidden return pointer corrupts this).
    {
        uint8_t* bailoutBase = (uint8_t*)bailout;
        Registers::RegisterContent* gpRegs =
            (Registers::RegisterContent*)(bailoutBase + BailoutStack::offsetOfRegs());
        FloatRegisters::RegisterContent* fpRegs =
            (FloatRegisters::RegisterContent*)(bailoutBase + BailoutStack::offsetOfFpRegs());
        for (unsigned i = 0; i < Registers::Total; i++)
            machine_.setRegisterLocation(Register::FromCode(i), &gpRegs[i].r);
        for (unsigned i = 0; i < FloatRegisters::TotalPhys; i++) {
            machine_.setRegisterLocation(FloatRegister(i, FloatRegister::Single), &fpRegs[i]);
            machine_.setRegisterLocation(FloatRegister(i, FloatRegister::Double), &fpRegs[i]);
        }
    }

    uint8_t* sp = bailout->parentStackPointer();
    framePointer_ = sp + bailout->frameSize();
    topFrameSize_ = framePointer_ - sp;


    // Safety check: validate calleeToken before dereferencing
    CalleeToken ct = ((JitFrameLayout*) framePointer_)->calleeToken();
    uintptr_t ctVal = (uintptr_t)ct;
    if (ctVal >= 0xFFFFFF80 || ctVal < 0x1000) {
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
  : machine_(bailout->machine())
{
    framePointer_ = (uint8_t*)bailout->fp();
    topFrameSize_ = framePointer_ - bailout->sp();
    topIonScript_ = bailout->ionScript();
    snapshotOffset_ = (uintptr_t)bailout->osiPointReturnAddress();
}
