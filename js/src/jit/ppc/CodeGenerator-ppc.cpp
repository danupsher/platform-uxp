/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#include "jit/ppc/CodeGenerator-ppc.h"
#include "jit/ppc/Assembler-ppc.h"
#include "jit/ppc/MacroAssembler-ppc.h"
#include "jit/ppc/LIR-ppc.h"
#include "jit/CodeGenerator.h"
#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "js/Conversions.h"
#include "vm/Shape.h"
#include "vm/TraceLogging.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"

using namespace js;
using namespace js::jit;

// ========================================================================
// OutOfLineTableSwitch

class js::jit::OutOfLineTableSwitch : public OutOfLineCodeBase<CodeGeneratorPPC>
{
    MTableSwitch* mir_;
    CodeLabel jumpLabel_;

    void accept(CodeGeneratorPPC* codegen) {
        codegen->visitOutOfLineTableSwitch(this);
    }

  public:
    OutOfLineTableSwitch(MTableSwitch* mir)
      : mir_(mir)
    {}

    MTableSwitch* mir() const { return mir_; }
    CodeLabel* jumpLabel() { return &jumpLabel_; }
};

// ========================================================================
// OutOfLineBailout

namespace js {
namespace jit {
class OutOfLineBailout : public OutOfLineCodeBase<CodeGeneratorPPC>
{
    LSnapshot* snapshot_;

  public:
    OutOfLineBailout(LSnapshot* snapshot)
      : snapshot_(snapshot)
    { }

    void accept(CodeGeneratorPPC* codegen) {
        codegen->visitOutOfLineBailout(this);
    }

    LSnapshot* snapshot() const { return snapshot_; }
};
} // namespace jit
} // namespace js

// ========================================================================
// FrameSizeClass

static const uint32_t FrameSizes[] = { 128, 256, 512, 1024 };

__attribute__((visibility("default")))
FrameSizeClass
FrameSizeClass::FromDepth(uint32_t frameDepth)
{
    for (uint32_t i = 0; i < JS_ARRAY_LENGTH(FrameSizes); i++) {
        if (frameDepth < FrameSizes[i])
            return FrameSizeClass(i);
    }
    return FrameSizeClass::None();
}

__attribute__((visibility("default")))
FrameSizeClass
FrameSizeClass::ClassLimit()
{
    return FrameSizeClass(JS_ARRAY_LENGTH(FrameSizes));
}

__attribute__((visibility("default")))
uint32_t
FrameSizeClass::frameSize() const
{
    MOZ_ASSERT(class_ != NO_FRAME_SIZE_CLASS_ID);
    MOZ_ASSERT(class_ < JS_ARRAY_LENGTH(FrameSizes));
    return FrameSizes[class_];
}

// ========================================================================
// Constructor and value helpers

CodeGeneratorPPC::CodeGeneratorPPC(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm)
  : CodeGeneratorShared(gen, graph, masm)
{ }

ValueOperand
CodeGeneratorPPC::ToValue(LInstruction* ins, size_t pos)
{
    return ValueOperand(ToRegister(ins->getOperand(pos + TYPE_INDEX)),
                        ToRegister(ins->getOperand(pos + PAYLOAD_INDEX)));
}

ValueOperand
CodeGeneratorPPC::ToOutValue(LInstruction* ins)
{
    return ValueOperand(ToRegister(ins->getDef(TYPE_INDEX)),
                        ToRegister(ins->getDef(PAYLOAD_INDEX)));
}

ValueOperand
CodeGeneratorPPC::ToTempValue(LInstruction* ins, size_t pos)
{
    return ValueOperand(ToRegister(ins->getTemp(pos + TYPE_INDEX)),
                        ToRegister(ins->getTemp(pos + PAYLOAD_INDEX)));
}

Register
CodeGeneratorPPC::splitTagForTest(const ValueOperand& value)
{
    return value.typeReg();
}

MoveOperand
CodeGeneratorPPC::toMoveOperand(LAllocation a) const
{
    if (a.isGeneralReg())
        return MoveOperand(ToRegister(a));
    if (a.isFloatReg())
        return MoveOperand(ToFloatRegister(a));
    int32_t offset = ToStackOffset(&a);
    MOZ_ASSERT((offset & 3) == 0);
    return MoveOperand(StackPointer, offset);
}

// ========================================================================
// Bailout infrastructure

void
CodeGeneratorPPC::bailoutFrom(Label* label, LSnapshot* snapshot)
{
    if (masm.bailed())
        return;

    MOZ_ASSERT_IF(!masm.oom(), label->used());
    MOZ_ASSERT_IF(!masm.oom(), !label->bound());

    encode(snapshot);

    MOZ_ASSERT_IF(frameClass_ != FrameSizeClass::None(),
                  frameClass_.frameSize() == masm.framePushed());

    InlineScriptTree* tree = snapshot->mir()->block()->trackedTree();
    OutOfLineBailout* ool = new(alloc()) OutOfLineBailout(snapshot);
    addOutOfLineCode(ool, new(alloc()) BytecodeSite(tree, tree->script()->code()));

    // Retarget the label to the OOL entry (like MIPS).
    // This patches the branch instruction(s) that reference this label
    // to jump directly to the OOL bailout code when it's bound later.
    // We must NOT use bind(label)+jump(ool) here because that would emit
    // a jump instruction in the fall-through path of the far branch pattern
    // used by EmitBranchOnCondition.
    masm.retarget(label, ool->entry());
}

void
CodeGeneratorPPC::bailout(LSnapshot* snapshot)
{
    Label label;
    masm.jump(&label);
    bailoutFrom(&label, snapshot);
}

void
CodeGeneratorPPC::visitOutOfLineBailout(OutOfLineBailout* ool)
{
    // Push snapshot offset to stack for the bailout handler.
    masm.subPtr(Imm32(2 * sizeof(void*)), StackPointer);
    masm.storePtr(ImmWord(ool->snapshot()->snapshotOffset()),
                  Address(StackPointer, 0));

    masm.jump(&deoptLabel_);
}

bool
CodeGeneratorPPC::generateOutOfLineCode()
{
    if (!CodeGeneratorShared::generateOutOfLineCode())
        return false;

    if (deoptLabel_.used()) {
        masm.bind(&deoptLabel_);

        // Load the Ion frame size into r0 for the bailout handler.
        // (Matches MIPS pattern of loading frameSize into ra.)
        masm.ma_li(r0, Imm32(frameSize()));

        JitCode* handler = gen->jitRuntime()->getGenericBailoutHandler();
        masm.branch(handler);
    }

    return true;
}

void
CodeGeneratorPPC::generateInvalidateEpilogue()
{
    for (size_t i = 0; i < sizeof(void*); i += Assembler::NopSize())
        masm.nop();

    masm.bind(&invalidate_);

    // Push the LR (return address of the point that we bailed out at).
    masm.as_mflr(ScratchRegister);
    masm.Push(ScratchRegister);

    // Push the Ion script onto the stack (to be patched).
    invalidateEpilogueData_ = masm.pushWithPatch(ImmWord(uintptr_t(-1)));

    JitCode* thunk = gen->jitRuntime()->getInvalidationThunk();
    masm.branch(thunk);

    masm.assumeUnreachable("Should have returned directly to its caller instead of here.");
}

// ========================================================================
// Test/branch emit helpers

void
CodeGeneratorPPC::testNullEmitBranch(Assembler::Condition cond, const ValueOperand& value,
                                     MBasicBlock* ifTrue, MBasicBlock* ifFalse)
{
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    emitBranch(value.typeReg(), ImmTag(JSVAL_TAG_NULL), cond, ifTrue, ifFalse);
}

void
CodeGeneratorPPC::testUndefinedEmitBranch(Assembler::Condition cond, const ValueOperand& value,
                                          MBasicBlock* ifTrue, MBasicBlock* ifFalse)
{
    emitBranch(value.typeReg(), ImmTag(JSVAL_TAG_UNDEFINED), cond, ifTrue, ifFalse);
}

void
CodeGeneratorPPC::testObjectEmitBranch(Assembler::Condition cond, const ValueOperand& value,
                                       MBasicBlock* ifTrue, MBasicBlock* ifFalse)
{
    emitBranch(value.typeReg(), ImmTag(JSVAL_TAG_OBJECT), cond, ifTrue, ifFalse);
}

// ========================================================================
// Box/Unbox

void
CodeGeneratorPPC::visitBox(LBox* box)
{
    const LDefinition* type = box->getDef(TYPE_INDEX);
    const LDefinition* payload = box->getDef(PAYLOAD_INDEX);
    const LAllocation* in = box->getOperand(0);
    MOZ_ASSERT(!in->isConstant());

    // Write type tag
    masm.move32(Imm32(MIRTypeToTag(box->type())), ToRegister(type));

    // Move input to payload output if they differ.
    // defineBox allocates a separate register for the payload,
    // so we must explicitly move the input value there.
    if (ToRegister(in) != ToRegister(payload))
        masm.movePtr(ToRegister(in), ToRegister(payload));
}

void
CodeGeneratorPPC::visitBoxFloatingPoint(LBoxFloatingPoint* box)
{
    const LDefinition* payload = box->getDef(PAYLOAD_INDEX);
    const LDefinition* type = box->getDef(TYPE_INDEX);
    FloatRegister reg = ToFloatRegister(box->getOperand(0));
    if (box->type() == MIRType::Float32) {
        masm.convertFloat32ToDouble(reg, ScratchDoubleReg);
        reg = ScratchDoubleReg;
    }
    // Store double to stack, read back type and payload (big-endian).
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(reg, StackPointer, 0);
    masm.as_lwz(ToRegister(type), StackPointer, 0);     // high word = type tag
    masm.as_lwz(ToRegister(payload), StackPointer, 4);   // low word = payload
    masm.addPtr(Imm32(8), StackPointer);
}

void
CodeGeneratorPPC::visitUnbox(LUnbox* unbox)
{
    MUnbox* mir = unbox->mir();
    Register type = ToRegister(unbox->type());


    if (mir->fallible()) {
        bailoutCmp32(Assembler::NotEqual, type, Imm32(MIRTypeToTag(mir->type())),
                     unbox->snapshot());
    }

    // NunBox32 FIX: The RA may assign the output register differently from
    // the payload register (especially under register pressure). Ensure the
    // unboxed payload ends up in the output register.
    Register payload = ToRegister(unbox->payload());
    Register result = ToRegister(unbox->output());
    if (payload != result)
        masm.movePtr(payload, result);

    // PPC safety: guard against null object pointers that would SIGBUS.
    // On PPC big-endian, type inference can be wrong in edge cases.
    if (mir->type() == MIRType::Object && unbox->snapshot()) {
        bailoutCmpPtr(Assembler::Equal, result, ImmPtr(nullptr), unbox->snapshot());
    }
}

// ========================================================================
// Table switch

void
CodeGeneratorPPC::visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool)
{
    MTableSwitch* mir = ool->mir();

    masm.haltingAlign(sizeof(void*));
    masm.bind(ool->jumpLabel()->target());
    masm.addCodeLabel(*ool->jumpLabel());

    for (size_t i = 0; i < mir->numCases(); i++) {
        LBlock* caseblock = skipTrivialBlocks(mir->getCase(i))->lir();
        Label* caseheader = caseblock->label();
        uint32_t caseoffset = caseheader->offset();

        CodeLabel cl;
        masm.ma_li(ScratchRegister, cl.patchAt());
        masm.branch(ScratchRegister);
        cl.target()->bind(caseoffset);
        masm.addCodeLabel(cl);
    }
}

void
CodeGeneratorPPC::emitTableSwitchDispatch(MTableSwitch* mir, Register index, Register address)
{
    Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();

    if (mir->low() != 0)
        masm.subPtr(Imm32(mir->low()), index);

    int32_t cases = mir->numCases();
    masm.branchPtr(Assembler::AboveOrEqual, index, ImmWord(cases), defaultcase);

    OutOfLineTableSwitch* ool = new(alloc()) OutOfLineTableSwitch(mir);
    addOutOfLineCode(ool, mir);

    masm.ma_li(address, ool->jumpLabel()->patchAt());
    // Each table entry is: li32 (8 bytes) + branch (4 bytes) = 12 bytes?
    // Actually MIPS uses lshiftPtr(Imm32(4), index) = multiply by 16.
    // PPC: li32 = lis+ori (8 bytes) + mtctr+bctr (8 bytes) = 16 bytes per entry.
    // Actually masm.branch(reg) = mtctr + bctr = 8 bytes.
    // lis+ori = 8 bytes. Total = 16 bytes per entry. Shift by 4.
    masm.lshiftPtr(Imm32(4), index);
    masm.addPtr(index, address);

    masm.branch(address);
}

// ========================================================================
// Compare visitors

void
CodeGeneratorPPC::visitCompareB(LCompareB* lir)
{
    MCompare* mir = lir->mir();
    const ValueOperand lhs = ToValue(lir, LCompareB::Lhs);
    const LAllocation* rhs = lir->rhs();
    const Register output = ToRegister(lir->output());

    MOZ_ASSERT(mir->jsop() == JSOP_STRICTEQ || mir->jsop() == JSOP_STRICTNE);
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());

    Label notBoolean, done;
    masm.branchTestBoolean(Assembler::NotEqual, lhs, &notBoolean);
    {
        if (rhs->isConstant())
            masm.cmp32Set(cond, lhs.payloadReg(), Imm32(rhs->toConstant()->toBoolean()), output);
        else
            masm.cmp32Set(cond, lhs.payloadReg(), ToRegister(rhs), output);
        masm.jump(&done);
    }
    masm.bind(&notBoolean);
    {
        masm.move32(Imm32(mir->jsop() == JSOP_STRICTNE), output);
    }
    masm.bind(&done);
}

void
CodeGeneratorPPC::visitCompareBAndBranch(LCompareBAndBranch* lir)
{
    MCompare* mir = lir->cmpMir();
    const ValueOperand lhs = ToValue(lir, LCompareBAndBranch::Lhs);
    const LAllocation* rhs = lir->rhs();

    MOZ_ASSERT(mir->jsop() == JSOP_STRICTEQ || mir->jsop() == JSOP_STRICTNE);

    MBasicBlock* mirNotBoolean = (mir->jsop() == JSOP_STRICTEQ) ? lir->ifFalse() : lir->ifTrue();
    branchToBlock(lhs.typeReg(), ImmType(JSVAL_TYPE_BOOLEAN), mirNotBoolean, Assembler::NotEqual);

    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    if (rhs->isConstant())
        emitBranch(lhs.payloadReg(), Imm32(rhs->toConstant()->toBoolean()), cond,
                   lir->ifTrue(), lir->ifFalse());
    else
        emitBranch(lhs.payloadReg(), ToRegister(rhs), cond, lir->ifTrue(), lir->ifFalse());
}

void
CodeGeneratorPPC::visitCompareBitwise(LCompareBitwise* lir)
{
    MCompare* mir = lir->mir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const ValueOperand lhs = ToValue(lir, LCompareBitwise::LhsInput);
    const ValueOperand rhs = ToValue(lir, LCompareBitwise::RhsInput);
    const Register output = ToRegister(lir->output());

    MOZ_ASSERT(IsEqualityOp(mir->jsop()));

    Label notEqual, done;
    masm.ma_b(lhs.typeReg(), rhs.typeReg(), &notEqual, Assembler::NotEqual);
    {
        masm.cmp32Set(cond, lhs.payloadReg(), rhs.payloadReg(), output);
        masm.jump(&done);
    }
    masm.bind(&notEqual);
    {
        masm.move32(Imm32(cond == Assembler::NotEqual), output);
    }
    masm.bind(&done);
}

void
CodeGeneratorPPC::visitCompareBitwiseAndBranch(LCompareBitwiseAndBranch* lir)
{
    MCompare* mir = lir->cmpMir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const ValueOperand lhs = ToValue(lir, LCompareBitwiseAndBranch::LhsInput);
    const ValueOperand rhs = ToValue(lir, LCompareBitwiseAndBranch::RhsInput);

    MOZ_ASSERT(mir->jsop() == JSOP_EQ || mir->jsop() == JSOP_STRICTEQ ||
               mir->jsop() == JSOP_NE || mir->jsop() == JSOP_STRICTNE);

    MBasicBlock* notEqual = (cond == Assembler::Equal) ? lir->ifFalse() : lir->ifTrue();
    branchToBlock(lhs.typeReg(), rhs.typeReg(), notEqual, Assembler::NotEqual);
    emitBranch(lhs.payloadReg(), rhs.payloadReg(), cond, lir->ifTrue(), lir->ifFalse());
}

void
CodeGeneratorPPC::setReturnDoubleRegs(LiveRegisterSet* regs)
{
    MOZ_ASSERT(ReturnDoubleReg.isDouble());
    regs->add(ReturnDoubleReg);
}

// ========================================================================
// Test and branch

void
CodeGeneratorPPC::visitTestIAndBranch(LTestIAndBranch* test)
{
    const LAllocation* opd = test->getOperand(0);
    Register reg = ToRegister(opd);
    emitBranch(reg, Imm32(0), Assembler::NonZero, test->ifTrue(), test->ifFalse());
}

void
CodeGeneratorPPC::visitCompare(LCompare* comp)
{
    MCompare* mir = comp->mir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const LAllocation* left = comp->getOperand(0);
    const LAllocation* right = comp->getOperand(1);
    Register output = ToRegister(comp->output());

    if (right->isConstant())
        masm.cmp32Set(cond, ToRegister(left), Imm32(ToInt32(right)), output);
    else if (right->isGeneralReg())
        masm.cmp32Set(cond, ToRegister(left), ToRegister(right), output);
    else {
        masm.load32(Address(StackPointer, ToStackOffset(right)), ScratchRegister);
        masm.cmp32Set(cond, ToRegister(left), ScratchRegister, output);
    }
}

void
CodeGeneratorPPC::visitCompareAndBranch(LCompareAndBranch* comp)
{
    MCompare* mir = comp->cmpMir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const LAllocation* left = comp->left();
    const LAllocation* right = comp->right();

    if (right->isConstant())
        emitBranch(ToRegister(left), Imm32(ToInt32(right)), cond,
                   comp->ifTrue(), comp->ifFalse());
    else if (right->isGeneralReg())
        emitBranch(ToRegister(left), ToRegister(right), cond,
                   comp->ifTrue(), comp->ifFalse());
    else {
        masm.load32(Address(StackPointer, ToStackOffset(right)), ScratchRegister);
        emitBranch(ToRegister(left), ScratchRegister, cond,
                   comp->ifTrue(), comp->ifFalse());
    }
}

// ========================================================================
// Integer arithmetic

void
CodeGeneratorPPC::visitAddI(LAddI* ins)
{
    const LAllocation* lhs = ins->getOperand(0);
    const LAllocation* rhs = ins->getOperand(1);
    const LDefinition* dest = ins->getDef(0);

    if (!ins->snapshot()) {
        if (rhs->isConstant())
            masm.ma_addu(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)));
        else
            masm.ma_addu(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
        return;
    }

    Label overflow;
    if (rhs->isConstant())
        masm.ma_addTestOverflow(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)), &overflow);
    else
        masm.ma_addTestOverflow(ToRegister(dest), ToRegister(lhs), ToRegister(rhs), &overflow);
    bailoutFrom(&overflow, ins->snapshot());
}

void
CodeGeneratorPPC::visitSubI(LSubI* ins)
{
    const LAllocation* lhs = ins->getOperand(0);
    const LAllocation* rhs = ins->getOperand(1);
    const LDefinition* dest = ins->getDef(0);

    if (!ins->snapshot()) {
        if (rhs->isConstant())
            masm.ma_subu(ToRegister(dest), ToRegister(lhs), Imm32(ToInt32(rhs)));
        else
            masm.ma_subu(ToRegister(dest), ToRegister(lhs), ToRegister(rhs));
        return;
    }

    Label overflow;
    if (rhs->isConstant()) {
        masm.ma_li(ScratchRegister, Imm32(ToInt32(rhs)));
        masm.ma_subTestOverflow(ToRegister(dest), ToRegister(lhs), ScratchRegister, &overflow);
    } else {
        masm.ma_subTestOverflow(ToRegister(dest), ToRegister(lhs), ToRegister(rhs), &overflow);
    }
    bailoutFrom(&overflow, ins->snapshot());
}

void
CodeGeneratorPPC::visitMulI(LMulI* ins)
{
    const LAllocation* lhs = ins->getOperand(0);
    const LAllocation* rhs = ins->getOperand(1);
    Register dest = ToRegister(ins->getDef(0));
    Register lhsReg = ToRegister(lhs);
    MMul* mul = ins->mir();

    if (rhs->isConstant()) {
        int32_t constant = ToInt32(rhs);
        Register src = lhsReg;

        // Special cases for constant multipliers.
        switch (constant) {
          case -1:
            if (mul->canOverflow())
                bailoutCmp32(Assembler::Equal, src, Imm32(INT32_MIN), ins->snapshot());
            masm.ma_negu(dest, src);
            return;
          case 0:
            if (mul->canBeNegativeZero()) {
                bailoutCmp32(Assembler::LessThan, src, Imm32(0), ins->snapshot());
            }
            masm.move32(Imm32(0), dest);
            return;
          case 1:
            masm.move32(src, dest);
            return;
          case 2:
            if (mul->canOverflow()) {
                Label overflow;
                masm.ma_addTestOverflow(dest, src, src, &overflow);
                bailoutFrom(&overflow, ins->snapshot());
            } else {
                masm.ma_addu(dest, src, src);
            }
            return;
          default:
            break;
        }

        // General constant multiply.
        masm.ma_li(ScratchRegister, Imm32(constant));
        masm.as_mullw(dest, src, ScratchRegister);
        if (mul->canOverflow()) {
            masm.as_mulhw(SecondScratchRegister, src, ScratchRegister);
            masm.as_srawi(ScratchRegister, dest, 31);
            bailoutCmp32(Assembler::NotEqual, SecondScratchRegister, ScratchRegister,
                         ins->snapshot());
        }
    } else {
        Register rhsReg = ToRegister(rhs);
        masm.as_mullw(dest, lhsReg, rhsReg);

        if (mul->canOverflow()) {
            masm.as_mulhw(SecondScratchRegister, lhsReg, rhsReg);
            masm.as_srawi(ScratchRegister, dest, 31);
            bailoutCmp32(Assembler::NotEqual, SecondScratchRegister, ScratchRegister,
                         ins->snapshot());
        }
    }

    if (mul->canBeNegativeZero()) {
        Label done;
        masm.ma_b(dest, Imm32(0), &done, Assembler::NotEqual);
        // Result is zero. Check if either operand was negative.
        Register check = rhs->isConstant() ? ScratchRegister : ToRegister(rhs);
        if (rhs->isConstant())
            masm.ma_li(ScratchRegister, Imm32(ToInt32(rhs)));
        masm.as_or(ScratchRegister, lhsReg, check);
        bailoutCmp32(Assembler::Signed, ScratchRegister, ScratchRegister, ins->snapshot());
        masm.bind(&done);
    }
}

void
CodeGeneratorPPC::visitDivI(LDivI* ins)
{
    Register lhs = ToRegister(ins->lhs());
    Register rhs = ToRegister(ins->rhs());
    Register dest = ToRegister(ins->output());
    Register temp = ToRegister(ins->getTemp(0));
    MDiv* mir = ins->mir();

    Label done;

    // Handle divide by zero.
    if (mir->canBeDivideByZero()) {
        if (mir->trapOnError()) {
            masm.ma_b(rhs, Imm32(0), trap(mir, wasm::Trap::IntegerDivideByZero),
                      Assembler::Equal);
        } else if (mir->canTruncateInfinities()) {
            Label notzero;
            masm.ma_b(rhs, Imm32(0), &notzero, Assembler::NotEqual);
            masm.move32(Imm32(0), dest);
            masm.jump(&done);
            masm.bind(&notzero);
        } else {
            bailoutCmp32(Assembler::Equal, rhs, Imm32(0), ins->snapshot());
        }
    }

    // Handle INT_MIN / -1 (overflow).
    if (mir->canBeNegativeOverflow()) {
        Label notMinInt;
        masm.ma_b(lhs, Imm32(INT32_MIN), &notMinInt, Assembler::NotEqual);
        masm.ma_b(rhs, Imm32(-1), &notMinInt, Assembler::NotEqual);
        if (mir->trapOnError()) {
            masm.jump(trap(mir, wasm::Trap::IntegerOverflow));
        } else if (mir->canTruncateOverflow()) {
            masm.move32(Imm32(INT32_MIN), dest);
            masm.jump(&done);
        } else {
            bailout(ins->snapshot());
        }
        masm.bind(&notMinInt);
    }

    // Perform the division.
    masm.as_divw(dest, lhs, rhs);

    // Check for exact division (no remainder).
    if (!mir->canTruncateRemainder()) {
        // remainder = lhs - (dest * rhs)
        masm.as_mullw(temp, dest, rhs);
        bailoutCmp32(Assembler::NotEqual, temp, lhs, ins->snapshot());
    }

    // Handle negative zero result.
    if (mir->canBeNegativeZero()) {
        Label notZero;
        masm.ma_b(dest, Imm32(0), &notZero, Assembler::NotEqual);
        // Result is zero. If lhs < 0, we have -0.
        bailoutCmp32(Assembler::LessThan, lhs, Imm32(0), ins->snapshot());
        masm.bind(&notZero);
    }

    masm.bind(&done);
}

void
CodeGeneratorPPC::visitDivPowTwoI(LDivPowTwoI* ins)
{
    Register lhs = ToRegister(ins->numerator());
    Register dest = ToRegister(ins->output());
    Register tmp = ToRegister(ins->getTemp(0));
    int32_t shift = ins->shift();

    if (shift == 0) {
        masm.move32(lhs, dest);
        return;
    }

    MDiv* mir = ins->mir();

    if (!mir->isTruncated()) {
        // Check that the remainder is zero (low bits must be 0).
        masm.ma_and(ScratchRegister, lhs, Imm32((1 << shift) - 1));
        bailoutCmp32(Assembler::NotEqual, ScratchRegister, Imm32(0), ins->snapshot());
    }

    if (!mir->canBeNegativeDividend()) {
        // Non-negative: simple arithmetic shift.
        masm.as_srawi(dest, lhs, shift);
        return;
    }

    // Signed division by power of 2:
    // Hacker's Delight: add (2^shift - 1) to negative numbers before shifting.
    // tmp = lhs >> 31 (sign extension: all 1s if negative, all 0s if positive)
    masm.as_srawi(tmp, lhs, 31);
    // Mask to get just the adjustment bits: tmp &= (2^shift - 1)
    // Using rlwinm: rotate 0, mask from bit (32-shift) to bit 31
    masm.as_rlwinm(tmp, tmp, 0, 32 - shift, 31);
    masm.as_add(dest, lhs, tmp);
    masm.as_srawi(dest, dest, shift);
}

void
CodeGeneratorPPC::visitModI(LModI* ins)
{
    Register lhs = ToRegister(ins->lhs());
    Register rhs = ToRegister(ins->rhs());
    Register dest = ToRegister(ins->output());
    Register callTemp = ToRegister(ins->callTemp());
    MMod* mir = ins->mir();
    Label done;

    // Handle divide by zero.
    if (mir->canBeDivideByZero()) {
        if (mir->isTruncated()) {
            Label notzero;
            masm.ma_b(rhs, Imm32(0), &notzero, Assembler::NotEqual);
            masm.move32(Imm32(0), dest);
            masm.jump(&done);
            masm.bind(&notzero);
        } else {
            bailoutCmp32(Assembler::Equal, rhs, Imm32(0), ins->snapshot());
        }
    }

    // Handle INT_MIN % -1 (result is 0, but divw may trap).
    {
        Label notMinInt;
        masm.ma_b(lhs, Imm32(INT32_MIN), &notMinInt, Assembler::NotEqual);
        masm.ma_b(rhs, Imm32(-1), &notMinInt, Assembler::NotEqual);
        masm.move32(Imm32(0), dest);
        masm.jump(&done);
        masm.bind(&notMinInt);
    }

    // remainder = lhs - (lhs / rhs) * rhs
    masm.as_divw(callTemp, lhs, rhs);
    masm.as_mullw(callTemp, callTemp, rhs);
    masm.as_subf(dest, callTemp, lhs);  // subf: dest = lhs - callTemp

    // Handle negative zero.
    if (mir->canBeNegativeDividend()) {
        Label notZero;
        masm.ma_b(dest, Imm32(0), &notZero, Assembler::NotEqual);
        bailoutCmp32(Assembler::LessThan, lhs, Imm32(0), ins->snapshot());
        masm.bind(&notZero);
    }

    masm.bind(&done);
}

void
CodeGeneratorPPC::visitModPowTwoI(LModPowTwoI* ins)
{
    Register lhs = ToRegister(ins->getOperand(0));
    Register dest = ToRegister(ins->getDef(0));
    int32_t shift = ins->shift();
    int32_t mask = (1 << shift) - 1;

    Label negative, done;
    masm.ma_b(lhs, Imm32(0), &negative, Assembler::Signed);

    // Positive case: simple AND.
    masm.ma_and(dest, lhs, Imm32(mask));
    masm.jump(&done);

    masm.bind(&negative);
    // Negative case: negate, AND, negate back.
    masm.ma_negu(dest, lhs);
    masm.ma_and(dest, dest, Imm32(mask));
    masm.ma_negu(dest, dest);

    if (ins->mir()->canBeNegativeDividend()) {
        Label nonzero;
        masm.ma_b(dest, Imm32(0), &nonzero, Assembler::NotEqual);
        bailout(ins->snapshot());
        masm.bind(&nonzero);
    }

    masm.bind(&done);
}

void
CodeGeneratorPPC::visitModMaskI(LModMaskI* ins)
{
    Register src = ToRegister(ins->getOperand(0));
    Register dest = ToRegister(ins->getDef(0));
    Register hold = ToRegister(ins->getTemp(0));
    Register remain = ToRegister(ins->getTemp(1));
    int32_t shift = ins->shift();

    // Compute src % (1 << shift) without division.
    // Algorithm: repeatedly take low 'shift' bits and add them together.
    Label loop, done;

    masm.move32(src, hold);
    masm.move32(Imm32(0), dest);

    // Handle negative input: work with absolute value.
    Label positive;
    masm.ma_b(hold, Imm32(0), &positive, Assembler::NotSigned);
    masm.ma_negu(hold, hold);
    masm.bind(&positive);

    masm.bind(&loop);
    // remain = hold & ((1 << shift) - 1)
    masm.ma_and(remain, hold, Imm32((1 << shift) - 1));
    // dest += remain
    masm.as_add(dest, dest, remain);
    // hold >>= shift
    masm.ma_srl(hold, hold, Imm32(shift));
    // Loop if hold != 0
    masm.ma_b(hold, Imm32(0), &loop, Assembler::NotEqual);

    // Final reduction: if dest >= (1 << shift), repeat.
    Label finalCheck;
    masm.bind(&finalCheck);
    masm.ma_b(dest, Imm32(1 << shift), &done, Assembler::Below);
    masm.ma_and(remain, dest, Imm32((1 << shift) - 1));
    masm.ma_srl(dest, dest, Imm32(shift));
    masm.as_add(dest, dest, remain);
    masm.jump(&finalCheck);

    masm.bind(&done);

    // Restore sign if original was negative.
    Label notNeg;
    masm.ma_b(src, Imm32(0), &notNeg, Assembler::NotSigned);
    // dest == 0 and src < 0 means negative zero — bail.
    if (ins->mir()->canBeNegativeDividend()) {
        Label nonzero;
        masm.ma_b(dest, Imm32(0), &nonzero, Assembler::NotEqual);
        bailout(ins->snapshot());
        masm.bind(&nonzero);
    }
    masm.ma_negu(dest, dest);
    masm.bind(&notNeg);
}

// ========================================================================
// Bitwise operations

void
CodeGeneratorPPC::visitBitNotI(LBitNotI* ins)
{
    const LAllocation* input = ins->getOperand(0);
    Register output = ToRegister(ins->getDef(0));
    masm.ma_not(output, ToRegister(input));
}

void
CodeGeneratorPPC::visitBitOpI(LBitOpI* ins)
{
    const LAllocation* lhs = ins->getOperand(0);
    const LAllocation* rhs = ins->getOperand(1);
    Register dest = ToRegister(ins->getDef(0));

    switch (ins->bitop()) {
      case JSOP_BITOR:
        if (rhs->isConstant())
            masm.ma_or(dest, ToRegister(lhs), Imm32(ToInt32(rhs)));
        else
            masm.as_or(dest, ToRegister(lhs), ToRegister(rhs));
        break;
      case JSOP_BITXOR:
        if (rhs->isConstant())
            masm.ma_xor(dest, ToRegister(lhs), Imm32(ToInt32(rhs)));
        else
            masm.as_xor(dest, ToRegister(lhs), ToRegister(rhs));
        break;
      case JSOP_BITAND:
        if (rhs->isConstant())
            masm.ma_and(dest, ToRegister(lhs), Imm32(ToInt32(rhs)));
        else
            masm.as_and(dest, ToRegister(lhs), ToRegister(rhs));
        break;
      default:
        MOZ_CRASH("unexpected binary opcode");
    }
}

// ========================================================================
// Shifts

void
CodeGeneratorPPC::visitShiftI(LShiftI* ins)
{
    Register lhs = ToRegister(ins->lhs());
    const LAllocation* rhs = ins->rhs();
    Register dest = ToRegister(ins->output());

    if (rhs->isConstant()) {
        int32_t shift = ToInt32(rhs) & 0x1f;
        switch (ins->bitop()) {
          case JSOP_LSH:
            if (shift)
                masm.ma_sll(dest, lhs, Imm32(shift));
            else
                masm.move32(lhs, dest);
            break;
          case JSOP_RSH:
            if (shift)
                masm.ma_sra(dest, lhs, Imm32(shift));
            else
                masm.move32(lhs, dest);
            break;
          case JSOP_URSH:
            if (shift)
                masm.ma_srl(dest, lhs, Imm32(shift));
            else {
                // x >>> 0 can overflow to unsigned.
                masm.move32(lhs, dest);
                if (ins->mir()->toUrsh()->fallible())
                    bailoutCmp32(Assembler::LessThan, dest, Imm32(0), ins->snapshot());
            }
            break;
          default:
            MOZ_CRASH("unexpected shift opcode");
        }
    } else {
        Register shiftReg = ToRegister(rhs);
        // PPC shift instructions use bits [27:31] of the shift register,
        // which gives the right mod-32 behavior.
        switch (ins->bitop()) {
          case JSOP_LSH:
            masm.ma_sll(dest, lhs, shiftReg);
            break;
          case JSOP_RSH:
            masm.ma_sra(dest, lhs, shiftReg);
            break;
          case JSOP_URSH:
            masm.ma_srl(dest, lhs, shiftReg);
            if (ins->mir()->toUrsh()->fallible())
                bailoutCmp32(Assembler::LessThan, dest, Imm32(0), ins->snapshot());
            break;
          default:
            MOZ_CRASH("unexpected shift opcode");
        }
    }
}

void
CodeGeneratorPPC::visitUrshD(LUrshD* ins)
{
    Register lhs = ToRegister(ins->lhs());
    Register temp = ToRegister(ins->getTemp(0));
    const LAllocation* rhs = ins->rhs();
    FloatRegister dest = ToFloatRegister(ins->output());

    if (rhs->isConstant()) {
        int32_t shift = ToInt32(rhs) & 0x1f;
        if (shift)
            masm.ma_srl(temp, lhs, Imm32(shift));
        else
            masm.move32(lhs, temp);
    } else {
        masm.ma_srl(temp, lhs, ToRegister(rhs));
    }

    masm.convertUInt32ToDouble(temp, dest);
}

// ========================================================================
// Bit counting

void
CodeGeneratorPPC::visitClzI(LClzI* ins)
{
    Register input = ToRegister(ins->input());
    Register output = ToRegister(ins->output());
    masm.clz32(input, output, false);
}

void
CodeGeneratorPPC::visitCtzI(LCtzI* ins)
{
    Register input = ToRegister(ins->input());
    Register output = ToRegister(ins->output());
    masm.ctz32(input, output, false);
}

void
CodeGeneratorPPC::visitPopcntI(LPopcntI* ins)
{
    Register input = ToRegister(ins->input());
    Register output = ToRegister(ins->output());
    masm.popcnt32(input, output, ScratchRegister);
}

// ========================================================================
// Float math

void
CodeGeneratorPPC::visitMinMaxD(LMinMaxD* ins)
{
    FloatRegister first = ToFloatRegister(ins->first());
    FloatRegister second = ToFloatRegister(ins->second());
    FloatRegister output = ToFloatRegister(ins->output());

    MOZ_ASSERT(first == output);
    if (ins->mir()->isMax())
        masm.maxDouble(second, output, /* handleNaN = */ true);
    else
        masm.minDouble(second, output, /* handleNaN = */ true);
}

void
CodeGeneratorPPC::visitMinMaxF(LMinMaxF* ins)
{
    FloatRegister first = ToFloatRegister(ins->first());
    FloatRegister second = ToFloatRegister(ins->second());
    FloatRegister output = ToFloatRegister(ins->output());

    MOZ_ASSERT(first == output);
    if (ins->mir()->isMax())
        masm.maxFloat32(second, output, /* handleNaN = */ true);
    else
        masm.minFloat32(second, output, /* handleNaN = */ true);
}

void
CodeGeneratorPPC::visitAbsD(LAbsD* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());
    masm.absDouble(input, output);
}

void
CodeGeneratorPPC::visitAbsF(LAbsF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());
    masm.absFloat32(input, output);
}

void
CodeGeneratorPPC::visitSqrtD(LSqrtD* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());
    masm.sqrtDouble(input, output);
}

void
CodeGeneratorPPC::visitSqrtF(LSqrtF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());
    masm.sqrtFloat32(input, output);
}

void
CodeGeneratorPPC::visitMathD(LMathD* math)
{
    FloatRegister src1 = ToFloatRegister(math->getOperand(0));
    FloatRegister src2 = ToFloatRegister(math->getOperand(1));
    FloatRegister output = ToFloatRegister(math->getDef(0));

    // PPC: fadd/fsub/fmul/fdiv take 3 registers. Use as_f* directly.
    switch (math->jsop()) {
      case JSOP_ADD:
        masm.as_fadd(output, src1, src2);
        break;
      case JSOP_SUB:
        masm.as_fsub(output, src1, src2);
        break;
      case JSOP_MUL:
        masm.as_fmul(output, src1, src2);
        break;
      case JSOP_DIV:
        masm.as_fdiv(output, src1, src2);
        break;
      default:
        MOZ_CRASH("unexpected double opcode");
    }
}

void
CodeGeneratorPPC::visitMathF(LMathF* math)
{
    FloatRegister src1 = ToFloatRegister(math->getOperand(0));
    FloatRegister src2 = ToFloatRegister(math->getOperand(1));
    FloatRegister output = ToFloatRegister(math->getDef(0));

    switch (math->jsop()) {
      case JSOP_ADD:
        masm.as_fadds(output, src1, src2);
        break;
      case JSOP_SUB:
        masm.as_fsubs(output, src1, src2);
        break;
      case JSOP_MUL:
        masm.as_fmuls(output, src1, src2);
        break;
      case JSOP_DIV:
        masm.as_fdivs(output, src1, src2);
        break;
      default:
        MOZ_CRASH("unexpected float32 opcode");
    }
}

void
CodeGeneratorPPC::visitPowHalfD(LPowHalfD* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());

    Label done, sqrt;

    // x^0.5 = sqrt(x), but with special handling for -Infinity and -0.

    // Check for -Infinity: return +Infinity.
    masm.loadConstantDouble(-1.0/0.0, ScratchDoubleReg);  // -Inf
    masm.branchDouble(Assembler::DoubleNotEqual, input, ScratchDoubleReg, &sqrt);
    masm.negateDouble(ScratchDoubleReg);
    masm.moveDouble(ScratchDoubleReg, output);
    masm.jump(&done);

    masm.bind(&sqrt);
    // Add 0.0 to handle -0 (converts to +0).
    masm.loadConstantDouble(0.0, ScratchDoubleReg);
    masm.as_fadd(output, input, ScratchDoubleReg);
    masm.sqrtDouble(output, output);

    masm.bind(&done);
}

// ========================================================================
// Floor / Ceil / Round

void
CodeGeneratorPPC::visitFloor(LFloor* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());

    Label bail;

    // Bail on NaN.
    masm.branchDouble(Assembler::DoubleUnordered, input, input, &bail);

    // Truncate toward zero.
    masm.as_fctiwz(ScratchDoubleReg, input);
    // Read integer result (big-endian: integer is in low 32 bits of FPR = offset 4).
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(ScratchDoubleReg, StackPointer, 0);
    masm.as_lwz(output, StackPointer, 4);
    masm.addPtr(Imm32(8), StackPointer);

    // Bail on overflow (fctiwz returns 0x80000000).
    masm.ma_b(output, Imm32(INT32_MIN), &bail, Assembler::Equal);

    // Convert back and compare: if trunc > input, subtract 1 (negative non-integer).
    masm.convertInt32ToDouble(output, ScratchDoubleReg);
    Label done;
    masm.branchDouble(Assembler::DoubleLessThanOrEqual, ScratchDoubleReg, input, &done);
    masm.sub32(Imm32(1), output);

    // Check for -0: if result is 0 and input was negative, bail.
    Label notZero;
    masm.ma_b(output, Imm32(0), &notZero, Assembler::NotEqual);
    // Peek at sign bit of input.
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(input, StackPointer, 0);
    masm.as_lwz(ScratchRegister, StackPointer, 0); // high word
    masm.addPtr(Imm32(8), StackPointer);
    masm.ma_b(ScratchRegister, Imm32(0), &bail, Assembler::LessThan);
    masm.bind(&notZero);

    masm.bind(&done);
    bailoutFrom(&bail, lir->snapshot());
}

void
CodeGeneratorPPC::visitFloorF(LFloorF* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());

    Label bail;

    masm.branchFloat(Assembler::DoubleUnordered, input, input, &bail);

    // Convert to double for fctiwz.
    masm.convertFloat32ToDouble(input, ScratchDoubleReg);
    masm.as_fctiwz(ScratchDoubleReg, ScratchDoubleReg);
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(ScratchDoubleReg, StackPointer, 0);
    masm.as_lwz(output, StackPointer, 4);
    masm.addPtr(Imm32(8), StackPointer);

    masm.ma_b(output, Imm32(INT32_MIN), &bail, Assembler::Equal);

    // Check if floor adjustment needed.
    masm.convertInt32ToFloat32(output, ScratchFloat32Reg);
    Label done;
    masm.branchFloat(Assembler::DoubleLessThanOrEqual, ScratchFloat32Reg, input, &done);
    masm.sub32(Imm32(1), output);

    masm.bind(&done);
    bailoutFrom(&bail, lir->snapshot());
}

void
CodeGeneratorPPC::visitCeil(LCeil* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());

    Label bail;

    masm.branchDouble(Assembler::DoubleUnordered, input, input, &bail);

    // Truncate toward zero.
    masm.as_fctiwz(ScratchDoubleReg, input);
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(ScratchDoubleReg, StackPointer, 0);
    masm.as_lwz(output, StackPointer, 4);
    masm.addPtr(Imm32(8), StackPointer);

    masm.ma_b(output, Imm32(INT32_MIN), &bail, Assembler::Equal);

    // If trunc < input, add 1 (positive non-integer).
    masm.convertInt32ToDouble(output, ScratchDoubleReg);
    Label done;
    masm.branchDouble(Assembler::DoubleGreaterThanOrEqual, ScratchDoubleReg, input, &done);
    masm.add32(Imm32(1), output);

    // Bail on -0: if output == 0, check sign.
    Label notZero;
    masm.ma_b(output, Imm32(0), &notZero, Assembler::NotEqual);
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(input, StackPointer, 0);
    masm.as_lwz(ScratchRegister, StackPointer, 0);
    masm.addPtr(Imm32(8), StackPointer);
    masm.ma_b(ScratchRegister, Imm32(0), &bail, Assembler::LessThan);
    masm.bind(&notZero);

    masm.bind(&done);
    bailoutFrom(&bail, lir->snapshot());
}

void
CodeGeneratorPPC::visitCeilF(LCeilF* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());

    Label bail;
    masm.branchFloat(Assembler::DoubleUnordered, input, input, &bail);

    masm.convertFloat32ToDouble(input, ScratchDoubleReg);
    masm.as_fctiwz(ScratchDoubleReg, ScratchDoubleReg);
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(ScratchDoubleReg, StackPointer, 0);
    masm.as_lwz(output, StackPointer, 4);
    masm.addPtr(Imm32(8), StackPointer);

    masm.ma_b(output, Imm32(INT32_MIN), &bail, Assembler::Equal);

    masm.convertInt32ToFloat32(output, ScratchFloat32Reg);
    Label done;
    masm.branchFloat(Assembler::DoubleGreaterThanOrEqual, ScratchFloat32Reg, input, &done);
    masm.add32(Imm32(1), output);

    masm.bind(&done);
    bailoutFrom(&bail, lir->snapshot());
}

void
CodeGeneratorPPC::visitRound(LRound* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());
    FloatRegister temp = ToFloatRegister(lir->temp());

    Label bail;

    masm.branchDouble(Assembler::DoubleUnordered, input, input, &bail);

    // round(x) = floor(x + 0.5), but with special handling for the range (-0.5, 0).
    Label negative, doFloor;
    masm.loadConstantDouble(0.0, ScratchDoubleReg);
    masm.branchDouble(Assembler::DoubleLessThan, input, ScratchDoubleReg, &negative);

    // Positive: add 0.5 and truncate.
    masm.loadConstantDouble(0.5, temp);
    masm.as_fadd(ScratchDoubleReg, input, temp);
    masm.as_fctiwz(ScratchDoubleReg, ScratchDoubleReg);
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(ScratchDoubleReg, StackPointer, 0);
    masm.as_lwz(output, StackPointer, 4);
    masm.addPtr(Imm32(8), StackPointer);
    masm.ma_b(output, Imm32(INT32_MIN), &bail, Assembler::Equal);
    Label done;
    masm.jump(&done);

    masm.bind(&negative);
    // For negative numbers, check if input >= -0.5 (result should be -0).
    masm.loadConstantDouble(-0.5, ScratchDoubleReg);
    masm.branchDouble(Assembler::DoubleGreaterThanOrEqual, input, ScratchDoubleReg, &bail);
    // Less than -0.5: add 0.5 and use floor.
    masm.loadConstantDouble(0.5, temp);
    masm.as_fadd(ScratchDoubleReg, input, temp);
    masm.as_fctiwz(ScratchDoubleReg, ScratchDoubleReg);
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(ScratchDoubleReg, StackPointer, 0);
    masm.as_lwz(output, StackPointer, 4);
    masm.addPtr(Imm32(8), StackPointer);
    masm.ma_b(output, Imm32(INT32_MIN), &bail, Assembler::Equal);

    // Floor adjustment for negative: check if trunc > input+0.5.
    masm.convertInt32ToDouble(output, temp);
    masm.loadConstantDouble(0.5, ScratchDoubleReg);
    masm.as_fadd(ScratchDoubleReg, input, ScratchDoubleReg);
    masm.branchDouble(Assembler::DoubleLessThanOrEqual, temp, ScratchDoubleReg, &done);
    masm.sub32(Imm32(1), output);

    masm.bind(&done);
    bailoutFrom(&bail, lir->snapshot());
}

void
CodeGeneratorPPC::visitRoundF(LRoundF* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());
    FloatRegister temp = ToFloatRegister(lir->temp());

    Label bail;
    masm.branchFloat(Assembler::DoubleUnordered, input, input, &bail);

    // Convert to double, round via same logic.
    masm.convertFloat32ToDouble(input, ScratchDoubleReg);

    Label negative, done;
    FloatRegister zero = temp;
    masm.loadConstantDouble(0.0, zero);
    masm.branchDouble(Assembler::DoubleLessThan, ScratchDoubleReg, zero, &negative);

    // Positive.
    masm.loadConstantDouble(0.5, zero);
    masm.as_fadd(ScratchDoubleReg, ScratchDoubleReg, zero);
    masm.as_fctiwz(ScratchDoubleReg, ScratchDoubleReg);
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(ScratchDoubleReg, StackPointer, 0);
    masm.as_lwz(output, StackPointer, 4);
    masm.addPtr(Imm32(8), StackPointer);
    masm.ma_b(output, Imm32(INT32_MIN), &bail, Assembler::Equal);
    masm.jump(&done);

    masm.bind(&negative);
    masm.loadConstantDouble(-0.5, zero);
    masm.branchDouble(Assembler::DoubleGreaterThanOrEqual, ScratchDoubleReg, zero, &bail);
    masm.loadConstantDouble(0.5, zero);
    masm.as_fadd(ScratchDoubleReg, ScratchDoubleReg, zero);
    masm.as_fctiwz(ScratchDoubleReg, ScratchDoubleReg);
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(ScratchDoubleReg, StackPointer, 0);
    masm.as_lwz(output, StackPointer, 4);
    masm.addPtr(Imm32(8), StackPointer);
    masm.ma_b(output, Imm32(INT32_MIN), &bail, Assembler::Equal);

    // Floor adjustment.
    masm.convertInt32ToDouble(output, zero);
    masm.branchDouble(Assembler::DoubleLessThanOrEqual, zero, ScratchDoubleReg, &done);
    masm.sub32(Imm32(1), output);

    masm.bind(&done);
    bailoutFrom(&bail, lir->snapshot());
}

// ========================================================================
// Truncation

void
CodeGeneratorPPC::visitTruncateDToInt32(LTruncateDToInt32* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());
    Label bail;
    masm.branchTruncateDoubleToInt32(input, output, &bail);
    bailoutFrom(&bail, ins->snapshot());
}

void
CodeGeneratorPPC::visitTruncateFToInt32(LTruncateFToInt32* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());
    Label bail;
    masm.branchTruncateFloat32ToInt32(input, output, &bail);
    bailoutFrom(&bail, ins->snapshot());
}

// ========================================================================
// CopySign

void
CodeGeneratorPPC::visitCopySignD(LCopySignD* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->getOperand(0));
    FloatRegister rhs = ToFloatRegister(ins->getOperand(1));
    FloatRegister output = ToFloatRegister(ins->getDef(0));

    // PPC: use fabs to clear sign, then fneg conditionally.
    masm.as_fabs(output, lhs);
    // Check sign of rhs: store to stack, check high bit.
    Label done;
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(rhs, StackPointer, 0);
    masm.as_lwz(ScratchRegister, StackPointer, 0); // high word
    masm.addPtr(Imm32(8), StackPointer);
    masm.ma_b(ScratchRegister, Imm32(0), &done, Assembler::NotSigned);
    // rhs is negative, negate the output.
    masm.as_fneg(output, output);
    masm.bind(&done);
}

void
CodeGeneratorPPC::visitCopySignF(LCopySignF* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->getOperand(0));
    FloatRegister rhs = ToFloatRegister(ins->getOperand(1));
    FloatRegister output = ToFloatRegister(ins->getDef(0));

    masm.as_fabs(output, lhs);
    masm.subPtr(Imm32(8), StackPointer);
    masm.as_stfd(rhs, StackPointer, 0);
    masm.as_lwz(ScratchRegister, StackPointer, 0);
    masm.addPtr(Imm32(8), StackPointer);
    Label done;
    masm.ma_b(ScratchRegister, Imm32(0), &done, Assembler::NotSigned);
    masm.as_fneg(output, output);
    masm.bind(&done);
}

// ========================================================================
// Value/Double/Float32 loading

void
CodeGeneratorPPC::visitValue(LValue* value)
{
    const ValueOperand out = ToOutValue(value);
    masm.moveValue(value->value(), out);
}

void
CodeGeneratorPPC::visitDouble(LDouble* ins)
{
    FloatRegister output = ToFloatRegister(ins->getDef(0));
    masm.loadConstantDouble(ins->getDouble(), output);
}

void
CodeGeneratorPPC::visitFloat32(LFloat32* ins)
{
    FloatRegister output = ToFloatRegister(ins->getDef(0));
    masm.loadConstantFloat32(ins->getFloat(), output);
}

// ========================================================================
// Float test and branch

void
CodeGeneratorPPC::visitTestDAndBranch(LTestDAndBranch* test)
{
    FloatRegister input = ToFloatRegister(test->input());
    masm.loadConstantDouble(0.0, ScratchDoubleReg);
    masm.branchDouble(Assembler::DoubleEqual, input, ScratchDoubleReg, test->ifFalse()->lir()->label());
    masm.branchDouble(Assembler::DoubleUnordered, input, input, test->ifFalse()->lir()->label());
    masm.jump(test->ifTrue()->lir()->label());
}

void
CodeGeneratorPPC::visitTestFAndBranch(LTestFAndBranch* test)
{
    FloatRegister input = ToFloatRegister(test->input());
    masm.loadConstantFloat32(0.0f, ScratchFloat32Reg);
    masm.branchFloat(Assembler::DoubleEqual, input, ScratchFloat32Reg, test->ifFalse()->lir()->label());
    masm.branchFloat(Assembler::DoubleUnordered, input, input, test->ifFalse()->lir()->label());
    masm.jump(test->ifTrue()->lir()->label());
}

// ========================================================================
// Float compare

void
CodeGeneratorPPC::visitCompareD(LCompareD* comp)
{
    FloatRegister lhs = ToFloatRegister(comp->left());
    FloatRegister rhs = ToFloatRegister(comp->right());
    Register output = ToRegister(comp->output());
    Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());

    // Set output to 1, branch over the 0 if condition is true.
    Label isTrue, done;
    masm.move32(Imm32(0), output);
    masm.branchDouble(cond, lhs, rhs, &isTrue);
    masm.jump(&done);
    masm.bind(&isTrue);
    masm.move32(Imm32(1), output);
    masm.bind(&done);
}

void
CodeGeneratorPPC::visitCompareF(LCompareF* comp)
{
    FloatRegister lhs = ToFloatRegister(comp->left());
    FloatRegister rhs = ToFloatRegister(comp->right());
    Register output = ToRegister(comp->output());
    Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());

    Label isTrue, done;
    masm.move32(Imm32(0), output);
    masm.branchFloat(cond, lhs, rhs, &isTrue);
    masm.jump(&done);
    masm.bind(&isTrue);
    masm.move32(Imm32(1), output);
    masm.bind(&done);
}

void
CodeGeneratorPPC::visitCompareDAndBranch(LCompareDAndBranch* comp)
{
    FloatRegister lhs = ToFloatRegister(comp->left());
    FloatRegister rhs = ToFloatRegister(comp->right());
    Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->cmpMir()->jsop());
    MBasicBlock* ifFalse = skipTrivialBlocks(comp->ifFalse());

    masm.branchDouble(cond, lhs, rhs, comp->ifTrue()->lir()->label());
    masm.jump(ifFalse->lir()->label());
}

void
CodeGeneratorPPC::visitCompareFAndBranch(LCompareFAndBranch* comp)
{
    FloatRegister lhs = ToFloatRegister(comp->left());
    FloatRegister rhs = ToFloatRegister(comp->right());
    Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->cmpMir()->jsop());
    MBasicBlock* ifFalse = skipTrivialBlocks(comp->ifFalse());

    masm.branchFloat(cond, lhs, rhs, comp->ifTrue()->lir()->label());
    masm.jump(ifFalse->lir()->label());
}

void
CodeGeneratorPPC::visitBitAndAndBranch(LBitAndAndBranch* baab)
{
    if (baab->right()->isConstant())
        masm.ma_and(ScratchRegister, ToRegister(baab->left()), Imm32(ToInt32(baab->right())));
    else
        masm.as_and(ScratchRegister, ToRegister(baab->left()), ToRegister(baab->right()));

    emitBranch(ScratchRegister, Imm32(0), Assembler::NonZero, baab->ifTrue(), baab->ifFalse());
}

// ========================================================================
// Not operations

void
CodeGeneratorPPC::visitNotI(LNotI* ins)
{
    Register input = ToRegister(ins->input());
    Register output = ToRegister(ins->output());

    masm.cmp32Set(Assembler::Equal, input, Imm32(0), output);
}

void
CodeGeneratorPPC::visitNotD(LNotD* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());

    // !x is true if x == 0 or x is NaN.
    Label isTrue, done;
    masm.move32(Imm32(0), output);
    masm.loadConstantDouble(0.0, ScratchDoubleReg);
    masm.branchDouble(Assembler::DoubleEqualOrUnordered, input, ScratchDoubleReg, &isTrue);
    masm.jump(&done);
    masm.bind(&isTrue);
    masm.move32(Imm32(1), output);
    masm.bind(&done);
}

void
CodeGeneratorPPC::visitNotF(LNotF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());

    Label isTrue, done;
    masm.move32(Imm32(0), output);
    masm.loadConstantFloat32(0.0f, ScratchFloat32Reg);
    masm.branchFloat(Assembler::DoubleEqualOrUnordered, input, ScratchFloat32Reg, &isTrue);
    masm.jump(&done);
    masm.bind(&isTrue);
    masm.move32(Imm32(1), output);
    masm.bind(&done);
}

// ========================================================================
// Negation

void
CodeGeneratorPPC::visitNegI(LNegI* ins)
{
    Register input = ToRegister(ins->input());
    Register output = ToRegister(ins->output());
    masm.ma_negu(output, input);
}

void
CodeGeneratorPPC::visitNegD(LNegD* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());
    masm.as_fneg(output, input);
}

void
CodeGeneratorPPC::visitNegF(LNegF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());
    masm.as_fneg(output, input);
}

// ========================================================================
// Guards

void
CodeGeneratorPPC::visitGuardShape(LGuardShape* guard)
{
    Register obj = ToRegister(guard->input());
    Register tmp = ToRegister(guard->tempInt());
    MOZ_ASSERT(obj != tmp);

    masm.loadPtr(Address(obj, ShapedObject::offsetOfShape()), tmp);
    bailoutCmpPtr(Assembler::NotEqual, tmp, ImmGCPtr(guard->mir()->shape()), guard->snapshot());
}

void
CodeGeneratorPPC::visitGuardObjectGroup(LGuardObjectGroup* guard)
{
    Register obj = ToRegister(guard->input());
    Register tmp = ToRegister(guard->tempInt());
    MOZ_ASSERT(obj != tmp);

    masm.loadPtr(Address(obj, JSObject::offsetOfGroup()), tmp);
    Assembler::Condition cond = guard->mir()->bailOnEquality()
                                ? Assembler::Equal
                                : Assembler::NotEqual;
    bailoutCmpPtr(cond, tmp, ImmGCPtr(guard->mir()->group()), guard->snapshot());
}

void
CodeGeneratorPPC::visitGuardClass(LGuardClass* guard)
{
    Register obj = ToRegister(guard->input());
    Register tmp = ToRegister(guard->tempInt());

    masm.loadObjClass(obj, tmp);
    bailoutCmpPtr(Assembler::NotEqual, tmp, ImmPtr(guard->mir()->getClass()),
                  guard->snapshot());
}

// ========================================================================
// Memory barrier

void
CodeGeneratorPPC::visitMemoryBarrier(LMemoryBarrier* ins)
{
    masm.memoryBarrier(ins->type());
}

// ========================================================================
// Effective address

void
CodeGeneratorPPC::visitEffectiveAddress(LEffectiveAddress* ins)
{
    const MEffectiveAddress* mir = ins->mir();
    Register base = ToRegister(ins->base());
    Register index = ToRegister(ins->index());
    Register output = ToRegister(ins->output());

    // output = base + index * scale + displacement
    masm.ma_sll(ScratchRegister, index, Imm32(mir->scale()));
    masm.as_add(output, base, ScratchRegister);
    if (mir->displacement())
        masm.ma_addu(output, output, Imm32(mir->displacement()));
}

// ========================================================================
// Unsigned div/mod

void
CodeGeneratorPPC::visitUDivOrMod(LUDivOrMod* ins)
{
    Register lhs = ToRegister(ins->lhs());
    Register rhs = ToRegister(ins->rhs());
    Register output = ToRegister(ins->output());
    Label done;

    // Prevent divide by zero.
    if (ins->canBeDivideByZero()) {
        if (ins->mir()->isTruncated()) {
            if (ins->trapOnError()) {
                masm.ma_b(rhs, Imm32(0), trap(ins, wasm::Trap::IntegerDivideByZero),
                          Assembler::Equal);
            } else {
                Label notzero;
                masm.ma_b(rhs, Imm32(0), &notzero, Assembler::NotEqual);
                masm.move32(Imm32(0), output);
                masm.jump(&done);
                masm.bind(&notzero);
            }
        } else {
            bailoutCmp32(Assembler::Equal, rhs, Imm32(0), ins->snapshot());
        }
    }

    // Unsigned division.
    masm.as_divwu(output, lhs, rhs);

    if (ins->mir()->isDiv()) {
        // Check remainder if not truncated.
        if (!ins->mir()->toDiv()->canTruncateRemainder()) {
            // remainder = lhs - output * rhs
            masm.as_mullw(ScratchRegister, output, rhs);
            bailoutCmp32(Assembler::NotEqual, ScratchRegister, lhs, ins->snapshot());
        }
    } else {
        // Mod: output = lhs - (lhs / rhs) * rhs
        masm.as_mullw(ScratchRegister, output, rhs);
        masm.as_subf(output, ScratchRegister, lhs);
    }

    if (!ins->mir()->isTruncated())
        bailoutCmp32(Assembler::LessThan, output, Imm32(0), ins->snapshot());

    masm.bind(&done);
}

// ========================================================================
// Int64 stubs (not needed for baseline JIT)

void CodeGeneratorPPC::visitAddI64(LAddI64* ins) { MOZ_CRASH("PPC: NYI visitAddI64"); }
void CodeGeneratorPPC::visitSubI64(LSubI64* ins) { MOZ_CRASH("PPC: NYI visitSubI64"); }
void CodeGeneratorPPC::visitMulI64(LMulI64* ins) { MOZ_CRASH("PPC: NYI visitMulI64"); }
void CodeGeneratorPPC::visitBitOpI64(LBitOpI64* ins) { MOZ_CRASH("PPC: NYI visitBitOpI64"); }
void CodeGeneratorPPC::visitShiftI64(LShiftI64* ins) { MOZ_CRASH("PPC: NYI visitShiftI64"); }
void CodeGeneratorPPC::visitRotateI64(LRotateI64* ins) { MOZ_CRASH("PPC: NYI visitRotateI64"); }
void CodeGeneratorPPC::visitPopcntI64(LPopcntI64* ins) { MOZ_CRASH("PPC: NYI visitPopcntI64"); }

// ========================================================================
// Wasm stubs (not needed for baseline JIT)

void CodeGeneratorPPC::visitWasmUint32ToDouble(LWasmUint32ToDouble* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmUint32ToFloat32(LWasmUint32ToFloat32* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmTruncateToInt32(LWasmTruncateToInt32* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitOutOfLineWasmTruncateCheck(OutOfLineWasmTruncateCheck* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmLoadGlobalVar(LWasmLoadGlobalVar* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmStoreGlobalVar(LWasmStoreGlobalVar* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmCall(LWasmCall* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmCallI64(LWasmCallI64* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmLoad(LWasmLoad* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmStore(LWasmStore* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmAddOffset(LWasmAddOffset* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmStackArg(LWasmStackArg* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmStackArgI64(LWasmStackArgI64* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmSelect(LWasmSelect* ins) { MOZ_CRASH("PPC wasm NYI"); }
void CodeGeneratorPPC::visitWasmReinterpret(LWasmReinterpret* ins) { MOZ_CRASH("PPC wasm NYI"); }

// ========================================================================
// AsmJS stubs (not needed for baseline JIT)

void CodeGeneratorPPC::visitAsmJSLoadHeap(LAsmJSLoadHeap* ins) { MOZ_CRASH("PPC asm.js NYI"); }
void CodeGeneratorPPC::visitAsmJSStoreHeap(LAsmJSStoreHeap* ins) { MOZ_CRASH("PPC asm.js NYI"); }
void CodeGeneratorPPC::visitAsmJSCompareExchangeHeap(LAsmJSCompareExchangeHeap* ins) { MOZ_CRASH("PPC asm.js NYI"); }
void CodeGeneratorPPC::visitAsmJSAtomicExchangeHeap(LAsmJSAtomicExchangeHeap* ins) { MOZ_CRASH("PPC asm.js NYI"); }
void CodeGeneratorPPC::visitAsmJSAtomicBinopHeap(LAsmJSAtomicBinopHeap* ins) { MOZ_CRASH("PPC asm.js NYI"); }
void CodeGeneratorPPC::visitAsmJSAtomicBinopHeapForEffect(LAsmJSAtomicBinopHeapForEffect* ins) { MOZ_CRASH("PPC asm.js NYI"); }

// ========================================================================
// Typed array stubs

void CodeGeneratorPPC::visitLoadTypedArrayElementStatic(LLoadTypedArrayElementStatic* ins) { MOZ_CRASH("NYI"); }
void CodeGeneratorPPC::visitStoreTypedArrayElementStatic(LStoreTypedArrayElementStatic* ins) { MOZ_CRASH("NYI"); }

// ========================================================================
// Atomic stubs (not needed for baseline JIT)

void CodeGeneratorPPC::visitAtomicTypedArrayElementBinop(LAtomicTypedArrayElementBinop* ins) { MOZ_CRASH("PPC atomics NYI"); }
void CodeGeneratorPPC::visitAtomicTypedArrayElementBinopForEffect(LAtomicTypedArrayElementBinopForEffect* ins) { MOZ_CRASH("PPC atomics NYI"); }
void CodeGeneratorPPC::visitCompareExchangeTypedArrayElement(LCompareExchangeTypedArrayElement* ins) { MOZ_CRASH("PPC atomics NYI"); }
void CodeGeneratorPPC::visitAtomicExchangeTypedArrayElement(LAtomicExchangeTypedArrayElement* ins) { MOZ_CRASH("PPC atomics NYI"); }
