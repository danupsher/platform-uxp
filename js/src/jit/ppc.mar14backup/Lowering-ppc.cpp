/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#include "jit/ppc/Lowering-ppc.h"
#include "mozilla/MathAlgorithms.h"
#include "jit/shared/Lowering-shared-inl.h"
#include "jit/ppc/Assembler-ppc.h"
#include "jit/MIR.h"
#include "jit/ppc/LIR-ppc.h"
using namespace js;
using namespace js::jit;
using mozilla::FloorLog2;
LBoxAllocation
LIRGeneratorPPC::useBoxFixed(MDefinition* mir, Register reg1, Register reg2, bool useAtStart)
{
    MOZ_ASSERT(mir->type() == MIRType::Value);
    MOZ_ASSERT(reg1 != reg2);

    ensureDefined(mir);
    return LBoxAllocation(LUse(reg1, mir->virtualRegister(), useAtStart),
                          LUse(reg2, VirtualRegisterOfPayload(mir), useAtStart));
}
void LIRGeneratorPPC::visitBox(MBox* box) {
    MDefinition* inner = box->getOperand(0);
    if (IsFloatingPointType(inner->type())) {
        LBoxFloatingPoint* lir = new(alloc()) LBoxFloatingPoint(
            useRegister(inner), tempCopy(inner, 0), inner->type());
        defineBox(lir, box);
    } else {
        // Non-FP: use LBox (nunbox32 two-register boxing)
        LBox* lir = new(alloc()) LBox(use(inner), inner->type());
        defineBox(lir, box);
    }
}
void LIRGeneratorPPC::visitUnbox(MUnbox* unbox) {
    MDefinition* inner = unbox->getOperand(0);

    MOZ_ASSERT(inner->type() == MIRType::Value);

    ensureDefined(inner);

    if (IsFloatingPointType(unbox->type())) {
        LUnboxFloatingPoint* lir = new(alloc()) LUnboxFloatingPoint(useBox(inner), unbox->type());
        if (unbox->fallible())
            assignSnapshot(lir, unbox->bailoutKind());
        define(lir, unbox);
        return;
    }

    // Swap the order we use the box pieces so we can re-use the payload register.
    LUnbox* lir = new(alloc()) LUnbox;
    lir->setOperand(0, usePayloadInRegisterAtStart(inner));
    lir->setOperand(1, useType(inner, LUse::REGISTER));

    if (unbox->fallible())
        assignSnapshot(lir, unbox->bailoutKind());

    defineReuseInput(lir, unbox, 0);
}
void LIRGeneratorPPC::visitReturn(MReturn* ret) {
    MDefinition* opd = ret->getOperand(0);
    MOZ_ASSERT(opd->type() == MIRType::Value);
    LReturn* ins = new(alloc()) LReturn;
    ins->setOperand(0, LUse(JSReturnReg_Type, opd->virtualRegister()));
    ins->setOperand(1, LUse(JSReturnReg_Data, VirtualRegisterOfPayload(opd)));
    add(ins);
}
void LIRGeneratorPPC::visitRandom(MRandom* ins) {
    LRandom* lir = new(alloc()) LRandom(temp(), temp(), temp(), temp(),
                                         temp(), temp(), temp());
    defineFixed(lir, ins, LFloatReg(ReturnDoubleReg));
}
void LIRGeneratorPPC::lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex) {
    // NUNBOX32: each Value phi has two LIR phis (type + payload)
    MDefinition* operand = phi->getOperand(inputPosition);
    LPhi* type = block->getPhi(lirIndex + VREG_TYPE_OFFSET);
    LPhi* payload = block->getPhi(lirIndex + VREG_DATA_OFFSET);
    type->setOperand(inputPosition, LUse(operand->virtualRegister() + VREG_TYPE_OFFSET, LUse::ANY));
    payload->setOperand(inputPosition, LUse(VirtualRegisterOfPayload(operand), LUse::ANY));
}
void LIRGeneratorPPC::defineUntypedPhi(MPhi* phi, size_t lirIndex) {
    LPhi* type = current->getPhi(lirIndex + VREG_TYPE_OFFSET);
    LPhi* payload = current->getPhi(lirIndex + VREG_DATA_OFFSET);

    uint32_t typeVreg = getVirtualRegister();
    phi->setVirtualRegister(typeVreg);

    uint32_t payloadVreg = getVirtualRegister();
    MOZ_ASSERT(typeVreg + 1 == payloadVreg);

    type->setDef(0, LDefinition(typeVreg, LDefinition::TYPE));
    payload->setDef(0, LDefinition(payloadVreg, LDefinition::PAYLOAD));
    annotate(type);
    annotate(payload);
}
void LIRGeneratorPPC::lowerInt64PhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex) {
    MOZ_CRASH("Int64 not supported on PPC32");
}
void LIRGeneratorPPC::defineInt64Phi(MPhi* phi, size_t lirIndex) {
    MOZ_CRASH("Int64 not supported on PPC32");
}
void LIRGeneratorPPC::lowerTruncateDToInt32(MTruncateToInt32* ins) {
    MDefinition* opd = ins->input();
    define(new(alloc()) LTruncateDToInt32(useRegister(opd), LDefinition::BogusTemp()), ins);
}
void LIRGeneratorPPC::lowerTruncateFToInt32(MTruncateToInt32* ins) {
    MDefinition* opd = ins->input();
    define(new(alloc()) LTruncateFToInt32(useRegister(opd), LDefinition::BogusTemp()), ins);
}


LAllocation
LIRGeneratorPPC::useByteOpRegister(MDefinition* mir)
{
    return useRegister(mir);
}

LAllocation
LIRGeneratorPPC::useByteOpRegisterAtStart(MDefinition* mir)
{
    return useRegisterAtStart(mir);
}

LAllocation
LIRGeneratorPPC::useByteOpRegisterOrNonDoubleConstant(MDefinition* mir)
{
    return useRegisterOrNonDoubleConstant(mir);
}

LDefinition
LIRGeneratorPPC::tempByteOpRegister()
{
    return temp();
}

void
LIRGeneratorPPC::lowerForShift(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                                MDefinition* lhs, MDefinition* rhs)
{
    ins->setOperand(0, useRegister(lhs));
    ins->setOperand(1, useRegisterOrConstant(rhs));
    define(ins, mir);
}

void
LIRGeneratorPPC::lowerUrshD(MUrsh* mir)
{
    MDefinition* lhs = mir->lhs();
    MDefinition* rhs = mir->rhs();
    MOZ_ASSERT(lhs->type() == MIRType::Int32);
    MOZ_ASSERT(rhs->type() == MIRType::Int32);
    LUrshD* lir = new(alloc()) LUrshD(useRegister(lhs), useRegisterOrConstant(rhs), temp());
    define(lir, mir);
}

void
LIRGeneratorPPC::lowerForALU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir,
                              MDefinition* input)
{
    ins->setOperand(0, useRegister(input));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

void
LIRGeneratorPPC::lowerForALU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                              MDefinition* lhs, MDefinition* rhs)
{
    ins->setOperand(0, useRegister(lhs));
    ins->setOperand(1, useRegisterOrConstant(rhs));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

void
LIRGeneratorPPC::lowerForALUInt64(LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* ins,
                                   MDefinition* mir, MDefinition* lhs, MDefinition* rhs)
{
    MOZ_CRASH("PPC: NYI lowerForALUInt64");
}

void
LIRGeneratorPPC::lowerForMulInt64(LMulI64* ins, MMul* mir, MDefinition* lhs, MDefinition* rhs)
{
    MOZ_CRASH("PPC: NYI lowerForMulInt64");
}

void
LIRGeneratorPPC::lowerForFPU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir,
                              MDefinition* src)
{
    ins->setOperand(0, useRegister(src));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template<size_t Temps>
void
LIRGeneratorPPC::lowerForFPU(LInstructionHelper<1, 2, Temps>* ins, MDefinition* mir,
                              MDefinition* lhs, MDefinition* rhs)
{
    ins->setOperand(0, useRegister(lhs));
    ins->setOperand(1, useRegister(rhs));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template void LIRGeneratorPPC::lowerForFPU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                                            MDefinition* lhs, MDefinition* rhs);
template void LIRGeneratorPPC::lowerForFPU(LInstructionHelper<1, 2, 1>* ins, MDefinition* mir,
                                            MDefinition* lhs, MDefinition* rhs);

template<size_t Temps>
void
LIRGeneratorPPC::lowerForShiftInt64(LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, Temps>* ins,
                                     MDefinition* mir, MDefinition* lhs, MDefinition* rhs)
{
    MOZ_CRASH("PPC: NYI lowerForShiftInt64");
}

template void LIRGeneratorPPC::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES+1, 0>* ins, MDefinition* mir,
    MDefinition* lhs, MDefinition* rhs);
template void LIRGeneratorPPC::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES+1, 1>* ins, MDefinition* mir,
    MDefinition* lhs, MDefinition* rhs);

void
LIRGeneratorPPC::lowerForBitAndAndBranch(LBitAndAndBranch* baab, MInstruction* mir,
                                          MDefinition* lhs, MDefinition* rhs)
{
    baab->setOperand(0, useRegisterAtStart(lhs));
    baab->setOperand(1, useRegisterOrConstantAtStart(rhs));
    add(baab, mir);
}

void
LIRGeneratorPPC::lowerDivI(MDiv* div)
{
    if (div->isUnsigned()) {
        lowerUDiv(div);
        return;
    }

    if (div->rhs()->isConstant()) {
        int32_t rhs = div->rhs()->toConstant()->toInt32();
        int32_t shift = FloorLog2(rhs);
        if (rhs > 0 && 1 << shift == rhs) {
            LDivPowTwoI* lir = new(alloc()) LDivPowTwoI(useRegister(div->lhs()), shift, temp());
            if (div->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            define(lir, div);
            return;
        }
    }

    LDivI* lir = new(alloc()) LDivI(useRegister(div->lhs()), useRegister(div->rhs()), temp());
    if (div->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    define(lir, div);
}

void
LIRGeneratorPPC::lowerModI(MMod* mod)
{
    if (mod->isUnsigned()) {
        lowerUMod(mod);
        return;
    }

    if (mod->rhs()->isConstant()) {
        int32_t rhs = mod->rhs()->toConstant()->toInt32();
        int32_t shift = FloorLog2(rhs);
        if (rhs > 0 && 1 << shift == rhs) {
            LModPowTwoI* lir = new(alloc()) LModPowTwoI(useRegister(mod->lhs()), shift);
            if (mod->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            define(lir, mod);
            return;
        } else if (shift < 31 && (1 << (shift + 1)) - 1 == rhs) {
            LModMaskI* lir = new(alloc()) LModMaskI(useRegister(mod->lhs()),
                                                    temp(LDefinition::GENERAL),
                                                    temp(LDefinition::GENERAL),
                                                    shift + 1);
            if (mod->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            define(lir, mod);
            return;
        }
    }

    LModI* lir = new(alloc()) LModI(useRegister(mod->lhs()), useRegister(mod->rhs()),
                                    temp(LDefinition::GENERAL));
    if (mod->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    define(lir, mod);
}

void
LIRGeneratorPPC::lowerMulI(MMul* mul, MDefinition* lhs, MDefinition* rhs)
{
    LMulI* lir = new(alloc()) LMulI;
    if (mul->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    lowerForALU(lir, mul, lhs, rhs);
}

void
LIRGeneratorPPC::lowerUDiv(MDiv* div)
{
    MDefinition* lhs = div->getOperand(0);
    MDefinition* rhs = div->getOperand(1);
    LUDivOrMod* lir = new(alloc()) LUDivOrMod;
    lir->setOperand(0, useRegister(lhs));
    lir->setOperand(1, useRegister(rhs));
    if (div->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    define(lir, div);
}

void
LIRGeneratorPPC::lowerUMod(MMod* mod)
{
    MDefinition* lhs = mod->getOperand(0);
    MDefinition* rhs = mod->getOperand(1);
    LUDivOrMod* lir = new(alloc()) LUDivOrMod;
    lir->setOperand(0, useRegister(lhs));
    lir->setOperand(1, useRegister(rhs));
    if (mod->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    define(lir, mod);
}

LTableSwitch*
LIRGeneratorPPC::newLTableSwitch(const LAllocation& in, const LDefinition& inputCopy,
                                  MTableSwitch* ins)
{
    return new(alloc()) LTableSwitch(in, inputCopy, temp(), ins);
}

LTableSwitchV*
LIRGeneratorPPC::newLTableSwitchV(MTableSwitch* ins)
{
    return new(alloc()) LTableSwitchV(useBox(ins->getOperand(0)),
                                      temp(), tempDouble(), temp(), ins);
}

void
LIRGeneratorPPC::lowerPhi(MPhi* phi)
{
    if (phi->type() == MIRType::Value) {
        defineUntypedPhi(phi, phi->id());
    } else if (phi->type() == MIRType::Int64) {
        defineInt64Phi(phi, phi->id());
    } else {
        MOZ_ASSERT(phi->type() != MIRType::None);
        LPhi* lphi = current->getPhi(phi->id());
        lphi->setDef(0, LDefinition(phi->virtualRegister(), LDefinition::TypeFrom(phi->type())));
        for (size_t i = 0, e = phi->numOperands(); i < e; i++)
            lphi->setOperand(i, LUse(phi->getOperand(i)->virtualRegister(), LUse::ANY));
    }
}

void
LIRGeneratorPPC::visitPowHalf(MPowHalf* ins)
{
    MDefinition* input = ins->input();
    MOZ_ASSERT(input->type() == MIRType::Double);
    LPowHalfD* lir = new(alloc()) LPowHalfD(useRegisterAtStart(input));
    defineReuseInput(lir, ins, 0);
}

void
LIRGeneratorPPC::visitAsmJSNeg(MAsmJSNeg* ins)
{
    MOZ_CRASH("PPC: NYI visitAsmJSNeg");
}


void
LIRGeneratorPPC::visitGuardShape(MGuardShape* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    LDefinition tempObj = temp(LDefinition::OBJECT);
    LGuardShape* guard = new(alloc()) LGuardShape(useRegister(ins->object()), tempObj);
    assignSnapshot(guard, ins->bailoutKind());
    add(guard, ins);
    redefine(ins, ins->object());
}

void
LIRGeneratorPPC::visitGuardObjectGroup(MGuardObjectGroup* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    LDefinition tempObj = temp(LDefinition::OBJECT);
    LGuardObjectGroup* guard = new(alloc()) LGuardObjectGroup(useRegister(ins->object()), tempObj);
    assignSnapshot(guard, ins->bailoutKind());
    add(guard, ins);
    redefine(ins, ins->object());
}

void
LIRGeneratorPPC::visitWasmUnsignedToDouble(MWasmUnsignedToDouble* ins)
{
    MOZ_CRASH("PPC: NYI visitWasmUnsignedToDouble");
}

void
LIRGeneratorPPC::visitWasmUnsignedToFloat32(MWasmUnsignedToFloat32* ins)
{
    MOZ_CRASH("PPC: NYI visitWasmUnsignedToFloat32");
}

void
LIRGeneratorPPC::visitAsmJSLoadHeap(MAsmJSLoadHeap* ins)
{
    MOZ_CRASH("PPC: NYI visitAsmJSLoadHeap");
}

void
LIRGeneratorPPC::visitAsmJSStoreHeap(MAsmJSStoreHeap* ins)
{
    MOZ_CRASH("PPC: NYI visitAsmJSStoreHeap");
}

void
LIRGeneratorPPC::visitAsmJSCompareExchangeHeap(MAsmJSCompareExchangeHeap* ins)
{
    MOZ_CRASH("PPC: NYI visitAsmJSCompareExchangeHeap");
}

void
LIRGeneratorPPC::visitAsmJSAtomicExchangeHeap(MAsmJSAtomicExchangeHeap* ins)
{
    MOZ_CRASH("PPC: NYI visitAsmJSAtomicExchangeHeap");
}

void
LIRGeneratorPPC::visitAsmJSAtomicBinopHeap(MAsmJSAtomicBinopHeap* ins)
{
    MOZ_CRASH("PPC: NYI visitAsmJSAtomicBinopHeap");
}

void
LIRGeneratorPPC::visitStoreTypedArrayElementStatic(MStoreTypedArrayElementStatic* ins)
{
    MOZ_CRASH("PPC: NYI visitStoreTypedArrayElementStatic");
}

void
LIRGeneratorPPC::visitCompareExchangeTypedArrayElement(MCompareExchangeTypedArrayElement* ins)
{
    MOZ_CRASH("PPC: NYI visitCompareExchangeTypedArrayElement");
}

void
LIRGeneratorPPC::visitAtomicExchangeTypedArrayElement(MAtomicExchangeTypedArrayElement* ins)
{
    MOZ_CRASH("PPC: NYI visitAtomicExchangeTypedArrayElement");
}

void
LIRGeneratorPPC::visitAtomicTypedArrayElementBinop(MAtomicTypedArrayElementBinop* ins)
{
    MOZ_CRASH("PPC: NYI visitAtomicTypedArrayElementBinop");
}

void
LIRGeneratorPPC::visitSubstr(MSubstr* ins)
{
    LSubstr* lir = new (alloc()) LSubstr(useRegister(ins->string()),
                                         useRegister(ins->begin()),
                                         useRegister(ins->length()),
                                         temp(),
                                         temp(),
                                         tempByteOpRegister());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGeneratorPPC::visitWasmTruncateToInt64(MWasmTruncateToInt64* ins)
{
    MOZ_CRASH("PPC: NYI visitWasmTruncateToInt64");
}

void
LIRGeneratorPPC::visitInt64ToFloatingPoint(MInt64ToFloatingPoint* ins)
{
    MOZ_CRASH("PPC: NYI visitInt64ToFloatingPoint");
}

void
LIRGeneratorPPC::visitCopySign(MCopySign* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();
    MOZ_ASSERT(IsFloatingPointType(lhs->type()));
    MOZ_ASSERT(lhs->type() == rhs->type());
    MOZ_ASSERT(lhs->type() == ins->type());

    LInstructionHelper<1, 2, 2>* lir;
    if (lhs->type() == MIRType::Double)
        lir = new(alloc()) LCopySignD();
    else
        lir = new(alloc()) LCopySignF();

    lir->setOperand(0, useRegister(lhs));
    lir->setOperand(1, useRegister(rhs));
    lir->setTemp(0, temp());
    lir->setTemp(1, temp());
    defineReuseInput(lir, ins, 0);
}

void
LIRGeneratorPPC::visitExtendInt32ToInt64(MExtendInt32ToInt64* ins)
{
    MOZ_CRASH("PPC: NYI visitExtendInt32ToInt64");
}

void
LIRGeneratorPPC::visitWasmLoad(MWasmLoad* ins)
{
    MOZ_CRASH("PPC: NYI visitWasmLoad");
}

void
LIRGeneratorPPC::visitWasmStore(MWasmStore* ins)
{
    MOZ_CRASH("PPC: NYI visitWasmStore");
}

void
LIRGeneratorPPC::visitWasmSelect(MWasmSelect* ins)
{
    MOZ_CRASH("PPC: NYI visitWasmSelect");
}

void
LIRGeneratorPPC::visitSignExtendInt64(MSignExtendInt64* ins)
{
    MOZ_CRASH("PPC: visitSignExtendInt64 NYI");
}

// ==========================================================================
// MOZ_CRASH stub definitions for missing LIRGeneratorPPC methods
// ==========================================================================

void
LIRGeneratorPPC::lowerDivI64(MDiv* div)
{
    MOZ_CRASH("PPC: NYI lowerDivI64");
}

void
LIRGeneratorPPC::lowerModI64(MMod* mod)
{
    MOZ_CRASH("PPC: NYI lowerModI64");
}
