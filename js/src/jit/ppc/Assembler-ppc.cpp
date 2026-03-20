/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ppc/Assembler-ppc.h"
#include <mach/mach.h>
#include "gc/Nursery.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jscompartment.h"
#include "jsutil.h"

#include "gc/Marking.h"
#include "jit/ExecutableAllocator.h"
#include "jit/JitCompartment.h"

using mozilla::DebugOnly;

using namespace js;
using namespace js::jit;

// Register name table
const char* const Registers::RegNames[] = {
    "r0",  "sp",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
    "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
    "r24", "r25", "r26", "r27", "r28", "r29", "r30", "fp"
};

Registers::Code
Registers::FromName(const char* name)
{
    for (size_t i = 0; i < Total; i++) {
        if (strcmp(GetName(Code(i)), name) == 0)
            return Code(i);
    }
    return invalid_reg;
}

FloatRegisters::Code
FloatRegisters::FromName(const char* name)
{
    for (size_t i = 0; i < TotalPhys; i++) {
        if (strcmp(GetName(Code(i)), name) == 0)
            return Code(i);
    }
    return invalid_freg;
}

FloatRegisterSet
FloatRegister::ReduceSetForPush(const FloatRegisterSet& s)
{
    // PPC FPRs are all 64-bit. Always save as doubles.
    SetType bits = s.bits();
    // Move any single-precision bits into the double range
    SetType singles = bits & FloatRegisters::AllSingleMask;
    SetType doubles = bits & FloatRegisters::AllDoubleMask;
    // Merge singles into doubles position
    doubles |= (singles << 32);
    return FloatRegisterSet(doubles);
}

uint32_t
FloatRegister::GetPushSizeInBytes(const FloatRegisterSet& s)
{
    return s.size() * sizeof(double);
}

uint32_t
FloatRegister::getRegisterDumpOffsetInBytes()
{
    // In a register dump, FPRs are stored after GPRs.
    // Each FPR is 8 bytes (double).
    return id() * sizeof(double);
}

// =========================================================================
// ABIArgGenerator
// =========================================================================

ABIArgGenerator::ABIArgGenerator()
  : usedGPRs_(0),
    usedFPRs_(0),
    stackOffset_(ShadowStackSpace),
    current_()
{ }

ABIArg
ABIArgGenerator::next(MIRType type)
{
    switch (type) {
      case MIRType::Int32:
      case MIRType::Pointer:
        if (usedGPRs_ < 8) {
            current_ = ABIArg(Register::FromCode(Registers::r3 + usedGPRs_));
            usedGPRs_++;
        } else {
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(intptr_t);
        }
        break;
      case MIRType::Int64:
        // Align to even GPR pair
        usedGPRs_ += usedGPRs_ % 2;
        if (usedGPRs_ + 1 < 8) {
            // PPC32 big-endian: high word in lower register
            current_ = ABIArg(Register::FromCode(Registers::r3 + usedGPRs_),
                              Register::FromCode(Registers::r3 + usedGPRs_ + 1));
            usedGPRs_ += 2;
        } else {
            usedGPRs_ = 8;
            stackOffset_ += stackOffset_ % 8;
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(int64_t);
        }
        break;
      case MIRType::Float32:
        // PPC SVR4 ABI: floats go in FPRs f1-f8
        if (usedFPRs_ < 8) {
            current_ = ABIArg(FloatRegister(FloatRegisters::f1 + usedFPRs_,
                                            FloatRegister::Single));
            usedFPRs_++;
        } else {
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(float);
        }
        // PPC SVR4 ABI: each FP arg also consumes a GPR slot
        usedGPRs_++;
        break;
      case MIRType::Double:
        if (usedFPRs_ < 8) {
            current_ = ABIArg(FloatRegister(FloatRegisters::f1 + usedFPRs_,
                                            FloatRegister::Double));
            usedFPRs_++;
        } else {
            stackOffset_ += stackOffset_ % 8;
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(double);
        }
        // Each FP arg consumes two GPR slots on PPC32
        usedGPRs_ += 2;
        break;
      default:
        MOZ_CRASH("Unexpected argument type");
    }
    return current_;
}

// =========================================================================
// Condition handling
// =========================================================================

Assembler::Condition
Assembler::UnsignedCondition(Condition cond)
{
    switch (cond) {
      case Zero:
      case NonZero:
        return cond;
      case LessThan:
      case Below:
        return Below;
      case LessThanOrEqual:
      case BelowOrEqual:
        return BelowOrEqual;
      case GreaterThan:
      case Above:
        return Above;
      case AboveOrEqual:
      case GreaterThanOrEqual:
        return AboveOrEqual;
      default:
        MOZ_CRASH("unexpected condition");
    }
}

Assembler::Condition
Assembler::ConditionWithoutEqual(Condition cond)
{
    switch (cond) {
      case LessThan:
      case LessThanOrEqual:
        return LessThan;
      case Below:
      case BelowOrEqual:
        return Below;
      case GreaterThan:
      case GreaterThanOrEqual:
        return GreaterThan;
      case Above:
      case AboveOrEqual:
        return Above;
      default:
        MOZ_CRASH("unexpected condition");
    }
}

Assembler::Condition
Assembler::InvertCondition(Condition cond)
{
    switch (cond) {
      case Equal:        return NotEqual;
      case NotEqual:     return Equal;
      case Zero:         return NonZero;
      case NonZero:      return Zero;
      case LessThan:     return GreaterThanOrEqual;
      case LessThanOrEqual: return GreaterThan;
      case GreaterThan:  return LessThanOrEqual;
      case GreaterThanOrEqual: return LessThan;
      case Above:        return BelowOrEqual;
      case AboveOrEqual: return Below;
      case Below:        return AboveOrEqual;
      case BelowOrEqual: return Above;
      case Signed:       return NotSigned;
      case NotSigned:    return Signed;
      default:
        MOZ_CRASH("unexpected condition");
    }
}

Assembler::DoubleCondition
Assembler::InvertCondition(DoubleCondition cond)
{
    switch (cond) {
      case DoubleOrdered:
        return DoubleUnordered;
      case DoubleEqual:
        return DoubleNotEqualOrUnordered;
      case DoubleNotEqual:
        return DoubleEqualOrUnordered;
      case DoubleGreaterThan:
        return DoubleLessThanOrEqualOrUnordered;
      case DoubleGreaterThanOrEqual:
        return DoubleLessThanOrUnordered;
      case DoubleLessThan:
        return DoubleGreaterThanOrEqualOrUnordered;
      case DoubleLessThanOrEqual:
        return DoubleGreaterThanOrUnordered;
      case DoubleUnordered:
        return DoubleOrdered;
      case DoubleEqualOrUnordered:
        return DoubleNotEqual;
      case DoubleNotEqualOrUnordered:
        return DoubleEqual;
      case DoubleGreaterThanOrUnordered:
        return DoubleLessThanOrEqual;
      case DoubleGreaterThanOrEqualOrUnordered:
        return DoubleLessThan;
      case DoubleLessThanOrUnordered:
        return DoubleGreaterThanOrEqual;
      case DoubleLessThanOrEqualOrUnordered:
        return DoubleGreaterThan;
      default:
        MOZ_CRASH("unexpected condition");
    }
}

// =========================================================================
// Code patching and relocation
// =========================================================================

// PPC loads a 32-bit immediate with lis+ori (two instructions, 8 bytes).
// Extract the value from a lis+ori pair at the given address.
/* static */ uint32_t
Assembler::ExtractLisOriValue(uint8_t* inst)
{
    uint32_t* p = (uint32_t*)inst;
    // lis: bits 0-15 = high 16 bits of value
    uint32_t hi = p[0] & 0xffff;
    // ori: bits 0-15 = low 16 bits of value
    uint32_t lo = p[1] & 0xffff;
    return (hi << 16) | lo;
}

// Update a lis+ori pair to load a new 32-bit value.
/* static */ void
Assembler::UpdateLisOriValue(uint8_t* inst, uint32_t value)
{
    uint32_t* p = (uint32_t*)inst;
    // Preserve opcode and register fields, replace immediate
    p[0] = (p[0] & 0xffff0000) | ((value >> 16) & 0xffff);
    p[1] = (p[1] & 0xffff0000) | (value & 0xffff);
}

/* static */ uintptr_t
Assembler::GetPointer(uint8_t* inst)
{
    return ExtractLisOriValue(inst);
}

void
Assembler::Bind(uint8_t* rawCode, CodeOffset* label, const void* address)
{
    if (label->bound()) {
        intptr_t offset = label->offset();
        uint8_t* inst = rawCode + offset;
        UpdateLisOriValue(inst, (uint32_t)(uintptr_t)address);
    }
}

void
Assembler::trace(JSTracer* trc)
{
    for (size_t i = 0; i < jumps_.length(); i++) {
        RelativePatch& rp = jumps_[i];
        if (rp.kind == Relocation::JITCODE) {
            JitCode* code = JitCode::FromExecutable((uint8_t*)rp.target);
            TraceManuallyBarrieredEdge(trc, &code, "masmrel32");
            MOZ_ASSERT(code == JitCode::FromExecutable((uint8_t*)rp.target));
        }
    }
    if (dataRelocations_.length()) {
        CompactBufferReader reader(dataRelocations_);
        while (reader.more()) {
            size_t offset = reader.readUnsigned();
            uint8_t* inst = m_buffer.buffer() + offset;
            TraceOneDataRelocation(trc, inst);
        }
    }
}

/* static */ void
Assembler::TraceOneDataRelocation(JSTracer* trc, uint8_t* inst)
{
    // Validate that inst points to a lis+ori pair.
    uint32_t* p = (uint32_t*)inst;
    if ((p[0] & 0xFC1F0000) != 0x3C000000 || (p[1] & 0xFC000000) != 0x60000000) {
        return;
    }

    void* ptr = (void*)(uintptr_t)ExtractLisOriValue(inst);
    if (!ptr) return;

    // Verify the GC chunk containing this cell is still mapped.
    // Use Mach vm_read to safely probe the ChunkLocation field.
    uintptr_t chunkLocAddr = ((uintptr_t)ptr & ~js::gc::ChunkMask) | js::gc::ChunkLocationOffset;
    vm_offset_t data = 0;
    mach_msg_type_number_t dataCnt = 0;
    kern_return_t kr = vm_read(mach_task_self(), (vm_address_t)chunkLocAddr,
                               sizeof(uint32_t), &data, &dataCnt);
    if (kr != KERN_SUCCESS) {
        // Chunk is unmapped — stale pointer, skip
        return;
    }
    vm_deallocate(mach_task_self(), data, dataCnt);

    void* prior = ptr;
    TraceManuallyBarrieredGenericPointerEdge(trc, reinterpret_cast<gc::Cell**>(&ptr),
                                             "ion-masm-ptr");
    if (ptr != prior) {
        UpdateLisOriValue(inst, (uint32_t)(uintptr_t)ptr);
    }
}

/* static */ void
Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader)
{
    while (reader.more()) {
        size_t offset = reader.readUnsigned();
        uint8_t* inst = code->raw() + offset;
        uint32_t* p = (uint32_t*)inst;
        if ((p[0] & 0xFC1F0000) != 0x3C000000 || (p[1] & 0xFC000000) != 0x60000000)
            continue;
        uintptr_t val = ExtractLisOriValue(inst);
        if (val < 0x1000 || val > 0x40000000)
            continue;
        JitCode* child = JitCode::FromExecutable((uint8_t*)val);
        TraceManuallyBarrieredEdge(trc, &child, "rel32");
    }
}

/* static */ void
Assembler::TraceDataRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader)
{
    while (reader.more()) {
        size_t offset = reader.readUnsigned();
        uint8_t* inst = code->raw() + offset;
        TraceOneDataRelocation(trc, inst);
    }
}

/* static */ void
Assembler::PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue,
                                   ImmPtr expectedValue)
{
    PatchDataWithValueCheck(label, PatchedImmPtr(newValue.value),
                            PatchedImmPtr(expectedValue.value));
}

/* static */ void
Assembler::PatchDataWithValueCheck(CodeLocationLabel label, PatchedImmPtr newValue,
                                   PatchedImmPtr expectedValue)
{
    uint8_t* inst = (uint8_t*)label.raw();
    DebugOnly<uint32_t> value = ExtractLisOriValue(inst);
    MOZ_ASSERT(value == uint32_t(uintptr_t(expectedValue.value)));
    UpdateLisOriValue(inst, uint32_t(uintptr_t(newValue.value)));
    AutoFlushICache::flush(uintptr_t(inst), 8);
}

/* static */ void
Assembler::PatchInstructionImmediate(uint8_t* code, PatchedImmPtr imm)
{
    UpdateLisOriValue(code, (uint32_t)(uintptr_t)imm.value);
    AutoFlushICache::flush(uintptr_t(code), 8);
}

/* static */ uint32_t
Assembler::ExtractInstructionImmediate(uint8_t* code)
{
    return ExtractLisOriValue(code);
}

/* static */ void
Assembler::ToggleCall(CodeLocationLabel inst_, bool enabled)
{
    uint8_t* inst = (uint8_t*)inst_.raw();
    uint32_t* p = (uint32_t*)inst;

    // A call sequence is: lis r12, hi; ori r12, r12, lo; mtctr r12; bctrl
    // To disable: replace bctrl with nop
    // To enable: replace nop with bctrl
    // The bctrl/nop is at offset +12 (4th instruction)
    uint32_t bctrl = (19 << 26) | (BO_ALWAYS << 21) | (528 << 1) | 1;
    uint32_t nop = OP_ORI; // ori 0,0,0

    if (enabled) {
        p[3] = bctrl;
    } else {
        p[3] = nop;
    }
}

// =========================================================================
// Executable copy and finalization
// =========================================================================

void
Assembler::finish()
{
    MOZ_ASSERT(!isFinished);
    isFinished = true;
}

bool
Assembler::asmMergeWith(const Assembler& other)
{
    if (!AssemblerShared::asmMergeWith(size(), other))
        return false;
    return m_buffer.appendRawCode(other.m_buffer.buffer(), other.m_buffer.size());
}

void
Assembler::executableCopy(uint8_t* buffer)
{
    MOZ_ASSERT(isFinished);
    m_buffer.executableCopy(buffer);
    AutoFlushICache::setRange(uintptr_t(buffer), m_buffer.size());
}

void
Assembler::copyJumpRelocationTable(uint8_t* dest)
{
    if (jumpRelocations_.length())
        memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
}

void
Assembler::copyDataRelocationTable(uint8_t* dest)
{
    if (dataRelocations_.length())
        memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
}

void
Assembler::copyPreBarrierTable(uint8_t* dest)
{
    if (preBarriers_.length())
        memcpy(dest, preBarriers_.buffer(), preBarriers_.length());
}

void
Assembler::processCodeLabels(uint8_t* rawCode)
{
    for (size_t i = 0; i < codeLabels_.length(); i++) {
        CodeLabel label = codeLabels_[i];
        Bind(rawCode, label.patchAt(), rawCode + label.target()->offset());
    }
}

size_t
Assembler::jumpRelocationTableBytes() const
{
    return jumpRelocations_.length();
}

size_t
Assembler::dataRelocationTableBytes() const
{
    return dataRelocations_.length();
}

size_t
Assembler::preBarrierTableBytes() const
{
    return preBarriers_.length();
}

size_t
Assembler::bytesNeeded() const
{
    return size() +
           jumpRelocationTableBytes() +
           dataRelocationTableBytes() +
           preBarrierTableBytes();
}

// =========================================================================
// Jump patching
// =========================================================================

void
jit::PatchJump(CodeLocationJump& jump_, CodeLocationLabel label, ReprotectCode reprotect)
{
    uint8_t* inst = (uint8_t*)jump_.raw();
    uint32_t* p = (uint32_t*)inst;
    MaybeAutoWritableJitCode awjc(inst, 16, reprotect);

    uint32_t sourceAddr = (uint32_t)(uintptr_t)inst;
    uint32_t targetAddr = (uint32_t)(uintptr_t)label.raw();
    int32_t offset = targetAddr - sourceAddr;

    // Check if this was already optimized to a direct 'b' instruction.
    // If so, update the branch offset directly instead of corrupting it
    // with UpdateLisOriValue.
    uint32_t opcode = p[0] >> 26;
    if (opcode == 18) {
        // Already a 'b' instruction — update its offset
        if (offset >= -0x2000000 && offset < 0x2000000) {
            // Still fits in relative branch
            uint32_t lk = p[0] & 1; // preserve link bit
            p[0] = (18 << 26) | (offset & 0x03fffffc) | lk;
        } else {
            // Need to convert back to far jump: lis r11, hi; ori r11, r11, lo; mtctr r11; bctr
            uint32_t hi = (targetAddr >> 16) & 0xffff;
            uint32_t lo = targetAddr & 0xffff;
            p[0] = (15 << 26) | (Registers::r11 << 21) | hi;  // lis r11, hi
            p[1] = (24 << 26) | (Registers::r11 << 21) | (Registers::r11 << 16) | lo; // ori r11, r11, lo
            // p[2] and p[3] should still be mtctr r11; bctr from original farJumpWithPatch
        }
    } else {
        // Still a lis+ori pair — update normally
        Assembler::UpdateLisOriValue(inst, targetAddr);
    }

    AutoFlushICache::flush(uintptr_t(inst), 16);
}

void
jit::PatchBackedge(CodeLocationJump& jump, CodeLocationLabel label,
                   JitRuntime::BackedgeTarget target)
{
    uint8_t* inst = (uint8_t*)jump.raw();
    uint32_t* p = (uint32_t*)inst;

    uint32_t sourceAddr = (uint32_t)(uintptr_t)inst;
    uint32_t targetAddr = (uint32_t)(uintptr_t)label.raw();

    int32_t offset = targetAddr - sourceAddr;

    // If the offset fits in a 26-bit signed field (b instruction range),
    // use a direct branch. Otherwise use the lis+ori+mtctr+bctr sequence.
    if (offset >= -0x2000000 && offset < 0x2000000) {
        // Patch to direct branch
        p[0] = OP_B | (offset & 0x03fffffc);
        p[1] = OP_ORI; // nop
        p[2] = OP_ORI; // nop
        p[3] = OP_ORI; // nop
    } else {
        // Far branch: lis r12, hi; ori r12, r12, lo; mtctr r12; bctr
        uint32_t hi = (targetAddr >> 16) & 0xffff;
        uint32_t lo = targetAddr & 0xffff;
        p[0] = OP_ADDIS | (Registers::r12 << 21) | hi;  // lis r12, hi
        p[1] = OP_ORI | (Registers::r12 << 21) | (Registers::r12 << 16) | lo; // ori r12, r12, lo
        uint32_t spr = ((SPR_CTR & 0x1f) << 5) | ((SPR_CTR >> 5) & 0x1f);
        p[2] = OP_X31 | (Registers::r12 << 21) | (spr << 11) | (XO_MTSPR << 1); // mtctr r12
        p[3] = (19 << 26) | (BO_ALWAYS << 21) | (528 << 1); // bctr
    }

    AutoFlushICache::flush(uintptr_t(inst), 16);
}
void
Assembler::retarget(Label* label, Label* target)
{
    if (label->used()) {
        if (target->bound()) {
            // Walk label's use chain and patch each branch to target.
            int32_t current = label->offset();
            while (current != LabelBase::INVALID_OFFSET) {
                uint32_t* inst = (uint32_t*)(m_buffer.buffer() + current);
                // Extract displacement from b instruction (bits 6-29, sign-extended)
                int32_t disp = (int32_t)(*inst & 0x03FFFFFC);
                if (disp & 0x02000000)
                    disp |= (int32_t)0xFC000000; // sign extend
                int32_t next = (disp == 0) ? LabelBase::INVALID_OFFSET
                                           : current + disp;
                // Patch to branch to target
                int32_t targetDisp = target->offset() - current;
                *inst = (*inst & 0xFC000003) | (targetDisp & 0x03FFFFFC);
                current = next;
            }
        } else if (target->used()) {
            // Both unbound. Walk to end of label's chain, splice onto target's.
            int32_t current = label->offset();
            while (true) {
                uint32_t* inst = (uint32_t*)(m_buffer.buffer() + current);
                int32_t disp = (int32_t)(*inst & 0x03FFFFFC);
                if (disp & 0x02000000)
                    disp |= (int32_t)0xFC000000;
                if (disp == 0) {
                    // End of label's chain — link to target's chain head
                    int32_t linkDisp = target->offset() - current;
                    *inst = (*inst & 0xFC000003) | (linkDisp & 0x03FFFFFC);
                    break;
                }
                current = current + disp;
            }
            // Set target's head to label's head
            target->use(label->offset());
        } else {
            // Target unused — transfer label's chain to target
            target->use(label->offset());
        }
    }
    label->reset();
}

