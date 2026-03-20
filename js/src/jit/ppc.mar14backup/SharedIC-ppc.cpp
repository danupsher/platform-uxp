/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#include "jit/SharedIC.h"
#include "jit/SharedICHelpers.h"
#include "jit/MacroAssembler-inl.h"
using namespace js;
using namespace js::jit;
namespace js { namespace jit {
bool
ICBinaryArith_Int32::Compiler::generateStubCode(MacroAssembler& masm)
{
    // Guard both inputs are int32
    Label failure;
    masm.branchTestInt32(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);
    Register left = R0.payloadReg();
    Register right = R1.payloadReg();
    // Perform operation
    Label revertRegister;
    switch (op_) {
      case JSOP_BITOR:  masm.ma_or(left, right); break;
      case JSOP_BITXOR: masm.ma_xor(left, right); break;
      case JSOP_BITAND: masm.ma_and(left, right); break;
      case JSOP_ADD:
        masm.ma_addTestOverflow(left, left, right, &failure);
        break;
      case JSOP_SUB:
        masm.ma_subTestOverflow(left, left, right, &failure);
        break;
      default:
        masm.bind(&failure);
        EmitStubGuardFailure(masm);
        return true;
    }
    masm.tagValue(JSVAL_TYPE_INT32, left, R0);
    EmitReturnFromIC(masm);
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}
bool
ICUnaryArith_Int32::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestInt32(Assembler::NotEqual, R0, &failure);
    Register val = R0.payloadReg();
    switch (op) {
      case JSOP_BITNOT:
        masm.ma_not(val, val);
        break;
      case JSOP_NEG:
        // Guard against 0 and MIN_INT, both result in a double.
        masm.branchTest32(Assembler::Zero, val, Imm32(0x7fffffff), &failure);
        masm.ma_negu(val, val);
        break;
      case JSOP_INC:
        masm.ma_addu(val, val, Imm32(1));
        break;
      case JSOP_DEC:
        masm.ma_addu(val, val, Imm32(-1));
        break;
      default:
        MOZ_CRASH("unexpected op");
    }
    masm.tagValue(JSVAL_TYPE_INT32, val, R0);
    EmitReturnFromIC(masm);
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}
} }
