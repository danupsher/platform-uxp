/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#ifndef jit_ppc_CodeGenerator_ppc_h
#define jit_ppc_CodeGenerator_ppc_h
#include "jit/shared/CodeGenerator-shared.h"

namespace js { namespace jit {

class OutOfLineBailout;
class OutOfLineTableSwitch;
class OutOfLineWasmTruncateCheck;
class CodeGeneratorPPC : public CodeGeneratorShared {
  protected:
    CodeGeneratorPPC(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm);
    ValueOperand ToValue(LInstruction* ins, size_t pos);
    ValueOperand ToOutValue(LInstruction* ins);
    ValueOperand ToTempValue(LInstruction* ins, size_t pos);
    Register splitTagForTest(const ValueOperand& value);
    MoveOperand toMoveOperand(LAllocation a) const;
    void testNullEmitBranch(Assembler::Condition cond, const ValueOperand& value, MBasicBlock* ifTrue, MBasicBlock* ifFalse);
    void testUndefinedEmitBranch(Assembler::Condition cond, const ValueOperand& value, MBasicBlock* ifTrue, MBasicBlock* ifFalse);
    void testObjectEmitBranch(Assembler::Condition cond, const ValueOperand& value, MBasicBlock* ifTrue, MBasicBlock* ifFalse);
    void emitTableSwitchDispatch(MTableSwitch* mir, Register index, Register base);

    NonAssertingLabel deoptLabel_;

    void bailoutFrom(Label* label, LSnapshot* snapshot);
    void bailoutIf(Assembler::Condition cond, LSnapshot* snapshot) {
        Label bail;
        masm.ma_b(&bail, cond);
        bailoutFrom(&bail, snapshot);
    }
    void bailout(LSnapshot* snapshot);

    template <typename T1, typename T2>
    void bailoutCmp32(Assembler::Condition c, T1 lhs, T2 rhs, LSnapshot* snapshot) {
        Label bail;
        masm.branch32(c, lhs, rhs, &bail);
        bailoutFrom(&bail, snapshot);
    }
    template<typename T>
    void bailoutTest32(Assembler::Condition c, Register lhs, T rhs, LSnapshot* snapshot) {
        Label bail;
        masm.branchTest32(c, lhs, rhs, &bail);
        bailoutFrom(&bail, snapshot);
    }
    template <typename T1, typename T2>
    void bailoutCmpPtr(Assembler::Condition c, T1 lhs, T2 rhs, LSnapshot* snapshot) {
        Label bail;
        masm.branchPtr(c, lhs, rhs, &bail);
        bailoutFrom(&bail, snapshot);
    }
    void bailoutTestPtr(Assembler::Condition c, Register lhs, Register rhs, LSnapshot* snapshot) {
        Label bail;
        masm.branchTestPtr(c, lhs, rhs, &bail);
        bailoutFrom(&bail, snapshot);
    }
    void bailoutIfFalseBool(Register reg, LSnapshot* snapshot) {
        Label bail;
        masm.branchTest32(Assembler::Zero, reg, Imm32(0xFF), &bail);
        bailoutFrom(&bail, snapshot);
    }

    template <typename T>
    void branchToBlock(Register lhs, T rhs, MBasicBlock* mir, Assembler::Condition cond)
    {
        mir = skipTrivialBlocks(mir);
        Label* label = mir->lir()->label();
        masm.ma_b(lhs, rhs, label, cond);
    }


    template <typename T>
    void emitBranch(Register lhs, T rhs, Assembler::Condition cond,
                    MBasicBlock* mirTrue, MBasicBlock* mirFalse)
    {
        if (isNextBlock(mirFalse->lir())) {
            branchToBlock(lhs, rhs, mirTrue, cond);
        } else {
            branchToBlock(lhs, rhs, mirFalse, Assembler::InvertCondition(cond));
            jumpToBlock(mirTrue);
        }
    }

    void testZeroEmitBranch(Assembler::Condition cond, Register reg,
                            MBasicBlock* ifTrue, MBasicBlock* ifFalse)
    {
        emitBranch(reg, Imm32(0), cond, ifTrue, ifFalse);
    }

    bool generateOutOfLineCode();

  public:
    void generateInvalidateEpilogue();

    // Instruction visitors (platform-specific, correspond to MIPS-shared layer)
    void visitMinMaxD(LMinMaxD* ins);
    void visitMinMaxF(LMinMaxF* ins);
    void visitAbsD(LAbsD* ins);
    void visitAbsF(LAbsF* ins);
    void visitSqrtD(LSqrtD* ins);
    void visitSqrtF(LSqrtF* ins);
    void visitAddI(LAddI* ins);
    void visitAddI64(LAddI64* ins);
    void visitSubI(LSubI* ins);
    void visitSubI64(LSubI64* ins);
    void visitBitNotI(LBitNotI* ins);
    void visitBitOpI(LBitOpI* ins);
    void visitBitOpI64(LBitOpI64* ins);
    void visitMulI(LMulI* ins);
    void visitMulI64(LMulI64* ins);
    void visitDivI(LDivI* ins);
    void visitDivPowTwoI(LDivPowTwoI* ins);
    void visitModI(LModI* ins);
    void visitModPowTwoI(LModPowTwoI* ins);
    void visitModMaskI(LModMaskI* ins);
    void visitPowHalfD(LPowHalfD* ins);
    void visitShiftI(LShiftI* ins);
    void visitShiftI64(LShiftI64* ins);
    void visitRotateI64(LRotateI64* lir);
    void visitUrshD(LUrshD* ins);
    void visitClzI(LClzI* ins);
    void visitCtzI(LCtzI* ins);
    void visitPopcntI(LPopcntI* ins);
    void visitPopcntI64(LPopcntI64* lir);
    void visitTestIAndBranch(LTestIAndBranch* test);
    void visitCompare(LCompare* comp);
    void visitCompareAndBranch(LCompareAndBranch* comp);
    void visitTestDAndBranch(LTestDAndBranch* test);
    void visitTestFAndBranch(LTestFAndBranch* test);
    void visitCompareD(LCompareD* comp);
    void visitCompareF(LCompareF* comp);
    void visitCompareDAndBranch(LCompareDAndBranch* comp);
    void visitCompareFAndBranch(LCompareFAndBranch* comp);
    void visitBitAndAndBranch(LBitAndAndBranch* lir);
    void visitWasmUint32ToDouble(LWasmUint32ToDouble* lir);
    void visitWasmUint32ToFloat32(LWasmUint32ToFloat32* lir);
    void visitNotI(LNotI* ins);
    void visitNotD(LNotD* ins);
    void visitNotF(LNotF* ins);
    void visitMathD(LMathD* math);
    void visitMathF(LMathF* math);
    void visitFloor(LFloor* lir);
    void visitFloorF(LFloorF* lir);
    void visitCeil(LCeil* lir);
    void visitCeilF(LCeilF* lir);
    void visitRound(LRound* lir);
    void visitRoundF(LRoundF* lir);
    void visitTruncateDToInt32(LTruncateDToInt32* ins);
    void visitTruncateFToInt32(LTruncateFToInt32* ins);
    void visitWasmTruncateToInt32(LWasmTruncateToInt32* lir);
    void visitWasmLoadGlobalVar(LWasmLoadGlobalVar* ins);
    void visitWasmStoreGlobalVar(LWasmStoreGlobalVar* ins);
    void visitOutOfLineWasmTruncateCheck(OutOfLineWasmTruncateCheck* ool);
    void visitCopySignD(LCopySignD* ins);
    void visitCopySignF(LCopySignF* ins);
    void visitValue(LValue* value);
    void visitDouble(LDouble* ins);
    void visitFloat32(LFloat32* ins);
    void visitGuardShape(LGuardShape* guard);
    void visitGuardObjectGroup(LGuardObjectGroup* guard);
    void visitGuardClass(LGuardClass* guard);
    void visitNegI(LNegI* lir);
    void visitNegD(LNegD* lir);
    void visitNegF(LNegF* lir);
    void visitLoadTypedArrayElementStatic(LLoadTypedArrayElementStatic* ins);
    void visitStoreTypedArrayElementStatic(LStoreTypedArrayElementStatic* ins);
    void visitWasmCall(LWasmCall* ins);
    void visitWasmCallI64(LWasmCallI64* ins);
    void visitWasmLoad(LWasmLoad* ins);
    void visitWasmStore(LWasmStore* ins);
    void visitWasmAddOffset(LWasmAddOffset* ins);
    void visitAsmJSLoadHeap(LAsmJSLoadHeap* ins);
    void visitAsmJSStoreHeap(LAsmJSStoreHeap* ins);
    void visitAsmJSCompareExchangeHeap(LAsmJSCompareExchangeHeap* ins);
    void visitAsmJSAtomicExchangeHeap(LAsmJSAtomicExchangeHeap* ins);
    void visitAsmJSAtomicBinopHeap(LAsmJSAtomicBinopHeap* ins);
    void visitAsmJSAtomicBinopHeapForEffect(LAsmJSAtomicBinopHeapForEffect* ins);
    void visitWasmStackArg(LWasmStackArg* ins);
    void visitWasmStackArgI64(LWasmStackArgI64* ins);
    void visitWasmSelect(LWasmSelect* ins);
    void visitWasmReinterpret(LWasmReinterpret* ins);
    void visitMemoryBarrier(LMemoryBarrier* ins);
    void visitAtomicTypedArrayElementBinop(LAtomicTypedArrayElementBinop* lir);
    void visitAtomicTypedArrayElementBinopForEffect(LAtomicTypedArrayElementBinopForEffect* lir);
    void visitCompareExchangeTypedArrayElement(LCompareExchangeTypedArrayElement* lir);
    void visitAtomicExchangeTypedArrayElement(LAtomicExchangeTypedArrayElement* lir);
    void visitEffectiveAddress(LEffectiveAddress* ins);
    void visitUDivOrMod(LUDivOrMod* ins);


    void visitBox(LBox* box);
    void visitBoxFloatingPoint(LBoxFloatingPoint* box);
    void visitUnbox(LUnbox* unbox);
    void visitCompareB(LCompareB* lir);
    void visitCompareBAndBranch(LCompareBAndBranch* lir);
    void visitCompareBitwise(LCompareBitwise* lir);
    void visitCompareBitwiseAndBranch(LCompareBitwiseAndBranch* lir);
    void visitOutOfLineBailout(OutOfLineBailout* ool);
    void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);
    void setReturnDoubleRegs(LiveRegisterSet* regs);
};
typedef CodeGeneratorPPC CodeGeneratorSpecific;
} }
#endif
