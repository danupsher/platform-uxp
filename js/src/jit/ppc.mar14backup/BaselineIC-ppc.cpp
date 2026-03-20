/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#include "jit/BaselineIC.h"
#include "jit/SharedICHelpers.h"
#include "jit/MacroAssembler-inl.h"
using namespace js;
using namespace js::jit;
namespace js { namespace jit {
bool
ICCompare_Int32::Compiler::generateStubCode(MacroAssembler& masm)
{
    // Guard R0 is int32
    Label failure;
    masm.branchTestInt32(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
    // Compare payloads
    Register left = R0.payloadReg();
    Register right = R1.payloadReg();
    masm.ma_cmp_set(left, left, right, JSOpToCondition(op, /* signed = */true));
    masm.tagValue(JSVAL_TYPE_BOOLEAN, left, R0);
    EmitReturnFromIC(masm);
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}
} }

bool
ICCompare_Double::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure, done;
    masm.ensureDouble(R0, FloatReg0, &failure);
    masm.ensureDouble(R1, FloatReg1, &failure);

    Register dest = R0.scratchReg();

    Assembler::DoubleCondition doubleCond = JSOpToDoubleCondition(op);
    Assembler::DoubleCondition inverseCond = Assembler::InvertCondition(doubleCond);

    // Default to true, branch to done if condition met, else set false.
    masm.move32(Imm32(1), dest);
    masm.branchDouble(doubleCond, FloatReg0, FloatReg1, &done);
    masm.move32(Imm32(0), dest);
    masm.bind(&done);

    masm.tagValue(JSVAL_TYPE_BOOLEAN, dest, R0);
    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}
