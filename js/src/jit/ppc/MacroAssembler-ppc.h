/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ppc_MacroAssembler_ppc_h
#define jit_ppc_MacroAssembler_ppc_h

#include "jsopcode.h"

#include "jit/IonCaches.h"
#include "jit/JitFrames.h"
#include "jit/ppc/Assembler-ppc.h"
#include "jit/MoveResolver.h"

namespace js {
namespace jit {

enum LoadStoreSize {
    SizeByte = 8,
    SizeHalfWord = 16,
    SizeWord = 32,
    SizeDouble = 64
};

enum LoadStoreExtension {
    ZeroExtend = 0,
    SignExtend = 1
};

struct ImmTag : public Imm32
{
    ImmTag(JSValueTag mask)
      : Imm32(int32_t(mask))
    { }
};

struct ImmType : public ImmTag
{
    ImmType(JSValueType type)
      : ImmTag(JSVAL_TYPE_TO_TAG(type))
    { }
};

// nunbox32: type in r3, payload in r4
static const ValueOperand JSReturnOperand = ValueOperand(JSReturnReg_Type, JSReturnReg_Data);

// PPC32 is big-endian: type at lower address (offset 0), payload at offset 4
static const int32_t PAYLOAD_OFFSET = NUNBOX32_PAYLOAD_OFFSET;
static const int32_t TAG_OFFSET = NUNBOX32_TYPE_OFFSET;

class MacroAssemblerPPC : public Assembler
{
  protected:
    MacroAssembler& asMasm();
    const MacroAssembler& asMasm() const;

  public:
    using Assembler::size;

    // =====================================================================
    // Immediate loading
    // =====================================================================
    void ma_li(Register dest, Imm32 imm);
    void ma_li(Register dest, ImmWord imm);
    void ma_li(Register dest, ImmGCPtr ptr);
    void ma_li(Register dest, ImmPtr ptr);
    void ma_li(Register dest, CodeOffset* label);
    void ma_liPatchable(Register dest, Imm32 imm);
    void ma_liPatchable(Register dest, ImmPtr imm);
    void ma_liPatchable(Register dest, ImmWord imm);

    // =====================================================================
    // Register moves
    // =====================================================================
    void ma_move(Register rd, Register rs);

    // =====================================================================
    // Arithmetic
    // =====================================================================
    void ma_addu(Register rd, Register rs, Register rt);
    void ma_addu(Register rd, Register rs, Imm32 imm);
    void ma_addu(Register rd, Imm32 imm);
    void ma_subu(Register rd, Register rs, Register rt);
    void ma_subu(Register rd, Register rs, Imm32 imm);
    void ma_subu(Register rd, Imm32 imm);
    void ma_negu(Register rd, Register rs);
    void ma_mul(Register rd, Register rs, Register rt);
    void ma_mul(Register rd, Register rs, Imm32 imm);

    template <typename L>
    void ma_addTestOverflow(Register rd, Register rs, Register rt, L overflow);
    template <typename L>
    void ma_addTestOverflow(Register rd, Register rs, Imm32 imm, L overflow);
    void ma_subTestOverflow(Register rd, Register rs, Register rt, Label* overflow);

    // =====================================================================
    // Logic
    // =====================================================================
    void ma_and(Register rd, Register rs);
    void ma_and(Register rd, Imm32 imm);
    void ma_and(Register rd, Register rs, Imm32 imm);
    void ma_or(Register rd, Register rs);
    void ma_or(Register rd, Imm32 imm);
    void ma_or(Register rd, Register rs, Imm32 imm);
    void ma_xor(Register rd, Register rs);
    void ma_xor(Register rd, Imm32 imm);
    void ma_xor(Register rd, Register rs, Imm32 imm);
    void ma_not(Register rd, Register rs);

    // =====================================================================
    // Shifts
    // =====================================================================
    void ma_sll(Register rd, Register rt, Imm32 shift);
    void ma_srl(Register rd, Register rt, Imm32 shift);
    void ma_sra(Register rd, Register rt, Imm32 shift);
    void ma_sll(Register rd, Register rt, Register shift);
    void ma_srl(Register rd, Register rt, Register shift);
    void ma_sra(Register rd, Register rt, Register shift);
    void ma_rol(Register rd, Register rt, Imm32 shift);
    void ma_ror(Register rd, Register rt, Imm32 shift);

    // =====================================================================
    // Compare and set
    // =====================================================================
    void ma_cmp_set(Register dest, Register lhs, Register rhs, Condition c);
    void ma_cmp_set(Register dest, Register lhs, Imm32 imm, Condition c);
    void ma_cmp_set(Register dest, Register lhs, ImmPtr imm, Condition c);
    void ma_cmp_set(Register dest, Address lhs, Imm32 imm, Condition c);
    void ma_cmp_set(Register dest, Address lhs, ImmPtr imm, Condition c);

    // =====================================================================
    // Load/store
    // =====================================================================
    void ma_load(Register dest, Address address, LoadStoreSize size = SizeWord,
                 LoadStoreExtension extension = SignExtend);
    void ma_load(Register dest, const BaseIndex& src, LoadStoreSize size = SizeWord,
                 LoadStoreExtension extension = SignExtend);
    void ma_store(Register data, Address address, LoadStoreSize size = SizeWord,
                  LoadStoreExtension extension = SignExtend);
    void ma_store(Register data, const BaseIndex& dest, LoadStoreSize size = SizeWord,
                  LoadStoreExtension extension = SignExtend);
    void ma_store(Imm32 imm, Address address, LoadStoreSize size = SizeWord,
                  LoadStoreExtension extension = SignExtend);

    void ma_lw(Register data, Address address);
    void ma_sw(Register data, Address address);
    void ma_sw(Imm32 imm, Address address);
    void ma_sw(Register data, BaseIndex& address);

    // =====================================================================
    // Stack operations
    // =====================================================================
    void ma_pop(Register r);
    void ma_push(Register r);

    // =====================================================================
    // FP operations
    // =====================================================================
    void ma_ss(FloatRegister src, Address address);
    void ma_ss(FloatRegister src, BaseIndex address);
    void ma_sd(FloatRegister src, Address address);
    void ma_sd(FloatRegister src, BaseIndex address);
    void ma_ls(FloatRegister dest, Address address);
    void ma_ls(FloatRegister dest, BaseIndex address);
    void ma_ld(FloatRegister dest, Address address);
    void ma_ld(FloatRegister dest, BaseIndex address);

    // =====================================================================
    // Branches
    // =====================================================================
    void ma_b(Label* label);
    void ma_bl(Label* label);
    void ma_b(Register lhs, Register rhs, Label* label, Condition c);
    void ma_b(Register lhs, Imm32 imm, Label* label, Condition c);
    void ma_b(Register lhs, ImmPtr imm, Label* label, Condition c);
    void ma_b(Register lhs, ImmGCPtr imm, Label* label, Condition c);
    void ma_b(Register lhs, ImmWord imm, Label* label, Condition c);
    void ma_b(Address addr, Imm32 imm, Label* label, Condition c);
    void ma_b(Label* label, Condition c);
    template <typename T>
    void ma_b(Register lhs, T rhs, wasm::TrapDesc target, Condition c) {
        MOZ_CRASH("PPC wasm not supported");
    }
    void ma_b(wasm::TrapDesc target) {
        MOZ_CRASH("PPC wasm not supported");
    }

    // =====================================================================
    // SPR access helpers
    // =====================================================================
    void ma_mflr(Register rt) { as_mflr(rt); }
    void ma_mtlr(Register rs) { as_mtlr(rs); }
    void ma_mtctr(Register rs) { as_mtctr(rs); }
    void ma_bctr() { as_bctr(); }
    void ma_bctrl() { as_bctrl(); }
    void ma_blr() { as_blr(); }

    // =====================================================================
    // Address computation
    // =====================================================================
    void computeScaledAddress(const BaseIndex& address, Register dest);
    void computeEffectiveAddress(const Address& address, Register dest);

    // =====================================================================
    // FP branches
    // =====================================================================
    void ma_bc(FloatFormat fmt, DoubleCondition cond, FloatRegister lhs,
               FloatRegister rhs, Label* label);

    // =====================================================================
    // Emit a far branch sequence (lis+ori+mtctr+bctr = 16 bytes)
    // =====================================================================
    void ma_farBranch(Register scratch, const void* target);
    void ma_call(Register scratch, const void* target);
};

class MacroAssemblerPPCCompat : public MacroAssemblerPPC
{
    uint32_t savedFramePushed_;
  public:
    MacroAssemblerPPCCompat() : savedFramePushed_(0) { }

    // =====================================================================
    // Value operations (nunbox32)
    // =====================================================================
    void moveValue(const ValueOperand& src, const ValueOperand& dest);
    void moveValue(const Value& src, const ValueOperand& dest);

    void boxValue(JSValueType type, Register src, const ValueOperand& dest);
    void tagValue(JSValueType type, Register payload, ValueOperand dest);

    void pushValue(const ValueOperand& val);
    void pushValue(const Value& val);
    void pushValue(JSValueType type, Register reg);
    void pushValue(const Address& addr);

    void popValue(ValueOperand val);

    void storeValue(const ValueOperand& val, const Address& dest);
    void storeValue(const ValueOperand& val, const BaseIndex& dest);
    void storeValue(const Value& val, const Address& dest);
    void storeValue(JSValueType type, Register reg, const Address& dest);
    void storeValue(JSValueType type, Register reg, const BaseIndex& dest);

    void loadValue(const Address& src, const ValueOperand& dest);
    void loadValue(const BaseIndex& src, const ValueOperand& dest);

    // =====================================================================
    // Type testing
    // =====================================================================
    void branchTestValue(Condition cond, const ValueOperand& lhs, const Value& rhs, Label* label);

    template <typename T>
    void branchTestGCThing(Condition cond, T t, Label* label);

    Register extractTag(const Address& address, Register scratch);
    Register extractTag(const BaseIndex& address, Register scratch);
    Register extractTag(const ValueOperand& value, Register scratch);
    Register extractObject(const Address& address, Register scratch);
    Register extractObject(const ValueOperand& value, Register scratch);

    void unboxInt32(const ValueOperand& src, Register dest);
    void unboxInt32(const Address& src, Register dest);
    void unboxBoolean(const ValueOperand& src, Register dest);
    void unboxBoolean(const Address& src, Register dest);
    void unboxDouble(const ValueOperand& src, FloatRegister dest);
    void unboxDouble(const Address& src, FloatRegister dest);
    void unboxString(const ValueOperand& src, Register dest);
    void unboxString(const Address& src, Register dest);
    void unboxSymbol(const ValueOperand& src, Register dest);
    void unboxSymbol(const Address& src, Register dest);
    void unboxObject(const ValueOperand& src, Register dest);
    void unboxObject(const Address& src, Register dest);
    void unboxValue(const ValueOperand& src, AnyRegister dest);
    void unboxNonDouble(const ValueOperand& src, Register dest);
    void notBoolean(const ValueOperand& val) {
        as_xori(val.payloadReg(), val.payloadReg(), 1);
    }
    void boxDouble(FloatRegister src, const ValueOperand& dest);
    void boxNonDouble(JSValueType type, Register src, const ValueOperand& dest);
    Register extractInt32(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    Register extractBoolean(const ValueOperand& value, Register scratch) {
        return value.payloadReg();
    }
    void incrementInt32Value(const Address& addr) { add32(Imm32(1), ToPayload(addr)); }
    void pushReturnAddress() {
        // PPC: LR is an SPR, must mflr to GPR then push.
        // Use lowercase push() to NOT adjust framePushed_ (like MIPS).
        // framePushed_ should only track the Ion frame, not the return address.
        as_mflr(ScratchRegister);
        push(ScratchRegister);
    }

    // =====================================================================
    // Type conversions
    // =====================================================================
    void convertBoolToInt32(Register src, Register dest);
    void convertInt32ToDouble(Register src, FloatRegister dest);
    void convertInt32ToDouble(const Address& src, FloatRegister dest);
    void convertInt32ToDouble(const BaseIndex& src, FloatRegister dest) {
        load32(src, ScratchRegister);
        convertInt32ToDouble(ScratchRegister, dest);
    }
    void convertInt32ToFloat32(Register src, FloatRegister dest);
    void convertInt32ToFloat32(const Address& src, FloatRegister dest);
    void convertUInt32ToDouble(Register src, FloatRegister dest);
    void convertUInt32ToFloat32(Register src, FloatRegister dest);
    void convertDoubleToFloat32(FloatRegister src, FloatRegister dest);
    void convertDoubleToInt32(FloatRegister src, Register dest, Label* fail, bool negativeZeroCheck);
    void convertFloat32ToInt32(FloatRegister src, Register dest, Label* fail, bool negativeZeroCheck);
    void convertFloat32ToDouble(FloatRegister src, FloatRegister dest);

    // =====================================================================
    // Memory/stack
    // =====================================================================
    void addPtr(Register src, Register dest);
    void addPtr(Imm32 imm, Register dest);
    void addPtr(ImmWord imm, Register dest);
    void addPtr(Imm32 imm, const Address& dest);
    void subPtr(Register src, Register dest);
    void subPtr(Imm32 imm, Register dest);
    void addToStackPtr(Imm32 imm);

    void movePtr(Register src, Register dest);
    void movePtr(ImmWord imm, Register dest);
    void movePtr(ImmPtr imm, Register dest);
    void movePtr(ImmGCPtr imm, Register dest);

    void loadPtr(const Address& address, Register dest);
    void loadPtr(const BaseIndex& address, Register dest);
    void loadPtr(AbsoluteAddress address, Register dest);
    void storePtr(Register src, const Address& address);
    void storePtr(Register src, const BaseIndex& address);
    void storePtr(ImmWord imm, const Address& address);
    void storePtr(ImmPtr imm, const Address& address);
    void storePtr(ImmGCPtr imm, const Address& address);

    void load8ZeroExtend(const Address& src, Register dest);
    void load8SignExtend(const Address& src, Register dest);
    void load16ZeroExtend(const Address& src, Register dest);
    void load16SignExtend(const Address& src, Register dest);
    void load32(const Address& address, Register dest);
    void load32(const BaseIndex& address, Register dest);
    void store8(Register src, const Address& address);
    void store16(Register src, const Address& address);
    void store32(Register src, const Address& address);
    void store32(Imm32 imm, const Address& address);
    void store32(Register src, const BaseIndex& address);

    void push(Register reg);
    void push(Imm32 imm);
    void push(ImmWord imm);
    void push(const Address& addr);
    void pop(Register reg);
    void pop(const Address& addr) {
        Pop(ScratchRegister);
        storePtr(ScratchRegister, addr);
    }

    CodeOffset pushWithPatch(ImmWord imm);

    // =====================================================================
    // Branches
    // =====================================================================
    void branch(JitCode* target);
    void branch(Register target);
    void branch(const Address& addr);

    void branch32(Condition cond, Register lhs, Register rhs, Label* label);
    void branch32(Condition cond, Register lhs, Imm32 imm, Label* label);
    void branch32(Condition cond, const Address& lhs, Register rhs, Label* label);
    void branch32(Condition cond, const Address& lhs, Imm32 imm, Label* label);
    void branch32(Condition cond, const BaseIndex& lhs, Imm32 imm, Label* label);

    void branchPtr(Condition cond, Register lhs, Register rhs, Label* label);
    void branchPtr(Condition cond, Register lhs, ImmPtr imm, Label* label);
    void branchPtr(Condition cond, Register lhs, ImmWord imm, Label* label);
    void branchPtr(Condition cond, Register lhs, ImmGCPtr imm, Label* label);
    void branchPtr(Condition cond, const Address& lhs, Register rhs, Label* label);
    void branchPtr(Condition cond, const Address& lhs, ImmPtr imm, Label* label);
    void branchPtr(Condition cond, const Address& lhs, ImmWord imm, Label* label);
    void branchPtr(Condition cond, const BaseIndex& lhs, ImmWord imm, Label* label);

    void branchTest32(Condition cond, Register lhs, Register rhs, Label* label);
    void branchTest32(Condition cond, Register lhs, Imm32 imm, Label* label);
    void branchTest32(Condition cond, const Address& lhs, Imm32 imm, Label* label);

    void branchTestPtr(Condition cond, Register lhs, Register rhs, Label* label);

    // =====================================================================
    // Calls
    // =====================================================================
    void call(Register reg);
    void call(Label* label);
    void call(JitCode* code);
    void call(ImmWord imm);
    void call(ImmPtr imm);
    void call(const Address& addr);
    CodeOffset callWithPatch();

    // =====================================================================
    // Frame management
    // =====================================================================
    void makeFrameDescriptor(Register frameSizeReg, FrameType type);
    void linkExitFrame(Register cxReg, Register scratch);
    void handleFailureWithHandlerTail(void* handler);

    // =====================================================================
    // Tests
    // =====================================================================
    void testNullSet(Condition cond, const ValueOperand& value, Register dest);
    void testUndefinedSet(Condition cond, const ValueOperand& value, Register dest);
    void testObjectSet(Condition cond, const ValueOperand& value, Register dest);

    // =====================================================================
    // Profiling
    // =====================================================================
    CodeOffset toggledJump(Label* label);
    CodeOffset toggledCall(JitCode* target, bool enabled);

    void profilerEnterFrame(Register framePtr, Register scratch);
    void profilerExitFrame();

    // =====================================================================
    // GC barriers
    // =====================================================================
    // patchableCallPreBarrier: use shared template from MacroAssembler.h

    // =====================================================================
    // ICache flushing
    // =====================================================================
    void flushICacheForRange(Register start, Register length);

    // =====================================================================
    // Missing framework methods
    // =====================================================================
    
    // mov variants (used inline in MacroAssembler.h)
    void mov(Register src, Register dest) {
        ma_move(dest, src);
    }
    void mov(ImmWord imm, Register dest) {
        ma_li(dest, imm);
    }
    void mov(ImmPtr imm, Register dest) {
        ma_li(dest, imm);
    }
    void mov(Register src, Address dest) {
        ma_sw(src, dest);
    }
    void mov(Address src, Register dest) {
        ma_lw(dest, src);
    }

    // move32
    void move32(Imm32 imm, Register dest) {
        ma_li(dest, imm);
    }
    void move32(Register src, Register dest) {
        ma_move(dest, src);
    }

    // movePtr with wasm::SymbolicAddress
    void movePtr(wasm::SymbolicAddress imm, Register dest);
    
    // jump/bind/nop/ret
    void jump(Label* label) {
        ma_b(label);
    }
    void jump(Register reg) {
        as_mtctr(reg);
        as_bctr();
    }
    void jump(JitCode* target) {
        branch(target);
    }
    void jump(const Address& addr) {
        branch(addr);
    }
    void jump(wasm::TrapDesc target) {
        MOZ_CRASH("PPC: NYI jump(TrapDesc)");
    }
    void haltingAlign(int alignment);
    void nop() {
        // ori r0, r0, 0 = PPC nop
        as_nop();
    }
    void ret() {
        as_blr();
    }
    void retn(Imm32 n);
    void bind(Label* label);
    void bind(RepatchLabel* label);
    using Assembler::bind;  // bring in bind(CodeOffset*) from base

    // breakpoint
    void breakpoint();
    void checkStackAlignment();
    void alignStackPointer();
    void restoreStackPointer();

    // Push/Pop (capital P = adjust frame)
    void Push(Register reg);
    void Push(Imm32 imm);
    void Push(ImmWord imm);
    void Push(ImmPtr imm);
    void Push(ImmGCPtr imm);
    void Push(const Address& addr);
    void Push(FloatRegister reg);
    void Pop(Register reg);
    void Pop(FloatRegister reg);

    // Float movement
    void moveDouble(FloatRegister src, FloatRegister dest) {
        as_fmr(dest, src);
    }
    void moveFloat32(FloatRegister src, FloatRegister dest) {
        as_fmr(dest, src);
    }
    void zeroDouble(FloatRegister reg) {
        // fsub reg, reg, reg -> +0.0
        as_fsub(reg, reg, reg);
    }

    // Float loads
    void loadDouble(const Address& addr, FloatRegister dest);
    void loadDouble(const BaseIndex& src, FloatRegister dest);
    void loadFloat32(const Address& addr, FloatRegister dest);
    void loadFloat32(const BaseIndex& src, FloatRegister dest);
    void loadFloatAsDouble(const Address& addr, FloatRegister dest);
    void loadFloatAsDouble(const BaseIndex& src, FloatRegister dest);

    // Float stores
    void storeDouble(FloatRegister src, const Address& addr);
    void storeDouble(FloatRegister src, const BaseIndex& addr);
    void storeFloat32(FloatRegister src, const Address& addr);
    void storeFloat32(FloatRegister src, const BaseIndex& addr);

    // Float comparisons
    void branchDouble(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs, Label* label);
    void branchFloat(DoubleCondition cond, FloatRegister lhs, FloatRegister rhs, Label* label);

    // Value-to-float helpers
    void loadConstantDouble(double d, FloatRegister dest);
    void loadConstantDouble(wasm::RawF64 d, FloatRegister dest) { loadConstantDouble(d.fp(), dest); }
    void loadConstantFloat32(float f, FloatRegister dest);
    void loadConstantFloat32(wasm::RawF32 f, FloatRegister dest) { loadConstantFloat32(f.fp(), dest); }
    void boolValueToDouble(const ValueOperand& operand, FloatRegister dest);
    void boolValueToFloat32(const ValueOperand& operand, FloatRegister dest);
    void int32ValueToDouble(const ValueOperand& operand, FloatRegister dest);
    void int32ValueToFloat32(const ValueOperand& operand, FloatRegister dest);

    // ensureDouble
    void ensureDouble(const ValueOperand& source, FloatRegister dest, Label* failure);
    void ensureDouble(const Address& source, FloatRegister dest, Label* failure);

    // load/store private
    void loadPrivate(const Address& address, Register dest);

    // load/store with various sizes from BaseIndex
    void load8ZeroExtend(const BaseIndex& src, Register dest);
    void load8SignExtend(const BaseIndex& src, Register dest);
    void load16ZeroExtend(const BaseIndex& src, Register dest);
    void load16SignExtend(const BaseIndex& src, Register dest);
    void load32(AbsoluteAddress address, Register dest);
    void store8(Register src, const BaseIndex& address);
    void store8(Imm32 imm, const Address& address);
    void store8(Imm32 imm, const BaseIndex& address);
    void store16(Register src, const BaseIndex& address);
    void store16(Imm32 imm, const Address& address);
    void store16(Imm32 imm, const BaseIndex& address);
    void store32(Imm32 src, const BaseIndex& address);
    void store32(Register src, AbsoluteAddress address);
    void storePtr(Register src, AbsoluteAddress dest);

    // push/pop with FloatRegister
    void push(FloatRegister reg);
    void pop(FloatRegister reg);

    // movWithPatch
    CodeOffset movWithPatch(ImmWord imm, Register dest) {
        CodeOffset label = CodeOffset(nextOffset().getOffset());
        ma_liPatchable(dest, imm);
        return label;
    }
    CodeOffset movWithPatch(ImmPtr imm, Register dest) {
        return movWithPatch(ImmWord(uintptr_t(imm.value)), dest);
    }

    // lea
    void lea(Operand addr, Register dest) {
        ma_addu(dest, addr.baseReg(), Imm32(addr.disp()));
    }

    // abiret
    void abiret() {
        as_blr();
    }

    // computeScaledAddress / computeEffectiveAddress
    void computeScaledAddress(const BaseIndex& address, Register dest);
    void computeEffectiveAddress(const Address& address, Register dest) {
        ma_addu(dest, address.base, Imm32(address.offset));
    }
    void computeEffectiveAddress(const BaseIndex& address, Register dest);

    // Atomics stubs (MOZ_CRASH for now)
    template <typename T>
    void compareExchangeToTypedIntArray(Scalar::Type arrayType, const T& mem, 
                                        Register oldval, Register newval,
                                        Register temp, AnyRegister output) {
        MOZ_CRASH("PPC compareExchangeToTypedIntArray");
    }
    template <typename T>
    void atomicExchangeToTypedIntArray(Scalar::Type arrayType, const T& mem,
                                       Register value, Register temp,
                                       AnyRegister output) {
        MOZ_CRASH("PPC atomicExchangeToTypedIntArray");
    }


    // add32/sub32 to addresses
    void add32(Register src, Register dest) {
        ma_addu(dest, dest, src);
    }
    void add32(Imm32 imm, Register dest) {
        ma_addu(dest, dest, imm);
    }
    void add32(Imm32 imm, const Address& dest);
    void sub32(Register src, Register dest) {
        ma_subu(dest, dest, src);
    }
    void sub32(Imm32 imm, Register dest) {
        ma_subu(dest, dest, imm);
    }

    // 64-bit loads/stores
    void load64(const Address& address, Register64 dest) {
        load32(Address(address.base, address.offset + 0), dest.high);
        load32(Address(address.base, address.offset + 4), dest.low);
    }
    void store64(Register64 src, Address address) {
        store32(src.high, Address(address.base, address.offset + 0));
        store32(src.low, Address(address.base, address.offset + 4));
    }
    void store64(Imm64 imm, Address address) {
        store32(Imm32(imm.hi()), Address(address.base, address.offset + 0));
        store32(Imm32(imm.low()), Address(address.base, address.offset + 4));
    }

    // wasm
    void loadWasmGlobalPtr(uint32_t globalDataOffset, Register dest) {
        loadPtr(Address(WasmTlsReg, globalDataOffset), dest);
    }
    void loadWasmPinnedRegsFromTls() {
        MOZ_CRASH("PPC loadWasmPinnedRegsFromTls");
    }

    // misc
    bool buildOOLFakeExitFrame(void* fakeReturnAddr);
    CodeOffset labelForPatch() {
        return CodeOffset(nextOffset().getOffset());
    }

    // branchTestPtr
    void branchTestPtr(Condition cond, Register lhs, Imm32 imm, Label* label) {
        branchTest32(cond, lhs, imm, label);
    }

    // branchPtr with AbsoluteAddress
    void branchPtr(Condition cond, AbsoluteAddress lhs, Register rhs, Label* label);
    void branchPtr(Condition cond, AbsoluteAddress lhs, ImmWord imm, Label* label);

    // branch32 with AbsoluteAddress
    void branch32(Condition cond, AbsoluteAddress lhs, Register rhs, Label* label);
    void branch32(Condition cond, AbsoluteAddress lhs, Imm32 imm, Label* label);

    // branchDouble/Float with wasm
    void branchDoubleNotInInt64Range(Address src, Register temp, Label* fail);
    void branchDoubleNotInUInt64Range(Address src, Register temp, Label* fail);
    void branchFloat32NotInInt64Range(Address src, Register temp, Label* fail);
    void branchFloat32NotInUInt64Range(Address src, Register temp, Label* fail);

    // storeValue to AbsoluteAddress
    void storeValue(const ValueOperand& val, AbsoluteAddress dest);


    // test32
    void test32(Register lhs, Register rhs) {
        as_and(ScratchRegister, lhs, rhs);
    }
    void test32(Register lhs, Imm32 imm) {
        ma_and(ScratchRegister, lhs, imm);
    }
    void test32(const Address& addr, Imm32 imm);

    // xor32
    void xor32(Register src, Register dest) {
        ma_xor(dest, src);
    }
    void xor32(Imm32 imm, Register dest) {
        ma_xor(dest, imm);
    }

    // or32
    void or32(Register src, Register dest) {
        ma_or(dest, src);
    }
    void or32(Imm32 imm, Register dest) {
        ma_or(dest, imm);
    }

    // and32
    void and32(Register src, Register dest) {
        ma_and(dest, src);
    }
    void and32(Imm32 imm, Register dest) {
        ma_and(dest, imm);
    }

    // neg32
    void neg32(Register reg) {
        ma_negu(reg, reg);
    }

    // not32
    void not32(Register reg) {
        ma_not(reg, reg);
    }

    // lshift/rshift
    void lshift32(Imm32 imm, Register dest) {
        ma_sll(dest, dest, imm);
    }
    void rshift32(Imm32 imm, Register dest) {
        ma_sra(dest, dest, imm);
    }
    void rshift32Arithmetic(Imm32 imm, Register dest) {
        ma_sra(dest, dest, imm);
    }
    void rshiftPtr(Imm32 imm, Register dest) {
        ma_srl(dest, dest, imm);
    }
    void lshiftPtr(Imm32 imm, Register dest) {
        ma_sll(dest, dest, imm);
    }

    // Conditional moves
    void cmp32Move32(Condition cond, Register lhs, Register rhs, Register src, Register dest);
    void cmp32Move32(Condition cond, Register lhs, const Address& rhs, Register src, Register dest);
    
    // Float conditional
    void branchTestDoubleTruthy(bool truthy, FloatRegister reg, Label* label);
    void branchTestBooleanTruthy(bool truthy, const ValueOperand& val, Label* label);

    // wasm traps
    void wasmTruncateDoubleToInt32(FloatRegister input, Register output, Label* oolEntry);
    void wasmTruncateFloat32ToInt32(FloatRegister input, Register output, Label* oolEntry);
    void wasmTruncateDoubleToUInt32(FloatRegister input, Register output, Label* oolEntry);
    void wasmTruncateFloat32ToUInt32(FloatRegister input, Register output, Label* oolEntry);

    // convertUInt64ToDouble
    void convertUInt64ToDouble(Register64 src, FloatRegister dest, Register temp);

    // branch with condition
    void branchTest32(Condition cond, AbsoluteAddress lhs, Imm32 imm, Label* label);

    // Returns the register containing the type tag.
    Register splitTagForTest(const ValueOperand& value) {
        return value.typeReg();
    }

    void loadUnboxedValue(Address address, MIRType type, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(address, dest.fpu());
        else
            ma_lw(dest.gpr(), Address(address.base, address.offset + NUNBOX32_PAYLOAD_OFFSET));
    }

    void loadUnboxedValue(BaseIndex address, MIRType type, AnyRegister dest) {
        if (dest.isFloat())
            loadInt32OrDouble(address.base, address.index, dest.fpu(), address.scale);
        else
            load32(BaseIndex(address.base, address.index, address.scale,
                             address.offset + NUNBOX32_PAYLOAD_OFFSET), dest.gpr());
    }

    template <typename T>
    void storeUnboxedPayload(ValueOperand value, T address, size_t nbytes) {
        switch (nbytes) {
          case 4:
            store32(value.payloadReg(), address);
            return;
          case 1:
            store8(value.payloadReg(), address);
            return;
          default: MOZ_CRASH("Bad payload width");
        }
    }

    CodeOffsetJump backedgeJump(RepatchLabel* label, Label* documentation = nullptr) {
        // Emit a 4-instruction far jump: lis r12,hi; ori r12,r12,lo; mtctr r12; bctr
        // PatchBackedge will later patch this to a short branch if possible.
        BufferOffset bo = nextOffset();
        label->use(bo.getOffset());
        uint32_t target = label->bound() ? label->target() : 0;
        as_li32(ScratchRegister, target);
        as_mtctr(ScratchRegister);
        as_bctr();
        return CodeOffsetJump(bo.getOffset());
    }
    CodeOffsetJump jumpWithPatch(RepatchLabel* label, Condition cond, Label* documentation) {
        // Conditional patchable jump: if (!cond) skip; far_jump target
        // Emit inverse conditional branch over the far jump
        uint32_t bo, bi;
        switch (cond) {
          case Equal: case Zero: bo = BO_FALSE; bi = CR_EQ; break;
          case NotEqual: case NonZero: bo = BO_TRUE; bi = CR_EQ; break;
          case LessThan: bo = BO_FALSE; bi = CR_LT; break;
          case GreaterThanOrEqual: bo = BO_TRUE; bi = CR_LT; break;
          case GreaterThan: bo = BO_FALSE; bi = CR_GT; break;
          case LessThanOrEqual: bo = BO_TRUE; bi = CR_GT; break;
          case Below: bo = BO_FALSE; bi = CR_LT; break;
          case AboveOrEqual: bo = BO_TRUE; bi = CR_LT; break;
          case Above: bo = BO_FALSE; bi = CR_GT; break;
          case BelowOrEqual: bo = BO_TRUE; bi = CR_GT; break;
          default: MOZ_CRASH("Unexpected condition");
        }
        // Skip the far jump (4 instructions = 16 bytes + this bc = skip 20 total, but bc skips from next insn)
        as_bc(bo, bi, 4 * 4); // skip over lis+ori+mtctr+bctr
        BufferOffset bo2 = nextOffset();
        label->use(bo2.getOffset());
        uint32_t target = label->bound() ? label->target() : 0;
        as_li32(ScratchRegister, target);
        as_mtctr(ScratchRegister);
        as_bctr();
        return CodeOffsetJump(bo2.getOffset());
    }
    CodeOffsetJump jumpWithPatch(RepatchLabel* label, Label* documentation = nullptr) {
        BufferOffset bo = nextOffset();
        label->use(bo.getOffset());
        uint32_t target = label->bound() ? label->target() : 0;
        as_li32(ScratchRegister, target);
        as_mtctr(ScratchRegister);
        as_bctr();
        return CodeOffsetJump(bo.getOffset());
    }

    template <typename T>
    void storeUnboxedValue(ConstantOrRegister value, MIRType valueType, const T& dest,
                           MIRType slotType) {
        if (valueType == MIRType::Double) {
            storeDouble(value.reg().typedReg().fpu(), dest);
            return;
        }
        if (valueType != slotType)
            store32(ImmType(ValueTypeFromMIRType(valueType)), ToType(dest));
        if (value.constant())
            storeValue(value.value(), dest);
        else
            store32(value.reg().typedReg().gpr(), ToPayload(dest));
    }
    // =====================================================================
    // Static constants
    // =====================================================================
    static bool SupportsFloatingPoint() { return true; }
    static bool SupportsUnalignedAccesses() { return false; }
    static bool SupportsSimd() { return false; }

    static size_t ToggledCallSize(uint8_t* code) { return 4 * sizeof(uint32_t); }

    void flushBuffer() {}
    void nopAlign(size_t alignment) {
        // Pad with nops to align to the given boundary
        while (currentOffset() % alignment)
            nop();
    }
    uint32_t labelToPatchOffset(CodeOffset label) { return label.offset(); }

    class AutoPrepareForPatching {
      public:
        explicit AutoPrepareForPatching(MacroAssembler&) {}
    };
    void setPrinter(Sprinter*) {}

    // RepatchLabel support
    void jump(RepatchLabel* label) {
        // Emit a jump that can be patched later
        // Use lis+ori+mtctr+bctr for patchability
        Label l;
        jump(&l);
        // Bind repatch label to allow later patching
        label->use(l.offset());
    }
    void writeCodePointer(CodeOffset* label) {
        // Write a placeholder pointer that will be patched later
        label->bind(currentOffset());
        writeInst(uint32_t(-1));  // placeholder
    }
    void popReturnAddress() {
        // Use lowercase pop() to NOT adjust framePushed_ (matches pushReturnAddress).
        pop(ScratchRegister);
        as_mtlr(ScratchRegister);
    }

    // storeValue overloads
    void storeValue(const Address& src, const Address& dest, Register temp) {
        // Copy tag and payload from src to dest
        load32(ToType(src), temp);
        store32(temp, ToType(dest));
        load32(ToPayload(src), temp);
        store32(temp, ToPayload(dest));
    }

    // storePtr with BaseIndex
    void storePtr(ImmGCPtr imm, const BaseIndex& dest) {
        ma_li(ScratchRegister, imm);
        storePtr(ScratchRegister, dest);
    }
    void storePtr(ImmWord imm, const BaseIndex& dest) {
        ma_li(ScratchRegister, Imm32(imm.value));
        storePtr(ScratchRegister, dest);
    }

    // storeValue with Value
    void storeValue(const Value& val, const BaseIndex& dest) {
        // Compute effective address, then store
        computeEffectiveAddress(dest, SecondScratchRegister);
        storeValue(val, Address(SecondScratchRegister, 0));
    }

    // unboxObject with BaseIndex
    void unboxObject(const BaseIndex& src, Register dest) {
        // Load payload from BaseIndex (payload at offset 4 on big-endian)
        BaseIndex payloadAddr(src.base, src.index, src.scale, src.offset + NUNBOX32_PAYLOAD_OFFSET);
        load32(payloadAddr, dest);
    }

    // convertUInt64ToDoubleNeedsTemp
    bool convertUInt64ToDoubleNeedsTemp() { return true; }

    // loadInt32OrDouble
    void loadInt32OrDouble(const Address& src, FloatRegister dest) {
        Label notInt, done;
        // Check if tag indicates int32
        branch32(Assembler::NotEqual, ToType(src), ImmType(JSVAL_TYPE_INT32), &notInt);
        // It's an int32 — convert to double
        convertInt32ToDouble(ToPayload(src), dest);
        jump(&done);
        bind(&notInt);
        // It's a double — load directly
        loadDouble(src, dest);
        bind(&done);
    }
    void loadInt32OrDouble(Register base, Register index, FloatRegister dest, int32_t shift) {
        Label notInt, done;
        // Compute scaled address, then use Address offsets for type/payload
        computeScaledAddress(BaseIndex(base, index, ShiftToScale(shift)), SecondScratchRegister);
        // Check type tag
        load32(Address(SecondScratchRegister, NUNBOX32_TYPE_OFFSET), ScratchRegister);
        branch32(Assembler::NotEqual, ScratchRegister, ImmType(JSVAL_TYPE_INT32), &notInt);
        // Load int32 payload and convert
        load32(Address(SecondScratchRegister, NUNBOX32_PAYLOAD_OFFSET), ScratchRegister);
        convertInt32ToDouble(ScratchRegister, dest);
        jump(&done);
        bind(&notInt);
        // Load as double
        loadDouble(Address(SecondScratchRegister, 0), dest);
        bind(&done);
    }

    // bindLater
    void bindLater(Label* label, wasm::TrapDesc desc) { MOZ_CRASH("PPC: NYI"); }

    // ToType/ToPayload (big-endian nunbox32)
    static Address ToType(const Address& addr) {
        return addr;
    }
    static Address ToPayload(const Address& addr) {
        return Address(addr.base, addr.offset + 4);
    }
    static BaseIndex ToType(const BaseIndex& bi) {
        return bi;
    }
    static BaseIndex ToPayload(const BaseIndex& bi) {
        return BaseIndex(bi.base, bi.index, bi.scale, bi.offset + 4);
    }

    // j() - conditional jump (like x86 j instruction)
    void j(Condition cond, Label* label) {
        ma_b(label, cond);
    }
};

typedef MacroAssemblerPPCCompat MacroAssemblerSpecific;

} // namespace jit
} // namespace js

#endif /* jit_ppc_MacroAssembler_ppc_h */
