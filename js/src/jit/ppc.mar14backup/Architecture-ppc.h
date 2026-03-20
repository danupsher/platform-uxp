/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ppc_Architecture_ppc_h
#define jit_ppc_Architecture_ppc_h

#include "mozilla/MathAlgorithms.h"

#include <limits.h>
#include <stdint.h>

#include "js/Utility.h"

namespace js {
namespace jit {
template <typename T>
class TypedRegisterSet;
} // namespace jit
} // namespace js


namespace js {
namespace jit {

// PPC32 uses nunboxing (separate type and payload words in a js::Value).
// On big-endian PPC, type tag is at the lower address (offset 0),
// payload at offset 4.
static const int32_t NUNBOX32_TYPE_OFFSET = 0;
static const int32_t NUNBOX32_PAYLOAD_OFFSET = 4;

// PPC32 Darwin ABI: 24-byte linkage area + 32-byte parameter save area = 56 bytes minimum.
// The caller must always allocate this space above the callee's SP.
static const uint32_t ShadowStackSpace = 56;

// Size of each bailout table entry (two instructions: li + b).
static const uint32_t BAILOUT_TABLE_ENTRY_SIZE = 2 * sizeof(uint32_t);

// How far forward/back can a jump go?
static const uint32_t JumpImmediateRange = UINT32_MAX;

// =========================================================================
// General Purpose Registers
// =========================================================================
//
// PPC32 SVR4 ABI register usage:
//   r0       - volatile, special (cannot be base in load/store)
//   r1       - stack pointer (callee-saved)
//   r2       - reserved (TOC pointer on PPC64, thread pointer on some PPC32)
//   r3-r4    - return values / first 2 args (volatile)
//   r5-r10   - argument registers (volatile)
//   r11-r12  - volatile (r11 = env pointer in some ABIs, r12 = branch target)
//   r13      - small data area pointer (reserved)
//   r14-r30  - callee-saved (non-volatile)
//   r31      - frame pointer or callee-saved

class Registers
{
  public:
    enum RegisterID {
        r0 = 0,
        r1,  // sp
        r2,  // reserved (TOC/thread)
        r3,  // return value / arg0
        r4,  // return value high / arg1
        r5,  // arg2
        r6,  // arg3
        r7,  // arg4
        r8,  // arg5
        r9,  // arg6
        r10, // arg7
        r11, // volatile (env pointer)
        r12, // volatile (branch target)
        r13, // reserved (SDA)
        r14, r15, r16, r17, r18, r19, r20,
        r21, r22, r23, r24, r25, r26, r27,
        r28, r29, r30,
        r31, // frame pointer
        // Aliases
        sp = r1,
        fp = r31,
        invalid_reg
    };
    typedef uint8_t Code;
    typedef RegisterID Encoding;

    union RegisterContent {
        uintptr_t r;
    };

    static const char* const RegNames[];
    static const char* GetName(Code code) {
        MOZ_ASSERT(code < Total);
        return RegNames[code];
    }
    static const char* GetName(Encoding i) {
        return GetName(Code(i));
    }
    static Code FromName(const char* name);

    static const Encoding StackPointer = sp;
    static const Encoding Invalid = invalid_reg;

    static const uint32_t Total = 32;
    static const uint32_t Allocatable = 16; // r14-r28 (15) + some volatile

    typedef uint32_t SetType;

    static const SetType AllMask = 0xffffffff;

    // Argument registers: r3-r10
    static const SetType ArgRegMask =
        (1 << r3) | (1 << r4) | (1 << r5) | (1 << r6) |
        (1 << r7) | (1 << r8) | (1 << r9) | (1 << r10);
    static const SetType SharedArgRegMask = ArgRegMask;

    // Volatile (caller-saved): r0, r3-r12
    static const SetType VolatileMask =
        (1 << r0) |
        (1 << r3)  | (1 << r4)  | (1 << r5)  | (1 << r6) |
        (1 << r7)  | (1 << r8)  | (1 << r9)  | (1 << r10) |
        (1 << r11) | (1 << r12);

    // Non-volatile (callee-saved): r14-r31 + LR (saved as part of prologue)
    static const SetType NonVolatileMask =
        (1 << r14) | (1 << r15) | (1 << r16) | (1 << r17) |
        (1 << r18) | (1 << r19) | (1 << r20) | (1 << r21) |
        (1 << r22) | (1 << r23) | (1 << r24) | (1 << r25) |
        (1 << r26) | (1 << r27) | (1 << r28) | (1 << r29) |
        (1 << r30) | (1 << r31);

    static const SetType WrapperMask = VolatileMask;

    // Non-allocatable: sp, reserved regs, scratch regs
    static const SetType NonAllocatableMask =
        (1 << r0)  | // r0 is special (can't be base reg)
        (1 << r1)  | // sp
        (1 << r2)  | // reserved (TOC/thread)
        (1 << r11) | // scratch1
        (1 << r12) | // scratch2
        (1 << r13) | // reserved (SDA)
        (1 << r18) | // ICStubReg — must not be clobbered by regalloc
        (1 << r19) | // argsBase in VMWrapper — clobbered by every VM call
        (1 << r29) | // BaselineFrameReg — must not be clobbered by regalloc
        (1 << r30) | // used by alignStackPointer/restoreStackPointer in VMWrapper
        (1 << r31);  // frame pointer

    static const SetType TempMask = VolatileMask & ~NonAllocatableMask;

    // Registers returned from a JS -> JS call.
    // nunbox32: type in r3, payload in r4
    static const SetType JSCallMask = (1 << r3) | (1 << r4);

    // Registers returned from a JS -> C call.
    static const SetType SharedCallMask = (1 << r3);
    static const SetType CallMask = (1 << r3);

    static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;

    static uint32_t SetSize(SetType x) {
        static_assert(sizeof(SetType) == 4, "SetType must be 32 bits");
        return mozilla::CountPopulation32(x);
    }
    static uint32_t FirstBit(SetType x) {
        return mozilla::CountTrailingZeroes32(x);
    }
    static uint32_t LastBit(SetType x) {
        return 31 - mozilla::CountLeadingZeroes32(x);
    }
};

typedef uint32_t PackedRegisterMask;

// =========================================================================
// Floating Point Registers
// =========================================================================
//
// PPC has 32 64-bit FPRs (f0-f31). Each can hold single or double.
// Unlike MIPS, there's no aliasing between single and double — the same
// register holds either format. This simplifies the register model.
//
// SVR4 ABI:
//   f0       - volatile scratch
//   f1-f8    - argument/return registers (volatile)
//   f9-f13   - volatile
//   f14-f31  - callee-saved (non-volatile)

class FloatRegisters
{
  public:
    enum FPRegisterID {
        f0 = 0,
        f1, f2, f3, f4, f5, f6, f7, f8,
        f9, f10, f11, f12, f13,
        f14, f15, f16, f17, f18, f19, f20,
        f21, f22, f23, f24, f25, f26, f27,
        f28, f29, f30, f31,
        invalid_freg
    };
    typedef FPRegisterID Code;
    typedef FPRegisterID Encoding;

    union RegisterContent {
        double d;
    };

    static const char* GetName(Code code) {
        static const char* const Names[] = {
            "f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",  "f7",
            "f8",  "f9",  "f10", "f11", "f12", "f13", "f14", "f15",
            "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23",
            "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31"
        };
        MOZ_ASSERT(code < Total);
        return Names[code];
    }

    static Code FromName(const char* name);

    static const Code Invalid = invalid_freg;

    // PPC FPRs are 64-bit, no single/double aliasing issue.
    // We model them as 64 entries (32 double + 32 single views) for
    // compatibility with the SpiderMonkey register allocator.
    static const uint32_t Total = 64;
    static const uint32_t TotalDouble = 32;
    static const uint32_t TotalSingle = 32;
    static const uint32_t TotalPhys = 32;
    static const uint32_t Allocatable = 44; // minus f0 (scratch) and non-alloc
    static const uint32_t RegisterIdLimit = 32;

    typedef uint64_t SetType;

    static const SetType AllSingleMask = (1ULL << 32) - 1;
    static const SetType AllDoubleMask = AllSingleMask << 32;
    static const SetType AllMask = AllDoubleMask | AllSingleMask;

    // Non-volatile: f14-f31
    static const SetType NonVolatileDoubleMask =
        ((1ULL << f14) | (1ULL << f15) | (1ULL << f16) | (1ULL << f17) |
         (1ULL << f18) | (1ULL << f19) | (1ULL << f20) | (1ULL << f21) |
         (1ULL << f22) | (1ULL << f23) | (1ULL << f24) | (1ULL << f25) |
         (1ULL << f26) | (1ULL << f27) | (1ULL << f28) | (1ULL << f29) |
         (1ULL << f30) | (1ULL << f31)) << 32;

    static const SetType NonVolatileMask =
        NonVolatileDoubleMask |
        (1ULL << f14) | (1ULL << f15) | (1ULL << f16) | (1ULL << f17) |
        (1ULL << f18) | (1ULL << f19) | (1ULL << f20) | (1ULL << f21) |
        (1ULL << f22) | (1ULL << f23) | (1ULL << f24) | (1ULL << f25) |
        (1ULL << f26) | (1ULL << f27) | (1ULL << f28) | (1ULL << f29) |
        (1ULL << f30) | (1ULL << f31);

    static const SetType VolatileMask = AllMask & ~NonVolatileMask;
    static const SetType VolatileDoubleMask = AllDoubleMask & ~NonVolatileDoubleMask;

    static const SetType WrapperMask = VolatileMask;

    // f0 is scratch (used by MacroAssembler), f13 is second scratch
    static const SetType NonAllocatableDoubleMask =
        ((1ULL << f0) | (1ULL << f13)) << 32;
    static const SetType NonAllocatableMask =
        NonAllocatableDoubleMask |
        (1ULL << f0) | (1ULL << f13);

    static const SetType TempMask = VolatileMask & ~NonAllocatableMask;
    static const SetType AllocatableMask = AllMask & ~NonAllocatableMask;
};

class FloatRegister
{
  public:
    enum RegType {
        Single = 0x0,
        Double = 0x1,
    };

    typedef FloatRegisters Codes;
    typedef Codes::Code Code;
    typedef Codes::Encoding Encoding;

  private:
    uint32_t code_ : 6;
    RegType kind_ : 1;

  public:
    constexpr FloatRegister(uint32_t code, RegType kind = Double)
      : code_(Code(code)), kind_(kind)
    { }
    constexpr FloatRegister()
      : code_(Code(FloatRegisters::invalid_freg)), kind_(Double)
    { }

    bool operator==(const FloatRegister& other) const {
        MOZ_ASSERT(!isInvalid());
        MOZ_ASSERT(!other.isInvalid());
        return kind_ == other.kind_ && code_ == other.code_;
    }
    bool operator!=(const FloatRegister& other) const {
        return other.kind_ != kind_ || code_ != other.code_;
    }
    bool equiv(const FloatRegister& other) const { return other.kind_ == kind_; }
    size_t size() const { return (kind_ == Double) ? 8 : 4; }
    bool isInvalid() const {
        return code_ == FloatRegisters::invalid_freg;
    }

    bool isSingle() const { return kind_ == Single; }
    bool isDouble() const { return kind_ == Double; }
    bool isSimd128() const { return false; }

    FloatRegister doubleOverlay(unsigned int which = 0) const {
        MOZ_ASSERT(which == 0);
        return FloatRegister(code_, Double);
    }
    FloatRegister singleOverlay(unsigned int which = 0) const {
        MOZ_ASSERT(which == 0);
        return FloatRegister(code_, Single);
    }
    FloatRegister asSingle() const { return singleOverlay(); }
    FloatRegister asDouble() const { return doubleOverlay(); }
    FloatRegister asSimd128() const { MOZ_CRASH("No SIMD"); }

    Code code() const {
        MOZ_ASSERT(!isInvalid());
        return Code(code_ | (kind_ << 5));
    }
    Encoding encoding() const {
        MOZ_ASSERT(!isInvalid());
        return Encoding(code_);
    }
    uint32_t id() const { return code_; }

    static FloatRegister FromCode(uint32_t i) {
        uint32_t code = i & 31;
        uint32_t kind = i >> 5;
        return FloatRegister(code, RegType(kind));
    }
    static FloatRegister FromIndex(uint32_t index, RegType kind) {
        return FloatRegister(index, kind);
    }

    bool volatile_() const {
        if (isDouble())
            return !!((1ULL << (code_ + 32)) & FloatRegisters::VolatileMask);
        return !!((1ULL << code_) & FloatRegisters::VolatileMask);
    }
    const char* name() const {
        return FloatRegisters::GetName(Encoding(code_));
    }
    bool aliases(const FloatRegister& other) {
        // On PPC, fn-single and fn-double alias (same physical register)
        return code_ == other.code_;
    }
    uint32_t numAliased() const { return 2; } // single + double view
    void aliased(uint32_t aliasIdx, FloatRegister* ret) {
        if (aliasIdx == 0) {
            *ret = *this;
            return;
        }
        MOZ_ASSERT(aliasIdx == 1);
        if (isDouble())
            *ret = singleOverlay();
        else
            *ret = doubleOverlay();
    }
    uint32_t numAlignedAliased() const { return 2; }
    void alignedAliased(uint32_t aliasIdx, FloatRegister* ret) {
        MOZ_ASSERT(aliasIdx < 2);
        if (aliasIdx == 0) {
            *ret = FloatRegister(code_, Double);
        } else {
            *ret = FloatRegister(code_, Single);
        }
    }

    FloatRegisters::SetType alignedOrDominatedAliasedSet() const {
        // Both single and double views of this register
        return (FloatRegisters::SetType(1) << code_) |
               (FloatRegisters::SetType(1) << (code_ + 32));
    }

    static Code FromName(const char* name) {
        return FloatRegisters::FromName(name);
    }

    typedef FloatRegisters::SetType SetType;

    static uint32_t SetSize(SetType x) {
        static_assert(sizeof(SetType) == 8, "SetType must be 64 bits");
        return mozilla::CountPopulation64(x);
    }
    static uint32_t FirstBit(SetType x) {
        return mozilla::CountTrailingZeroes64(x);
    }
    static uint32_t LastBit(SetType x) {
        return 63 - mozilla::CountLeadingZeroes64(x);
    }

    static TypedRegisterSet<FloatRegister> ReduceSetForPush(const TypedRegisterSet<FloatRegister>& s);
    static uint32_t GetPushSizeInBytes(const TypedRegisterSet<FloatRegister>& s);
    uint32_t getRegisterDumpOffsetInBytes();
};

// PPC SVR4 ABI passes floats in FPRs, not GPR pairs.
// But for nunbox32 JS values, we do pass type+payload in two GPRs.
#define JS_CODEGEN_REGISTER_PAIR 1

// PPC doesn't have double registers that can NOT be treated as float32.
inline bool hasUnaliasedDouble() { return false; }

// On PPC, single and double share the same register (no aliasing issue).
inline bool hasMultiAlias() { return false; }

} // namespace jit
} // namespace js

#endif /* jit_ppc_Architecture_ppc_h */
