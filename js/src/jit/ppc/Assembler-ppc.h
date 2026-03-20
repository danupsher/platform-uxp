#include <cstdio>
/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ppc_Assembler_ppc_h
#define jit_ppc_Assembler_ppc_h

#include "jit/ppc/Architecture-ppc.h"
#include "jit/shared/Assembler-shared.h"
#include "jit/shared/IonAssemblerBuffer.h"
#include "jit/JitCompartment.h"

namespace js {
namespace jit {

// Simple instruction buffer for PPC (all instructions are 32-bit)
class PPCBuffer {
    js::Vector<uint32_t, 256, SystemAllocPolicy> insns_;
    bool oom_;
  public:
    PPCBuffer() : oom_(false) {}
    
    size_t size() const { return insns_.length() * sizeof(uint32_t); }
    bool oom() const { return oom_; }
    void setOOM() { oom_ = true; }
    bool failed() const { return oom_; }
    
    BufferOffset putInt(uint32_t value) {
        size_t offset = insns_.length() * sizeof(uint32_t);
        if (insns_.begin() == nullptr && insns_.length() > 0) {
            fprintf(stderr, "PPC-BUG: putInt buffer null at len=%zu\n", insns_.length());
        }
        if (!insns_.append(value)) {
            fprintf(stderr, "PPC-BUG: putInt OOM at len=%zu cap=%zu\n", insns_.length(), insns_.capacity());
            oom_ = true;
        }
        return BufferOffset(offset);
    }
    
    uint32_t getInst(BufferOffset off) const {
        return insns_[off.getOffset() / sizeof(uint32_t)];
    }
    void setInst(BufferOffset off, uint32_t value) {
        insns_[off.getOffset() / sizeof(uint32_t)] = value;
    }
    
    uint8_t* buffer() { return reinterpret_cast<uint8_t*>(insns_.begin()); }
    const uint8_t* buffer() const { return reinterpret_cast<const uint8_t*>(insns_.begin()); }
    
    bool appendRawCode(const uint8_t* code, size_t numBytes) {
        MOZ_ASSERT(numBytes % sizeof(uint32_t) == 0);
        size_t numInsns = numBytes / sizeof(uint32_t);
        const uint32_t* data = reinterpret_cast<const uint32_t*>(code);
        for (size_t i = 0; i < numInsns; i++) {
            if (!insns_.append(data[i]))
                return false;
        }
        return true;
    }
    
    // For executableCopy
    void executableCopy(uint8_t* dest) const {
        if (size() > 0)
            memcpy(dest, insns_.begin(), size());
    }
};

// PPC register aliases used by the JIT
static constexpr Register r0{Registers::r0};
static constexpr Register r1{Registers::r1};
static constexpr Register r2{Registers::r2};
static constexpr Register r3{Registers::r3};
static constexpr Register r4{Registers::r4};
static constexpr Register r5{Registers::r5};
static constexpr Register r6{Registers::r6};
static constexpr Register r7{Registers::r7};
static constexpr Register r8{Registers::r8};
static constexpr Register r9{Registers::r9};
static constexpr Register r10{Registers::r10};
static constexpr Register r11{Registers::r11};
static constexpr Register r12{Registers::r12};
static constexpr Register r13{Registers::r13};
static constexpr Register r14{Registers::r14};
static constexpr Register r15{Registers::r15};
static constexpr Register r16{Registers::r16};
static constexpr Register r17{Registers::r17};
static constexpr Register r18{Registers::r18};
static constexpr Register r19{Registers::r19};
static constexpr Register r20{Registers::r20};
static constexpr Register r21{Registers::r21};
static constexpr Register r22{Registers::r22};
static constexpr Register r23{Registers::r23};
static constexpr Register r24{Registers::r24};
static constexpr Register r25{Registers::r25};
static constexpr Register r26{Registers::r26};
static constexpr Register r27{Registers::r27};
static constexpr Register r28{Registers::r28};
static constexpr Register r29{Registers::r29};
static constexpr Register r30{Registers::r30};
static constexpr Register r31{Registers::r31};
static constexpr Register InvalidReg{Registers::invalid_reg};
static constexpr FloatRegister InvalidFloatReg;
static constexpr FloatRegister ReturnSimd128Reg = InvalidFloatReg;

static constexpr Register StackPointer{Registers::sp};
static constexpr Register FramePointer{Registers::fp};

// Scratch registers (not allocatable, used by MacroAssembler)
static constexpr Register ScratchRegister{Registers::r11};
static constexpr Register SecondScratchRegister{Registers::r12};

// Helper classes for ScratchRegister usage. Asserts that only one piece
// of code thinks it has exclusive ownership of each scratch register.
struct ScratchRegisterScope : public AutoRegisterScope
{
    explicit ScratchRegisterScope(MacroAssembler& masm)
      : AutoRegisterScope(masm, ScratchRegister)
    { }
};
struct SecondScratchRegisterScope : public AutoRegisterScope
{
    explicit SecondScratchRegisterScope(MacroAssembler& masm)
      : AutoRegisterScope(masm, SecondScratchRegister)
    { }
};

// Call temp registers (caller-saved, available at call sites)
static constexpr Register CallTempReg0{Registers::r5};
static constexpr Register CallTempReg1{Registers::r6};
static constexpr Register CallTempReg2{Registers::r7};
static constexpr Register CallTempReg3{Registers::r8};
static constexpr Register CallTempReg4{Registers::r9};

// RegExp registers — must NOT overlap JSReturnOperand (r5/r4)
static constexpr Register RegExpMatcherRegExpReg = CallTempReg1;
static constexpr Register RegExpMatcherStringReg = CallTempReg2;
static constexpr Register RegExpMatcherLastIndexReg = CallTempReg3;
static constexpr Register RegExpTesterRegExpReg = CallTempReg1;
static constexpr Register RegExpTesterStringReg = CallTempReg2;
static constexpr Register RegExpTesterLastIndexReg = CallTempReg3;
static constexpr Register CallTempReg5{Registers::r10};

static constexpr Register CallTempNonArgRegs[] = { r5, r6, r7, r8, r9, r10 };
static const uint32_t NumCallTempNonArgRegs = mozilla::ArrayLength(CallTempNonArgRegs);

// ABI non-argument registers
static constexpr Register ABINonArgReg0{Registers::r5};
static constexpr Register ABINonArgReg1{Registers::r6};
static constexpr Register ABINonArgReg2{Registers::r7};
static constexpr Register ABINonArgReturnReg0{Registers::r5};
static constexpr Register ABINonArgReturnReg1{Registers::r6};

// WebAssembly TLS register
static constexpr Register WasmTlsReg{Registers::r30};

// Wasm table call registers
static constexpr Register WasmTableCallScratchReg = ABINonArgReg0;
static constexpr Register WasmTableCallSigReg = ABINonArgReg1;
static constexpr Register WasmTableCallIndexReg = ABINonArgReg2;

// JS return registers: nunbox32 returns type in r3, data in r4
static constexpr Register JSReturnReg_Type{Registers::r5};
static constexpr Register JSReturnReg_Data{Registers::r4};
// ReturnReg64 is defined after InvalidReg becomes available
// static constexpr Register64 ReturnReg64{InvalidReg, InvalidReg};

// Floating point registers
static constexpr FloatRegister ReturnFloat32Reg = {FloatRegisters::f1, FloatRegister::Single};
static constexpr FloatRegister ReturnDoubleReg = {FloatRegisters::f1, FloatRegister::Double};
static constexpr FloatRegister ScratchFloat32Reg = {FloatRegisters::f0, FloatRegister::Single};
static constexpr FloatRegister ScratchDoubleReg = {FloatRegisters::f0, FloatRegister::Double};
static constexpr FloatRegister SecondScratchFloat32Reg = {FloatRegisters::f13, FloatRegister::Single};
static constexpr FloatRegister SecondScratchDoubleReg = {FloatRegisters::f13, FloatRegister::Double};

// Named float register constants
static constexpr FloatRegister f0  = {FloatRegisters::f0, FloatRegister::Double};
static constexpr FloatRegister f1  = {FloatRegisters::f1, FloatRegister::Double};
static constexpr FloatRegister f2  = {FloatRegisters::f2, FloatRegister::Double};
static constexpr FloatRegister f3  = {FloatRegisters::f3, FloatRegister::Double};
static constexpr FloatRegister f4  = {FloatRegisters::f4, FloatRegister::Double};
static constexpr FloatRegister f5  = {FloatRegisters::f5, FloatRegister::Double};
static constexpr FloatRegister f6  = {FloatRegisters::f6, FloatRegister::Double};
static constexpr FloatRegister f7  = {FloatRegisters::f7, FloatRegister::Double};
static constexpr FloatRegister f8  = {FloatRegisters::f8, FloatRegister::Double};
static constexpr FloatRegister f9  = {FloatRegisters::f9, FloatRegister::Double};
static constexpr FloatRegister f10 = {FloatRegisters::f10, FloatRegister::Double};
static constexpr FloatRegister f11 = {FloatRegisters::f11, FloatRegister::Double};
static constexpr FloatRegister f12 = {FloatRegisters::f12, FloatRegister::Double};
static constexpr FloatRegister f13 = {FloatRegisters::f13, FloatRegister::Double};
static constexpr FloatRegister f14 = {FloatRegisters::f14, FloatRegister::Double};

// Wasm return registers
static constexpr Register WasmIonExitRegReturnData = JSReturnReg_Data;
static constexpr Register WasmIonExitRegReturnType = JSReturnReg_Type;

// WasmIonExit scratch registers
static constexpr Register WasmIonExitRegE0 = CallTempReg0;
static constexpr Register WasmIonExitRegE1 = CallTempReg1;

// Wasm return registers for doubles (f1, f2, f3 = first 3 volatile FPRs)
static constexpr Register WasmIonExitRegD0 = CallTempReg2;
static constexpr Register WasmIonExitRegD1 = CallTempReg3;
static constexpr Register WasmIonExitRegD2 = CallTempReg4;

// Return register (r3 is the PPC return register)
static constexpr Register ReturnReg{Registers::r3};

// Pre-barrier register
static constexpr Register PreBarrierReg{Registers::r4};

// Osrframe and other JIT registers
static constexpr Register OsrFrameReg{Registers::r6};
static constexpr Register ArgumentsRectifierReg{Registers::r20};

// Integer argument registers (PPC SVR4 ABI: r3-r10)
static constexpr Register IntArgReg0{Registers::r3};
static constexpr Register IntArgReg1{Registers::r4};
static constexpr Register IntArgReg2{Registers::r5};
static constexpr Register IntArgReg3{Registers::r6};
static constexpr Register IntArgReg4{Registers::r7};
static constexpr Register IntArgReg5{Registers::r8};
static constexpr Register IntArgReg6{Registers::r9};
static constexpr Register IntArgReg7{Registers::r10};
// Baseline stub register
static constexpr Register BaselineStubReg{Registers::r18};

// IC return address register — PPC uses LR, but we store it in a GPR
static constexpr Register ICReturnReg{Registers::r0};

// BaselineTailCallReg (same as ICTailCallReg)
static constexpr Register BaselineTailCallReg{Registers::r0};

// Stack alignment
static constexpr uint32_t ABIStackAlignment = 16;
static constexpr uint32_t JitStackAlignment = 16;
static constexpr uint32_t JitStackValueAlignment = JitStackAlignment / sizeof(Value);
static_assert(JitStackAlignment % sizeof(Value) == 0 &&
              JitStackValueAlignment >= 1,
              "Stack alignment should be a non-zero multiple of sizeof(Value)");

static constexpr uint32_t CodeAlignment = 4;

static constexpr uint32_t SimdMemoryAlignment = 16;
static constexpr uint32_t WasmStackAlignment = SimdMemoryAlignment;

static constexpr bool SupportsUint32x4FloatConversions = false;
static constexpr bool SupportsUint8x16Compares = false;
static constexpr bool SupportsUint16x8Compares = false;
static constexpr bool SupportsUint32x4Compares = false;

// These are used in static_assert in CodeGenerator-shared.cpp
static constexpr bool SupportsSimd = false;

static constexpr Scale ScalePointer = TimesFour;

// Number of integer argument registers
static const uint32_t NumIntArgRegs = 8;

static inline bool
GetIntArgReg(uint32_t usedArgSlots, Register* out)
{
    if (usedArgSlots < NumIntArgRegs) {
        *out = Register::FromCode(Registers::r3 + usedArgSlots);
        return true;
    }
    return false;
}

static inline bool
GetTempRegForIntArg(uint32_t usedIntArgs, uint32_t usedFloatArgs, Register* out)
{
    MOZ_ASSERT(usedFloatArgs == 0);
    if (GetIntArgReg(usedIntArgs, out))
        return true;
    usedIntArgs -= NumIntArgRegs;
    if (usedIntArgs >= NumCallTempNonArgRegs)
        return false;
    *out = CallTempNonArgRegs[usedIntArgs];
    return true;
}

static inline uint32_t
GetArgStackDisp(uint32_t usedArgSlots)
{
    MOZ_ASSERT(usedArgSlots >= NumIntArgRegs);
    return usedArgSlots * sizeof(intptr_t);
}

// =========================================================================
// PPC Instruction encoding helpers
// =========================================================================

// Primary opcodes
enum PPCOpcode {
    OP_ADDI   = 14 << 26,
    OP_ADDIS  = 15 << 26,
    OP_ORI    = 24 << 26,
    OP_ORIS   = 25 << 26,
    OP_XORI   = 26 << 26,
    OP_ANDI_  = 28 << 26,
    OP_ANDIS_ = 29 << 26,
    OP_MULLI  = 7 << 26,
    OP_ADDIC  = 12 << 26,
    OP_SUBFIC = 8 << 26,
    OP_CMPWI  = 11 << 26,
    OP_CMPLWI = 10 << 26,
    OP_LWZ    = 32 << 26,
    OP_LWZU   = 33 << 26,
    OP_LBZ    = 34 << 26,
    OP_LHZ    = 40 << 26,
    OP_LHA    = 42 << 26,
    OP_STW    = 36 << 26,
    OP_STWU   = 37 << 26,
    OP_STB    = 38 << 26,
    OP_STH    = 44 << 26,
    OP_LMW    = 46 << 26,
    OP_STMW   = 47 << 26,
    OP_LFS    = 48 << 26,
    OP_LFD    = 50 << 26,
    OP_STFS   = 52 << 26,
    OP_STFD   = 54 << 26,
    OP_B      = 18 << 26,
    OP_BC     = 16 << 26,
    OP_X31    = 31 << 26,
    OP_X63    = 63 << 26,
    OP_X59    = 59 << 26,
    OP_RLWINM = 21 << 26,
    OP_RLWIMI = 20 << 26,
    OP_RLWNM  = 23 << 26,
};

// Extended opcodes for OP_X31
enum PPCExtOp31 {
    XO_ADD    = 266, XO_ADDC   = 10,  XO_ADDE   = 138, XO_ADDZE  = 202,
    XO_SUBF   = 40,  XO_SUBFC  = 8,   XO_SUBFE  = 136, XO_SUBFZE = 200,
    XO_NEG    = 104,
    XO_MULLW  = 235, XO_MULHW  = 75,  XO_MULHWU = 11,
    XO_DIVW   = 491, XO_DIVWU  = 459,
    XO_AND    = 28,  XO_ANDC   = 60,  XO_OR     = 444, XO_ORC    = 412,
    XO_XOR    = 316, XO_NAND   = 476, XO_NOR    = 124, XO_EQV    = 284,
    XO_SLW    = 24,  XO_SRW    = 536, XO_SRAW   = 792, XO_SRAWI  = 824,
    XO_CMPW   = 0,   XO_CMPLW  = 32,
    XO_EXTSB  = 954, XO_EXTSH  = 922, XO_CNTLZW = 26,
    XO_LWZX   = 23,  XO_LBZX   = 87,  XO_LHZX   = 279, XO_LHAX   = 343,
    XO_STWX   = 151, XO_STBX   = 215, XO_STHX   = 407,
    XO_LWBRX  = 534, XO_STWBRX = 662,
    XO_LFSX   = 535, XO_LFDX   = 599, XO_STFSX  = 663, XO_STFDX  = 727,
    XO_MFSPR  = 339, XO_MTSPR  = 467,
    XO_MFCR   = 19,  XO_MTCRF  = 144,
    XO_SYNC   = 598, XO_ISYNC  = 150,
    XO_LWARX  = 20,  XO_STWCX_ = 150,
    XO_DCBF   = 86,  XO_ICBI   = 982,
};

// Extended opcodes for OP_X63 (double-precision FP)
enum PPCExtOp63 {
    XO_FADD   = 21,  XO_FSUB   = 20,  XO_FMUL   = 25,  XO_FDIV   = 18,
    XO_FNEG   = 40,  XO_FABS   = 264, XO_FNABS  = 136,
    XO_FMR    = 72,
    XO_FCMPU  = 0,   XO_FCMPO  = 32,
    XO_FRSP   = 12,  // Round to single
    XO_FCTIW  = 14,  XO_FCTIWZ = 15,  // Convert to integer word
    XO_FSEL   = 23,
    XO_FMADD  = 29,  XO_FMSUB  = 28,
    XO_FNMADD = 31,  XO_FNMSUB = 30,
    XO_FSQRT  = 22,
    XO_MFFS   = 583, XO_MTFSF  = 711, XO_MTFSB0 = 70,  XO_MTFSB1 = 38,
    XO_MTFSFI = 134,
};

// SPR numbers
enum PPCSPR {
    SPR_LR  = 8,
    SPR_CTR = 9,
    SPR_XER = 1,
};

// CR bit definitions
enum PPCCondBit {
    CR_LT = 0,
    CR_GT = 1,
    CR_EQ = 2,
    CR_SO = 3,
};

// Branch BO field encodings
enum PPCBO {
    BO_TRUE       = 12,
    BO_FALSE      = 4,
    BO_ALWAYS     = 20,
};

// =========================================================================
// ABIArgGenerator
// =========================================================================

class ABIArgGenerator
{
    unsigned usedGPRs_;
    unsigned usedFPRs_;
    unsigned stackOffset_;
    ABIArg current_;

  public:
    ABIArgGenerator();
    ABIArg next(MIRType argType);
    ABIArg& current() { return current_; }
    uint32_t stackBytesConsumedSoFar() const { return stackOffset_; }
};

// =========================================================================
// Assembler
// =========================================================================

// Operand - represents a register, float register, or memory operand
class Operand
{
  public:
    enum Tag {
        REG,
        FREG,
        MEM
    };

  private:
    Tag tag : 3;
    uint32_t reg : 5;
    int32_t offset;

  public:
    Operand(Register reg_)
      : tag(REG), reg(reg_.code()), offset(0)
    { }

    Operand(FloatRegister freg)
      : tag(FREG), reg(freg.code()), offset(0)
    { }

    Operand(Register base, Imm32 off)
      : tag(MEM), reg(base.code()), offset(off.value)
    { }

    Operand(Register base, int32_t off)
      : tag(MEM), reg(base.code()), offset(off)
    { }

    Operand(const Address& addr)
      : tag(MEM), reg(addr.base.code()), offset(addr.offset)
    { }

    Tag getTag() const { return tag; }

    Register toReg() const {
        MOZ_ASSERT(tag == REG);
        return Register::FromCode(reg);
    }

    FloatRegister toFReg() const {
        MOZ_ASSERT(tag == FREG);
        return FloatRegister::FromCode(reg);
    }

    Address toAddress() const {
        MOZ_ASSERT(tag == MEM);
        return Address(Register::FromCode(reg), offset);
    }

    int32_t disp() const {
        MOZ_ASSERT(tag == MEM);
        return offset;
    }

    Register baseReg() const {
        MOZ_ASSERT(tag == MEM);
        return Register::FromCode(reg);
    }
};

class Assembler : public AssemblerShared
{
  public:
    enum Condition {
        Equal,
        NotEqual,
        Above,
        AboveOrEqual,
        Below,
        BelowOrEqual,
        GreaterThan,
        GreaterThanOrEqual,
        LessThan,
        LessThanOrEqual,
        Overflow,
        CarrySet,
        CarryClear,
        Signed,
        NotSigned,
        Zero,
        NonZero,
        Always,
    };

    enum DoubleCondition {
        DoubleOrdered,
        DoubleEqual,
        DoubleNotEqual,
        DoubleGreaterThan,
        DoubleGreaterThanOrEqual,
        DoubleLessThan,
        DoubleLessThanOrEqual,
        DoubleUnordered,
        DoubleEqualOrUnordered,
        DoubleNotEqualOrUnordered,
        DoubleGreaterThanOrUnordered,
        DoubleGreaterThanOrEqualOrUnordered,
        DoubleLessThanOrUnordered,
        DoubleLessThanOrEqualOrUnordered
    };

    enum FloatFormat {
        SingleFloat,
        DoubleFloat
    };

    enum FloatTestKind {
        TestForTrue,
        TestForFalse
    };

    // Relocation tracking
    struct RelativePatch {
        BufferOffset offset;
        void* target;
        Relocation::Kind kind;

        RelativePatch(BufferOffset offset, void* target, Relocation::Kind kind)
          : offset(offset), target(target), kind(kind)
        { }
    };

  protected:
    js::Vector<RelativePatch, 8, SystemAllocPolicy> jumps_;
    CompactBufferWriter jumpRelocations_;
    CompactBufferWriter dataRelocations_;
    CompactBufferWriter preBarriers_;
    bool isFinished;

  public:
    PPCBuffer m_buffer;

    Assembler()
      : AssemblerShared(),
        isFinished(false)
    { }

    static Condition InvertCondition(Condition cond);
    static DoubleCondition InvertCondition(DoubleCondition cond);
    static Condition UnsignedCondition(Condition cond);
    static Condition ConditionWithoutEqual(Condition cond);

    // Relocation and data tracking
    void writeRelocation(BufferOffset src) {
        jumpRelocations_.writeUnsigned(src.getOffset());
    }
    void writeDataRelocation(ImmGCPtr ptr) {
        if (ptr.value) {
            if (gc::IsInsideNursery(ptr.value))
                embedsNurseryPointers_ = true;
            dataRelocations_.writeUnsigned(nextOffset().getOffset());
        }
    }
    void writePrebarrierOffset(CodeOffset label) {
        preBarriers_.writeUnsigned(label.offset());
    }

    uint32_t currentOffset() {
        return nextOffset().getOffset();
    }
    BufferOffset nextOffset() {
        return BufferOffset(m_buffer.size());
    }

    void bind(CodeOffset* label) {
        label->bind(currentOffset());
    }
    bool bailed() {
        return m_buffer.oom();
    }
    void retarget(Label* label, Label* target);
    static uint32_t NopSize() { return 4; }

    void addJump(RelativePatch rp) {
        jumps_.append(rp);
    }

    static const Register getStackPointer() {
        return StackPointer;
    }

    // Emit a raw 32-bit instruction
    BufferOffset writeInst(uint32_t inst) {
        BufferOffset off(m_buffer.size());
        m_buffer.putInt(inst);
        return off;
    }

    // Finalization
    void finish();
    bool asmMergeWith(const Assembler& other);
    void executableCopy(uint8_t* buffer);
    void copyJumpRelocationTable(uint8_t* dest);
    void copyDataRelocationTable(uint8_t* dest);
    void copyPreBarrierTable(uint8_t* dest);
    void processCodeLabels(uint8_t* rawCode);

    size_t size() const { return m_buffer.size(); }
    const uint8_t* buffer() const { return m_buffer.buffer(); }
    bool oom() const { return m_buffer.oom() || AssemblerShared::oom(); }
    size_t jumpRelocationTableBytes() const;
    size_t dataRelocationTableBytes() const;
    size_t preBarrierTableBytes() const;
    size_t bytesNeeded() const;

    // GC tracing
    void trace(JSTracer* trc);
    static void TraceOneDataRelocation(JSTracer* trc, uint8_t* inst);
    static void TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader);
    static void TraceDataRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader);

    // Code patching (lis+ori pairs)
    static uint32_t ExtractLisOriValue(uint8_t* inst);
    static void UpdateLisOriValue(uint8_t* inst, uint32_t value);

    static uintptr_t GetPointer(uint8_t*);
    void Bind(uint8_t* rawCode, CodeOffset* label, const void* address);

    static void PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue,
                                        ImmPtr expectedValue);
    static void PatchDataWithValueCheck(CodeLocationLabel label, PatchedImmPtr newValue,
                                        PatchedImmPtr expectedValue);
    static void PatchInstructionImmediate(uint8_t* code, PatchedImmPtr imm);
    static uint32_t ExtractInstructionImmediate(uint8_t* code);
    static void ToggleCall(CodeLocationLabel inst_, bool enabled);

    // Instruction size for immediate loading (lis + ori = 2 instructions)
    static uint32_t InstructionImmediateSize() {
        return 2 * sizeof(uint32_t);
    }

    // =====================================================================
    // PPC instruction emitters
    // =====================================================================

    // D-form: opcode | rt << 21 | ra << 16 | (d & 0xffff)
    BufferOffset as_dform(PPCOpcode op, Register rt, Register ra, int32_t d) {
        return writeInst(op | (rt.code() << 21) | (ra.code() << 16) | (d & 0xffff));
    }

    // X-form: opcode(31) | rt << 21 | ra << 16 | rb << 11 | xo << 1 | rc
    BufferOffset as_xform(PPCExtOp31 xo, Register rt, Register ra, Register rb, bool rc = false) {
        return writeInst(OP_X31 | (rt.code() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (xo << 1) | (rc ? 1 : 0));
    }

    // Integer arithmetic (D-form)
    BufferOffset as_addi(Register rt, Register ra, int32_t simm) { return as_dform(OP_ADDI, rt, ra, simm); }
    BufferOffset as_addis(Register rt, Register ra, int32_t simm) { return as_dform(OP_ADDIS, rt, ra, simm); }
    BufferOffset as_ori(Register ra, Register rs, uint32_t uimm) {
        return writeInst(OP_ORI | (rs.code() << 21) | (ra.code() << 16) | (uimm & 0xffff));
    }
    BufferOffset as_oris(Register ra, Register rs, uint32_t uimm) {
        return writeInst(OP_ORIS | (rs.code() << 21) | (ra.code() << 16) | (uimm & 0xffff));
    }
    BufferOffset as_xori(Register ra, Register rs, uint32_t uimm) {
        return writeInst(OP_XORI | (rs.code() << 21) | (ra.code() << 16) | (uimm & 0xffff));
    }
    BufferOffset as_andi(Register ra, Register rs, uint32_t uimm) {
        return writeInst(OP_ANDI_ | (rs.code() << 21) | (ra.code() << 16) | (uimm & 0xffff));
    }
    BufferOffset as_andis(Register ra, Register rs, uint32_t uimm) {
        return writeInst(OP_ANDIS_ | (rs.code() << 21) | (ra.code() << 16) | (uimm & 0xffff));
    }
    BufferOffset as_mulli(Register rt, Register ra, int32_t simm) { return as_dform(OP_MULLI, rt, ra, simm); }

    // Compare (D-form)
    BufferOffset as_cmpwi(Register ra, int32_t simm, uint32_t cr = 0) {
        return writeInst(OP_CMPWI | (cr << 23) | (ra.code() << 16) | (simm & 0xffff));
    }
    BufferOffset as_cmplwi(Register ra, uint32_t uimm, uint32_t cr = 0) {
        return writeInst(OP_CMPLWI | (cr << 23) | (ra.code() << 16) | (uimm & 0xffff));
    }

    // Integer arithmetic (X/XO-form)
    BufferOffset as_add(Register rt, Register ra, Register rb, bool rc = false) {
        return as_xform(XO_ADD, rt, ra, rb, rc);
    }
    BufferOffset as_addc(Register rt, Register ra, Register rb, bool rc = false) {
        return as_xform(XO_ADDC, rt, ra, rb, rc);
    }
    BufferOffset as_adde(Register rt, Register ra, Register rb, bool rc = false) {
        return as_xform(XO_ADDE, rt, ra, rb, rc);
    }
    BufferOffset as_subf(Register rt, Register ra, Register rb, bool rc = false) {
        return as_xform(XO_SUBF, rt, ra, rb, rc);
    }
    BufferOffset as_subfc(Register rt, Register ra, Register rb, bool rc = false) {
        return as_xform(XO_SUBFC, rt, ra, rb, rc);
    }
    BufferOffset as_subfe(Register rt, Register ra, Register rb, bool rc = false) {
        return as_xform(XO_SUBFE, rt, ra, rb, rc);
    }
    BufferOffset as_addze(Register rt, Register ra) {
        return writeInst(OP_X31 | (rt.code() << 21) | (ra.code() << 16) | (XO_ADDZE << 1));
    }
    BufferOffset as_addic(Register rt, Register ra, int32_t simm) {
        return as_dform(OP_ADDIC, rt, ra, simm);
    }
    BufferOffset as_mullw(Register rt, Register ra, Register rb, bool rc = false) {
        return as_xform(XO_MULLW, rt, ra, rb, rc);
    }
    BufferOffset as_mulhw(Register rt, Register ra, Register rb) {
        return as_xform(XO_MULHW, rt, ra, rb);
    }
    BufferOffset as_mulhwu(Register rt, Register ra, Register rb) {
        return as_xform(XO_MULHWU, rt, ra, rb);
    }
    BufferOffset as_divw(Register rt, Register ra, Register rb) {
        return as_xform(XO_DIVW, rt, ra, rb);
    }
    BufferOffset as_divwu(Register rt, Register ra, Register rb) {
        return as_xform(XO_DIVWU, rt, ra, rb);
    }
    BufferOffset as_neg(Register rt, Register ra) {
        return writeInst(OP_X31 | (rt.code() << 21) | (ra.code() << 16) | (XO_NEG << 1));
    }

    // Logic (X-form)
    BufferOffset as_and(Register ra, Register rs, Register rb, bool rc = false) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_AND << 1) | (rc ? 1 : 0));
    }
    BufferOffset as_or(Register ra, Register rs, Register rb, bool rc = false) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_OR << 1) | (rc ? 1 : 0));
    }
    BufferOffset as_xor(Register ra, Register rs, Register rb, bool rc = false) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_XOR << 1) | (rc ? 1 : 0));
    }
    BufferOffset as_nor(Register ra, Register rs, Register rb, bool rc = false) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_NOR << 1) | (rc ? 1 : 0));
    }
    BufferOffset as_mr(Register ra, Register rs) { return as_or(ra, rs, rs); }

    // Shifts
    BufferOffset as_slw(Register ra, Register rs, Register rb, bool rc = false) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_SLW << 1) | (rc ? 1 : 0));
    }
    BufferOffset as_srw(Register ra, Register rs, Register rb, bool rc = false) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_SRW << 1) | (rc ? 1 : 0));
    }
    BufferOffset as_sraw(Register ra, Register rs, Register rb, bool rc = false) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_SRAW << 1) | (rc ? 1 : 0));
    }
    BufferOffset as_srawi(Register ra, Register rs, uint32_t sh, bool rc = false) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) |
                         (sh << 11) | (XO_SRAWI << 1) | (rc ? 1 : 0));
    }

    // Sign/zero extend
    BufferOffset as_extsb(Register ra, Register rs) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) | (XO_EXTSB << 1));
    }
    BufferOffset as_extsh(Register ra, Register rs) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) | (XO_EXTSH << 1));
    }
    BufferOffset as_cntlzw(Register ra, Register rs) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) | (XO_CNTLZW << 1));
    }

    // Compare (X-form)
    BufferOffset as_cmpw(Register ra, Register rb, uint32_t cr = 0) {
        return writeInst(OP_X31 | (cr << 23) | (ra.code() << 16) | (rb.code() << 11) | (XO_CMPW << 1));
    }
    BufferOffset as_cmplw(Register ra, Register rb, uint32_t cr = 0) {
        return writeInst(OP_X31 | (cr << 23) | (ra.code() << 16) | (rb.code() << 11) | (XO_CMPLW << 1));
    }

    // Load/store (D-form)
    BufferOffset as_lwz(Register rt, Register ra, int32_t d) { return as_dform(OP_LWZ, rt, ra, d); }
    BufferOffset as_lbz(Register rt, Register ra, int32_t d) { return as_dform(OP_LBZ, rt, ra, d); }
    BufferOffset as_lhz(Register rt, Register ra, int32_t d) { return as_dform(OP_LHZ, rt, ra, d); }
    BufferOffset as_lha(Register rt, Register ra, int32_t d) { return as_dform(OP_LHA, rt, ra, d); }
    BufferOffset as_stw(Register rs, Register ra, int32_t d) { return as_dform(OP_STW, rs, ra, d); }
    BufferOffset as_stwu(Register rs, Register ra, int32_t d) { return as_dform(OP_STWU, rs, ra, d); }
    BufferOffset as_stb(Register rs, Register ra, int32_t d) { return as_dform(OP_STB, rs, ra, d); }
    BufferOffset as_sth(Register rs, Register ra, int32_t d) { return as_dform(OP_STH, rs, ra, d); }

    // Load/store indexed (X-form)
    BufferOffset as_lwzx(Register rt, Register ra, Register rb) { return as_xform(XO_LWZX, rt, ra, rb); }
    BufferOffset as_lbzx(Register rt, Register ra, Register rb) { return as_xform(XO_LBZX, rt, ra, rb); }
    BufferOffset as_lhzx(Register rt, Register ra, Register rb) { return as_xform(XO_LHZX, rt, ra, rb); }
    BufferOffset as_lhax(Register rt, Register ra, Register rb) { return as_xform(XO_LHAX, rt, ra, rb); }
    BufferOffset as_stwx(Register rs, Register ra, Register rb) { return as_xform(XO_STWX, rs, ra, rb); }
    BufferOffset as_stbx(Register rs, Register ra, Register rb) { return as_xform(XO_STBX, rs, ra, rb); }
    BufferOffset as_sthx(Register rs, Register ra, Register rb) { return as_xform(XO_STHX, rs, ra, rb); }

    // FP load/store (D-form)
    BufferOffset as_lfs(FloatRegister frt, Register ra, int32_t d) {
        return writeInst(OP_LFS | (frt.encoding() << 21) | (ra.code() << 16) | (d & 0xffff));
    }
    BufferOffset as_lfd(FloatRegister frt, Register ra, int32_t d) {
        return writeInst(OP_LFD | (frt.encoding() << 21) | (ra.code() << 16) | (d & 0xffff));
    }
    BufferOffset as_stfs(FloatRegister frs, Register ra, int32_t d) {
        return writeInst(OP_STFS | (frs.encoding() << 21) | (ra.code() << 16) | (d & 0xffff));
    }
    BufferOffset as_stfd(FloatRegister frs, Register ra, int32_t d) {
        return writeInst(OP_STFD | (frs.encoding() << 21) | (ra.code() << 16) | (d & 0xffff));
    }

    // FP load/store indexed (X-form via OP_X31)
    BufferOffset as_lfsx(FloatRegister frt, Register ra, Register rb) {
        return writeInst(OP_X31 | (frt.encoding() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_LFSX << 1));
    }
    BufferOffset as_lfdx(FloatRegister frt, Register ra, Register rb) {
        return writeInst(OP_X31 | (frt.encoding() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_LFDX << 1));
    }
    BufferOffset as_stfsx(FloatRegister frs, Register ra, Register rb) {
        return writeInst(OP_X31 | (frs.encoding() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_STFSX << 1));
    }
    BufferOffset as_stfdx(FloatRegister frs, Register ra, Register rb) {
        return writeInst(OP_X31 | (frs.encoding() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_STFDX << 1));
    }

    // FP arithmetic (A-form, opcode 63)
    BufferOffset as_fadd(FloatRegister frt, FloatRegister fra, FloatRegister frb) {
        return writeInst(OP_X63 | (frt.encoding() << 21) | (fra.encoding() << 16) |
                         (frb.encoding() << 11) | (XO_FADD << 1));
    }
    BufferOffset as_fsub(FloatRegister frt, FloatRegister fra, FloatRegister frb) {
        return writeInst(OP_X63 | (frt.encoding() << 21) | (fra.encoding() << 16) |
                         (frb.encoding() << 11) | (XO_FSUB << 1));
    }
    BufferOffset as_fmul(FloatRegister frt, FloatRegister fra, FloatRegister frc) {
        // fmul uses frc in bits 6-10, not frb
        return writeInst(OP_X63 | (frt.encoding() << 21) | (fra.encoding() << 16) |
                         (frc.encoding() << 6) | (XO_FMUL << 1));
    }
    BufferOffset as_fdiv(FloatRegister frt, FloatRegister fra, FloatRegister frb) {
        return writeInst(OP_X63 | (frt.encoding() << 21) | (fra.encoding() << 16) |
                         (frb.encoding() << 11) | (XO_FDIV << 1));
    }
    BufferOffset as_fneg(FloatRegister frt, FloatRegister frb) {
        return writeInst(OP_X63 | (frt.encoding() << 21) | (frb.encoding() << 11) | (XO_FNEG << 1));
    }
    BufferOffset as_fabs(FloatRegister frt, FloatRegister frb) {
        return writeInst(OP_X63 | (frt.encoding() << 21) | (frb.encoding() << 11) | (XO_FABS << 1));
    }
    BufferOffset as_fmr(FloatRegister frt, FloatRegister frb) {
        return writeInst(OP_X63 | (frt.encoding() << 21) | (frb.encoding() << 11) | (XO_FMR << 1));
    }
    BufferOffset as_fsqrt(FloatRegister frt, FloatRegister frb) {
        return writeInst(OP_X63 | (frt.encoding() << 21) | (frb.encoding() << 11) | (XO_FSQRT << 1));
    }

    // FP compare
    BufferOffset as_fcmpu(FloatRegister fra, FloatRegister frb, uint32_t cr = 0) {
        return writeInst(OP_X63 | (cr << 23) | (fra.encoding() << 16) |
                         (frb.encoding() << 11) | (XO_FCMPU << 1));
    }

    // FP conversion
    BufferOffset as_frsp(FloatRegister frt, FloatRegister frb) {
        return writeInst(OP_X63 | (frt.encoding() << 21) | (frb.encoding() << 11) | (XO_FRSP << 1));
    }
    BufferOffset as_fctiwz(FloatRegister frt, FloatRegister frb) {
        return writeInst(OP_X63 | (frt.encoding() << 21) | (frb.encoding() << 11) | (XO_FCTIWZ << 1));
    }

    // Single-precision arithmetic (opcode 59)
    BufferOffset as_fadds(FloatRegister frt, FloatRegister fra, FloatRegister frb) {
        return writeInst(OP_X59 | (frt.encoding() << 21) | (fra.encoding() << 16) |
                         (frb.encoding() << 11) | (XO_FADD << 1));
    }
    BufferOffset as_fsubs(FloatRegister frt, FloatRegister fra, FloatRegister frb) {
        return writeInst(OP_X59 | (frt.encoding() << 21) | (fra.encoding() << 16) |
                         (frb.encoding() << 11) | (XO_FSUB << 1));
    }
    BufferOffset as_fmuls(FloatRegister frt, FloatRegister fra, FloatRegister frc) {
        return writeInst(OP_X59 | (frt.encoding() << 21) | (fra.encoding() << 16) |
                         (frc.encoding() << 6) | (XO_FMUL << 1));
    }
    BufferOffset as_fdivs(FloatRegister frt, FloatRegister fra, FloatRegister frb) {
        return writeInst(OP_X59 | (frt.encoding() << 21) | (fra.encoding() << 16) |
                         (frb.encoding() << 11) | (XO_FDIV << 1));
    }

    // Branches
    BufferOffset as_b(int32_t offset, bool link = false) {
        return writeInst(OP_B | (offset & 0x03fffffc) | (link ? 1 : 0));
    }
    BufferOffset as_bl(int32_t offset) { return as_b(offset, true); }

    BufferOffset as_bc(uint32_t bo, uint32_t bi, int32_t offset, bool link = false) {
        return writeInst(OP_BC | (bo << 21) | (bi << 16) | (offset & 0xfffc) | (link ? 1 : 0));
    }

    BufferOffset as_blr() {
        return writeInst((19 << 26) | (BO_ALWAYS << 21) | (16 << 1));
    }
    BufferOffset as_blrl() {
        return writeInst((19 << 26) | (BO_ALWAYS << 21) | (16 << 1) | 1);
    }
    BufferOffset as_bctr() {
        return writeInst((19 << 26) | (BO_ALWAYS << 21) | (528 << 1));
    }
    BufferOffset as_bctrl() {
        return writeInst((19 << 26) | (BO_ALWAYS << 21) | (528 << 1) | 1);
    }
    BufferOffset as_bclr(uint32_t bo, uint32_t bi) {
        return writeInst((19 << 26) | (bo << 21) | (bi << 16) | (16 << 1));
    }

    // SPR access
    BufferOffset as_mflr(Register rt) {
        uint32_t spr = ((SPR_LR & 0x1f) << 5) | ((SPR_LR >> 5) & 0x1f);
        return writeInst(OP_X31 | (rt.code() << 21) | (spr << 11) | (XO_MFSPR << 1));
    }
    BufferOffset as_mtlr(Register rs) {
        uint32_t spr = ((SPR_LR & 0x1f) << 5) | ((SPR_LR >> 5) & 0x1f);
        return writeInst(OP_X31 | (rs.code() << 21) | (spr << 11) | (XO_MTSPR << 1));
    }
    BufferOffset as_mtctr(Register rs) {
        uint32_t spr = ((SPR_CTR & 0x1f) << 5) | ((SPR_CTR >> 5) & 0x1f);
        return writeInst(OP_X31 | (rs.code() << 21) | (spr << 11) | (XO_MTSPR << 1));
    }

    // Condition register
    BufferOffset as_mfcr(Register rt) {
        return writeInst(OP_X31 | (rt.code() << 21) | (XO_MFCR << 1));
    }

    // Rotate/mask
    BufferOffset as_rlwinm(Register ra, Register rs, uint32_t sh, uint32_t mb, uint32_t me, bool rc = false) {
        return writeInst(OP_RLWINM | (rs.code() << 21) | (ra.code() << 16) |
                         (sh << 11) | (mb << 6) | (me << 1) | (rc ? 1 : 0));
    }
    BufferOffset as_rlwimi(Register ra, Register rs, uint32_t sh, uint32_t mb, uint32_t me, bool rc = false) {
        return writeInst(OP_RLWIMI | (rs.code() << 21) | (ra.code() << 16) |
                         (sh << 11) | (mb << 6) | (me << 1) | (rc ? 1 : 0));
    }

    // Pseudo-instructions
    BufferOffset as_li(Register rd, int32_t imm) { return as_addi(rd, r0, imm); }
    BufferOffset as_lis(Register rd, int32_t imm) { return as_addis(rd, r0, imm); }

    BufferOffset as_li32(Register rd, uint32_t imm) {
        as_lis(rd, (imm >> 16) & 0xffff);
        return as_ori(rd, rd, imm & 0xffff);
    }

    BufferOffset as_nop() { return as_ori(r0, r0, 0); }

    // Memory barriers
    BufferOffset as_sync() {
        return writeInst(OP_X31 | (XO_SYNC << 1));
    }
    BufferOffset as_isync() {
        return writeInst((19 << 26) | (XO_ISYNC << 1));
    }

    // Cache control
    BufferOffset as_dcbf(Register ra, Register rb) {
        return writeInst(OP_X31 | (ra.code() << 16) | (rb.code() << 11) | (XO_DCBF << 1));
    }
    BufferOffset as_icbi(Register ra, Register rb) {
        return writeInst(OP_X31 | (ra.code() << 16) | (rb.code() << 11) | (XO_ICBI << 1));
    }

    // Atomics
    BufferOffset as_lwarx(Register rt, Register ra, Register rb) {
        return as_xform(XO_LWARX, rt, ra, rb);
    }
    BufferOffset as_stwcx(Register rs, Register ra, Register rb) {
        return writeInst(OP_X31 | (rs.code() << 21) | (ra.code() << 16) |
                         (rb.code() << 11) | (XO_STWCX_ << 1) | 1);
    }
    // PPC near call = single bl instruction (4 bytes)
    static uint32_t PatchWrite_NearCallSize() {
        return sizeof(uint32_t);
    }
    static void ToggleToCmp(CodeLocationLabel inst) {
        // Disable a toggled jump by replacing b-offset with cmpwi cr7,r0,offset/4.
        // This preserves the branch offset in the 16-bit SIMM field while being
        // effectively a NOP (only modifies CR7, no GPR side effects).
        // cmpwi cr7, r0, SIMM = 0x2F800000 | SIMM16
        uint32_t* p = (uint32_t*)inst.raw();
        uint32_t inst_val = *p;
        MOZ_ASSERT((inst_val >> 26) == 18 || (inst_val >> 26) == 11);
        if ((inst_val >> 26) == 18) {
            // b-instruction: extract LI from bits 25:2 (24-bit signed, *4 = byte offset)
            uint32_t li = (inst_val >> 2) & 0x00FFFFFF;
            // Store low 16 bits of LI in cmpwi SIMM field
            *p = 0x2F800000 | (li & 0xFFFF);
        }
        // else already toggled (cmpwi), nothing to do
        AutoFlushICache::flush(uintptr_t(p), 4);
    }
    static void ToggleToJmp(CodeLocationLabel inst) {
        // Re-enable a toggled jump: restore b-offset from cmpwi cr7,r0,offset/4.
        // cmpwi cr7, r0, SIMM = 0x2F800000 | SIMM16
        // b offset = 0x48000000 | (LI << 2)
        uint32_t* p = (uint32_t*)inst.raw();
        uint32_t inst_val = *p;
        MOZ_ASSERT((inst_val >> 26) == 11 || (inst_val >> 26) == 18);
        if ((inst_val >> 26) == 11) {
            // cmpwi: extract SIMM (low 16 bits = low 16 of LI)
            uint32_t li = inst_val & 0xFFFF;
            // Reconstruct b instruction (offset = LI * 4, always forward, < 256KB)
            *p = 0x48000000 | (li << 2);
        }
        // else already a branch, nothing to do
        AutoFlushICache::flush(uintptr_t(p), 4);
    }
    static void PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm) {
        // Write a 32-bit value to the 4 bytes before the return address.
        // This overwrites the call instruction with raw data (the delta to
        // the IonScript). The invalidated code will never execute this
        // instruction again, so no icache flush needed.
        uint32_t* raw = (uint32_t*)label.raw();
        *(raw - 1) = imm.value;
    }
    static void PatchWrite_NearCall(CodeLocationLabel start, CodeLocationLabel toCall) {
        // Patch the OSI point with a bl (branch-and-link) to the
        // invalidation epilogue.
        uint32_t* inst = (uint32_t*)start.raw();
        ptrdiff_t offset = (uint8_t*)toCall.raw() - (uint8_t*)start.raw();
        // PPC bl: opcode 18, LK=1, AA=0
        // Offset is in bits 6-29 (26-bit signed, but we use 24-bit field * 4)
        MOZ_ASSERT(offset == (int32_t)offset);
        MOZ_ASSERT((offset & 3) == 0);
        *inst = 0x48000001 | ((uint32_t)offset & 0x03FFFFFC);
        AutoFlushICache::flush(uintptr_t(inst), PatchWrite_NearCallSize());
    }

}; // class Assembler

// PPC is big-endian: first word (offset 0) = high, second word (offset 4) = low
inline Imm32
Imm64::firstHalf() const
{
    return hi();
}

inline Imm32
Imm64::secondHalf() const
{
    return low();
}

// Jump/backedge patching (non-member)
void PatchJump(CodeLocationJump& jump_, CodeLocationLabel label,
               ReprotectCode reprotect = DontReprotect);
void PatchBackedge(CodeLocationJump& jump, CodeLocationLabel label,
                   JitRuntime::BackedgeTarget target);

// Wasm registers (matching MIPS convention)
static constexpr Register GlobalReg{Registers::r29};
static constexpr Register HeapReg{Registers::r30};
static const int32_t WasmGlobalRegBias = 32768;

static const uint32_t NumArgRegs = NumIntArgRegs;

static constexpr Register64 ReturnReg64{InvalidReg, InvalidReg};

} // namespace jit
} // namespace js

#endif /* jit_ppc_Assembler_ppc_h */
