/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Bailouts.h"
#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/Linker.h"
#include "jit/MacroAssembler.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/ppc/Assembler-ppc.h"
#include "jit/ppc/Bailouts-ppc.h"
#include "jit/ppc/SharedICHelpers-ppc.h"
#include "jit/VMFunctions.h"

using namespace js;
using namespace js::jit;



// PPC SVR4 ABI callee-saved registers: r14-r31 (18 GPRs), f14-f31 (18 FPRs)
static const uint32_t NumCalleeGPRs = 18;
static const uint32_t NumCalleeFPRs = 18;
static const uint32_t CalleeSaveGPRSize = NumCalleeGPRs * sizeof(uint32_t);
static const uint32_t CalleeSaveFPRSize = NumCalleeFPRs * sizeof(double);
static const uint32_t CalleeSaveSize = CalleeSaveGPRSize + CalleeSaveFPRSize;

// BailoutStack data size: everything except snapshotOffset_
// (snapshotOffset_ is only pushed for lazy bailouts by the code generator)
static const uint32_t bailoutDataSize = sizeof(BailoutStack) - 2 * sizeof(uintptr_t);
static const uint32_t bailoutInfoOutParamSize = 2 * sizeof(uintptr_t);

static void
GenerateReturn(MacroAssembler& masm, int returnCode)
{
    masm.ma_li(r3, Imm32(returnCode));
    masm.ma_blr();
}

static void
PushBailoutFrame(MacroAssembler& masm, uint32_t frameClass, Register spArg)
{
    // r0 already holds frameSize, loaded by deoptLabel_ in CodeGenerator.

    // Make room for BailoutStack data (everything except snapshotOffset_ and padding_)
    masm.subPtr(Imm32(bailoutDataSize), StackPointer);

    // Save general purpose registers
    for (uint32_t i = 0; i < Registers::Total; i++) {
        uint32_t off = BailoutStack::offsetOfRegs() + i * sizeof(uintptr_t);
        masm.storePtr(Register::FromCode(i), Address(StackPointer, off));
    }

    // Save floating point registers
    for (uint32_t i = 0; i < FloatRegisters::TotalDouble; i++)
        masm.as_stfd(FloatRegister(i, FloatRegister::Double), StackPointer,
                     BailoutStack::offsetOfFpRegs() + i * sizeof(double));

    // Store the frameSize_/tableOffset_ from r0 (loaded by code generator)
    masm.storePtr(r0, Address(StackPointer, BailoutStack::offsetOfFrameSize()));

    // Store frame class ID
    masm.storePtr(ImmWord(frameClass), Address(StackPointer, BailoutStack::offsetOfFrameClass()));

    // Put pointer to BailoutStack as first argument
    masm.movePtr(StackPointer, spArg);
}

static void
GenerateBailoutThunk(JSContext* cx, MacroAssembler& masm, uint32_t frameClass)
{
    PushBailoutFrame(masm, frameClass, r3);

    // Make space for Bailout's outparam (BailoutInfo*)
    masm.subPtr(Imm32(bailoutInfoOutParamSize), StackPointer);
    masm.storePtr(ImmPtr(nullptr), Address(StackPointer, 0));
    masm.movePtr(StackPointer, r4);

    masm.setupAlignedABICall();
    masm.passABIArg(r3);
    masm.passABIArg(r4);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, Bailout));

    // PPC FATAL_ERROR shortcut: if Bailout() returned FATAL_ERROR (corrupted frame),
    // bypass bailoutTail entirely. Restore callee-saved registers from the enterJIT
    // save area (r14 in BailoutStack = savedSP) and return to C++ with JS_ION_ERROR.
    {
        Label notFatal;
        masm.branch32(Assembler::NotEqual, ReturnReg, Imm32(BAILOUT_RETURN_FATAL_ERROR),
                       &notFatal);

        // Load r14 (savedSP) from BailoutStack's register dump.
        // Stack layout: [SP] = bailoutInfoOutParam, [SP + bailoutInfoOutParamSize] = BailoutStack
        // r14 is at regs_[14] in BailoutStack.
        masm.loadPtr(Address(StackPointer,
            bailoutInfoOutParamSize + BailoutStack::offsetOfRegs() +
            14 * sizeof(Registers::RegisterContent)),
            StackPointer);
        // SP now = r14 = savedSP from enterJIT, pointing at saved FPRs.

        // Load saved vp pointer from guard zone (stored at r14-4 by enterJIT).
        masm.loadPtr(Address(StackPointer, -4), r6);  // r6 = vp (volatile, safe to use)

        // Restore callee-saved FPRs (f31 down to f14, matching enterJIT save order)
        for (int i = FloatRegisters::f31; i >= (int)FloatRegisters::f14; i--) {
            masm.as_lfd(FloatRegister(i, FloatRegister::Double), StackPointer, 0);
            masm.addPtr(Imm32(8), StackPointer);
        }

        // Restore callee-saved GPRs (r31 down to r14)
        for (int i = Registers::r31; i >= (int)Registers::r14; i--)
            masm.ma_pop(Register::FromCode(i));

        // Set return value = MagicValue(JS_ION_ERROR)
        masm.moveValue(MagicValue(JS_ION_ERROR), JSReturnOperand);

        // Store error to *vp (r6 = saved vp from guard zone)
        masm.as_stw(JSReturnReg_Type, r6, TAG_OFFSET);
        masm.as_stw(JSReturnReg_Data, r6, PAYLOAD_OFFSET);

        // Pop LR and return to C++
        masm.ma_pop(r0);
        masm.as_mtlr(r0);
        masm.as_blr();

        masm.bind(&notFatal);
    }

    // Get BailoutInfo pointer
    masm.loadPtr(Address(StackPointer, 0), r5);

    // Remove bailout frame and Ion frame from stack
    if (frameClass == NO_FRAME_SIZE_CLASS_ID) {
        // Lazy bailout: load frameSize from stack
        masm.loadPtr(Address(StackPointer,
                             bailoutInfoOutParamSize + BailoutStack::offsetOfFrameSize()), r4);

        // Remove complete BailoutStack + bailoutInfoOutParam
        masm.addPtr(Imm32(sizeof(BailoutStack) + bailoutInfoOutParamSize), StackPointer);
        // Remove the Ion frame
        masm.addPtr(r4, StackPointer);
    } else {
        uint32_t frameSize = FrameSizeClass::FromClass(frameClass).frameSize();
        masm.addPtr(Imm32(bailoutDataSize + bailoutInfoOutParamSize + frameSize), StackPointer);
    }

    // Jump to shared bailout tail. BailoutInfo pointer is in r5.
    JitCode* bailoutTail = cx->runtime()->jitRuntime()->getBailoutTail();
    masm.branch(bailoutTail);
}

JitCode*
JitRuntime::generateEnterJIT(JSContext* cx, EnterJitType type)
{
    MacroAssembler masm(cx);

    // Arguments on entry (PPC SVR4 ABI — all 8 in registers):
    //   r3 = jitcode (code to call)
    //   r4 = argc
    //   r5 = argv
    //   r6 = osrFrame (InterpreterFrame*)
    //   r7 = calleeToken
    //   r8 = scopeChain
    //   r9 = numStackValues
    //   r10 = result (Value* vp)
    //
    // OsrFrameReg = r6 (matches 4th argument position).

    const Register reg_code      = r3;
    const Register reg_argc      = r4;
    const Register reg_argv      = r5;
    const Register reg_frame     = r6;
    const Register reg_token     = r7;
    const Register reg_scope     = r8;
    const Register reg_numStack  = r9;
    const Register reg_result    = r10;

    MOZ_ASSERT(OsrFrameReg == reg_frame);

    // Save LR
    masm.as_mflr(r0);
    masm.ma_push(r0);

    // Save callee-saved GPRs (r14-r31)
    for (uint32_t i = Registers::r14; i <= Registers::r31; i++)
        masm.ma_push(Register::FromCode(i));

    // Save callee-saved FPRs (f14-f31)
    for (uint32_t i = FloatRegisters::f14; i <= FloatRegisters::f31; i++) {
        masm.subPtr(Imm32(8), StackPointer);
        masm.as_stfd(FloatRegister(i, FloatRegister::Double), StackPointer, 0);
    }

    // Save stack pointer — we'll use this to compute the Entry frame descriptor.
    masm.movePtr(StackPointer, r14);  // r14 = savedSP

    // Save arguments to callee-saved registers (they survive the arg push loop).
    masm.movePtr(reg_result, r28);    // r28 = vp (result pointer)
    masm.movePtr(reg_code, r27);      // r27 = jitcode
    masm.movePtr(reg_token, r16);     // r16 = calleeToken
    masm.movePtr(reg_frame, r26);     // r26 = osrFrame
    masm.movePtr(reg_scope, r25);     // r25 = scopeChain

    // Load numActualArgs from the vp Value (stored as Int32).
    masm.unboxInt32(Address(r28, 0), r17);  // r17 = numActualArgs

    // Set BaselineFrameReg for baseline entry.
    if (type == EnterJitBaseline)
        masm.movePtr(StackPointer, BaselineFrameReg);

    // Handle constructing: if calleeToken has the Constructing bit, argc++.
    {
        Label noNewTarget;
        masm.branchTest32(Assembler::Zero, r16, Imm32(CalleeToken_FunctionConstructing),
                          &noNewTarget);
        masm.add32(Imm32(1), reg_argc);
        masm.bind(&noNewTarget);
    }

    // Push arguments from argv onto stack in reverse order.
    // r15 = argv + argc * 8 (one past end)
    masm.ma_sll(r15, reg_argc, Imm32(3));  // r15 = argc * 8
    masm.as_add(r15, reg_argv, r15);       // r15 = &argv[argc]

    {
        Label header, footer;
        masm.ma_b(r15, reg_argv, &footer, Assembler::BelowOrEqual);

        masm.bind(&header);
        masm.subPtr(Imm32(sizeof(Value)), r15);
        masm.subPtr(Imm32(sizeof(Value)), StackPointer);

        // Copy one Value (8 bytes) using r20/r21 as temporaries.
        masm.load32(Address(r15, 0), r20);
        masm.store32(r20, Address(StackPointer, 0));
        masm.load32(Address(r15, 4), r21);
        masm.store32(r21, Address(StackPointer, 4));

        masm.ma_b(r15, reg_argv, &header, Assembler::Above);
        masm.bind(&footer);
    }

    // Push JitFrameLayout header: numActualArgs, then calleeToken.
    masm.subPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);
    masm.storePtr(r17, Address(StackPointer, sizeof(uintptr_t)));  // numActualArgs
    masm.storePtr(r16, Address(StackPointer, 0));                  // calleeToken

    // Compute the Entry frame descriptor.
    // Frame size = savedSP - currentSP = total bytes pushed since saving SP.
    masm.movePtr(r14, r15);            // r15 = savedSP
    masm.subPtr(StackPointer, r15);    // r15 = savedSP - SP = frame size
    masm.makeFrameDescriptor(r15, JitFrame_Entry, JitFrameLayout::Size());
    masm.push(r15);                    // push descriptor

    if (type == EnterJitBaseline) {
        // Non-OSR baseline entry: load scopeChain into R1.
        // (OSR handling not yet implemented.)
        masm.movePtr(r25, R1.scratchReg());
    }

    // Call the JIT code.
    // With JS_USE_LINK_REGISTER, call() does bctrl — the callee
    // is responsible for pushing LR (the return address) to the stack.
    masm.callJitNoProfiler(r27);

    // JIT code has returned.  The callee popped its own frame and
    // left the Entry descriptor on the stack for us.



    // Pop the Entry frame descriptor and use it to remove pushed arguments.
    masm.pop(r15);                              // r15 = descriptor
    masm.movePtr(r15, r16);                     // r16 = raw descriptor (save for diag)
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), r15);



    masm.addPtr(r15, StackPointer);

    // Store return value to *vp.
    masm.as_stw(JSReturnReg_Type, r28, TAG_OFFSET);
    masm.as_stw(JSReturnReg_Data, r28, PAYLOAD_OFFSET);

    // Restore callee-saved FPRs
    for (int i = FloatRegisters::f31; i >= (int)FloatRegisters::f14; i--) {
        masm.as_lfd(FloatRegister(i, FloatRegister::Double), StackPointer, 0);
        masm.addPtr(Imm32(8), StackPointer);
    }

    // Restore callee-saved GPRs
    for (int i = Registers::r31; i >= (int)Registers::r14; i--)
        masm.ma_pop(Register::FromCode(i));

    // Restore LR and return
    masm.ma_pop(r0);
    masm.as_mtlr(r0);
    masm.as_blr();

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);
    return code;
}


JitCode*
JitRuntime::generateInvalidator(JSContext* cx)
{
    MacroAssembler masm(cx);

    // NOTE: Members ionScript_ and osiPointReturnAddress_ of
    // InvalidationBailoutStack are already on the stack (pushed by
    // generateInvalidateEpilogue).
    static const uint32_t STACK_DATA_SIZE = sizeof(InvalidationBailoutStack) -
                                            2 * sizeof(uintptr_t);

    // Make room for the register dump portion of InvalidationBailoutStack.
    masm.subPtr(Imm32(STACK_DATA_SIZE), StackPointer);

    // Save general purpose registers at their correct struct offsets.
    for (uint32_t i = 0; i < Registers::Total; i++) {
        Address address(StackPointer, InvalidationBailoutStack::offsetOfRegs() +
                                      i * sizeof(uintptr_t));
        masm.storePtr(Register::FromCode(i), address);
    }

    // Save floating point registers at their correct struct offsets.
    for (uint32_t i = 0; i < FloatRegisters::TotalPhys; i++) {
        masm.as_stfd(FloatRegister(i, FloatRegister::Double), StackPointer,
                     InvalidationBailoutStack::offsetOfFpRegs() + i * sizeof(double));
    }

    // Pass pointer to InvalidationBailoutStack as first argument.
    masm.ma_move(r3, StackPointer);

    // Reserve space for output parameters: frameSizeOut and bailoutInfo.
    masm.subPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);
    // r4 = &frameSizeOut (higher address)
    masm.ma_addu(r4, StackPointer, Imm32(sizeof(uintptr_t)));
    // r5 = &bailoutInfo (lower address)
    masm.ma_move(r5, StackPointer);

    masm.setupAlignedABICall();
    masm.passABIArg(r3);
    masm.passABIArg(r4);
    masm.passABIArg(r5);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, InvalidationBailout));

    // Load output values from the stack.
    masm.loadPtr(Address(StackPointer, 0), r5);                    // bailoutInfo
    masm.loadPtr(Address(StackPointer, sizeof(uintptr_t)), r4);    // frameSizeOut

    // Remove the output params + InvalidationBailoutStack from the stack.
    masm.addPtr(Imm32(sizeof(InvalidationBailoutStack) + 2 * sizeof(uintptr_t)), StackPointer);

    // Remove the space that this Ion frame was using (computed by InvalidationBailout).
    masm.addPtr(r4, StackPointer);

    // Jump to shared bailout tail. BailoutInfo pointer is in r5.
    JitCode* bailoutTail = cx->runtime()->jitRuntime()->getBailoutTail();
    masm.branch(bailoutTail);

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

    JitSpew(JitSpew_IonInvalidate, "   invalidation thunk created at %p", (void*) code->raw());

    return code;
}

JitCode*
JitRuntime::generateArgumentsRectifier(JSContext* cx, void** returnAddrOut)
{
    MacroAssembler masm(cx);
    masm.pushReturnAddress();

    // ArgumentsRectifierReg (r20) contains the |nargs| pushed onto the
    // current frame. Including |this|, there are (|nargs| + 1) args to copy.
    MOZ_ASSERT(ArgumentsRectifierReg == r20);

    Register numActArgsReg = r6;
    Register calleeTokenReg = r7;
    Register numArgsReg = r5;

    // Copy number of actual arguments
    masm.loadPtr(Address(StackPointer, RectifierFrameLayout::offsetOfNumActualArgs()),
                 numActArgsReg);

    // Load callee token and get formal nargs from the function
    masm.loadPtr(Address(StackPointer, RectifierFrameLayout::offsetOfCalleeToken()),
                 calleeTokenReg);
    masm.mov(calleeTokenReg, numArgsReg);
    masm.andPtr(Imm32(CalleeTokenMask), numArgsReg);
    masm.load16ZeroExtend(Address(numArgsReg, JSFunction::offsetOfNargs()), numArgsReg);

    // r9 = number of undefined values to push = numArgsReg - ArgumentsRectifierReg
    masm.as_subf(r9, r20, numArgsReg);

    // Get the topmost argument pointer
    // r8 = nargs * 8
    masm.ma_sll(r8, r20, Imm32(3));
    // r10 = sp + nargs * 8
    masm.as_add(r10, StackPointer, r8);
    masm.addPtr(Imm32(sizeof(RectifierFrameLayout)), r10);

    {
        Label notConstructing;

        masm.branchTest32(Assembler::Zero, calleeTokenReg, Imm32(CalleeToken_FunctionConstructing),
                          &notConstructing);

        // Copy newTarget value
        masm.subPtr(Imm32(sizeof(Value)), StackPointer);
        masm.load32(Address(r10, NUNBOX32_TYPE_OFFSET + sizeof(Value)), r8);
        masm.store32(r8, Address(StackPointer, NUNBOX32_TYPE_OFFSET));
        masm.load32(Address(r10, NUNBOX32_PAYLOAD_OFFSET + sizeof(Value)), r8);
        masm.store32(r8, Address(StackPointer, NUNBOX32_PAYLOAD_OFFSET));

        // Include newTarget in the frame size
        masm.add32(Imm32(1), numArgsReg);

        masm.bind(&notConstructing);
    }

    // Push undefined values for missing arguments
    masm.moveValue(UndefinedValue(), ValueOperand(r3, r4));
    {
        Label undefLoopTop;
        masm.bind(&undefLoopTop);

        masm.subPtr(Imm32(sizeof(Value)), StackPointer);
        masm.storeValue(ValueOperand(r3, r4), Address(StackPointer, 0));
        masm.sub32(Imm32(1), r9);

        masm.ma_b(r9, r9, &undefLoopTop, Assembler::NonZero);
    }

    // Copy arguments, |nargs| + 1 times (to include |this|)
    {
        Label copyLoopTop, initialSkip;

        masm.ma_b(&initialSkip);

        masm.bind(&copyLoopTop);
        masm.subPtr(Imm32(sizeof(Value)), r10);
        masm.sub32(Imm32(1), r20);

        masm.bind(&initialSkip);

        // Read argument and push to stack
        masm.subPtr(Imm32(sizeof(Value)), StackPointer);
        masm.load32(Address(r10, NUNBOX32_TYPE_OFFSET), r8);
        masm.store32(r8, Address(StackPointer, NUNBOX32_TYPE_OFFSET));
        masm.load32(Address(r10, NUNBOX32_PAYLOAD_OFFSET), r8);
        masm.store32(r8, Address(StackPointer, NUNBOX32_PAYLOAD_OFFSET));

        masm.ma_b(r20, r20, &copyLoopTop, Assembler::NonZero);
    }

    // Translate framesize from values into bytes
    masm.ma_addu(r8, numArgsReg, Imm32(1));
    masm.lshiftPtr(Imm32(3), r8);

    // Construct sizeDescriptor
    masm.makeFrameDescriptor(r8, JitFrame_Rectifier, JitFrameLayout::Size());

    // Construct JitFrameLayout
    masm.subPtr(Imm32(3 * sizeof(uintptr_t)), StackPointer);
    // Push actual argument count
    masm.storePtr(numActArgsReg, Address(StackPointer, 2 * sizeof(uintptr_t)));
    // Push callee token
    masm.storePtr(calleeTokenReg, Address(StackPointer, sizeof(uintptr_t)));
    // Push frame descriptor
    masm.storePtr(r8, Address(StackPointer, 0));

    // Call the target function
    masm.andPtr(Imm32(CalleeTokenMask), calleeTokenReg);
    masm.loadPtr(Address(calleeTokenReg, JSFunction::offsetOfNativeOrScript()), r9);
    masm.loadBaselineOrIonRaw(r9, r9, nullptr);



    uint32_t returnOffset = masm.callJitNoProfiler(r9);

    // Remove the rectifier frame
    // r8 <- descriptor with FrameType
    masm.loadPtr(Address(StackPointer, 0), r8);

    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), r8);

    // Discard descriptor, calleeToken and number of actual arguments
    masm.addPtr(Imm32(3 * sizeof(uintptr_t)), StackPointer);

    // Discard pushed arguments
    masm.addPtr(r8, StackPointer);

    // PPC fix: restore LR from saved return address on stack.
    // MIPS ret() does ma_pop(ra)+jr(ra), but PPC ret() only does blr().
    // Without this, LR still points to the address after callJitNoProfiler's
    // bctrl, causing blr to re-enter the rectifier cleanup and corrupt SP.
    masm.popReturnAddress();
    masm.ret();
    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

    if (returnAddrOut)
        *returnAddrOut = (void*) (code->raw() + returnOffset);

    return code;
}

JitCode*
JitRuntime::generateBailoutTable(JSContext* cx, uint32_t frameClass)
{
    MacroAssembler masm(cx);

    Label bailout;
    for (size_t i = 0; i < BAILOUT_TABLE_SIZE; i++) {
        // Calculate offset to the end of the table (start of thunk code)
        int32_t offset = (BAILOUT_TABLE_SIZE - i) * BAILOUT_TABLE_ENTRY_SIZE;

        // Emit bl (branch and link) - stores PC+4 in LR.
        // The thunk uses LR to identify which table entry was hit.
        // PPC bl encoding: opcode 18, LI field, AA=0, LK=1
        masm.writeInst(0x48000001 | (offset & 0x03FFFFFC));
        // nop padding to fill BAILOUT_TABLE_ENTRY_SIZE (8 bytes)
        masm.writeInst(0x60000000);
    }
    masm.bind(&bailout);

    GenerateBailoutThunk(cx, masm, frameClass);

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);
    return code;
}

JitCode*
JitRuntime::generateBailoutHandler(JSContext* cx)
{
    MacroAssembler masm(cx);
    GenerateBailoutThunk(cx, masm, NO_FRAME_SIZE_CLASS_ID);

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);
    return code;
}

JitCode*
JitRuntime::generateVMWrapper(JSContext* cx, const VMFunction& f)
{
    MOZ_ASSERT(functionWrappers_);
    MOZ_ASSERT(functionWrappers_->initialized());
    VMWrapperMap::AddPtr p = functionWrappers_->lookupForAdd(&f);
    if (p)
        return p->value();

    MacroAssembler masm(cx);

    AllocatableGeneralRegisterSet regs(Register::Codes::WrapperMask);

    static_assert((Register::Codes::VolatileMask & ~Register::Codes::WrapperMask) == 0,
                  "Wrapper register set should be a superset of Volatile register set.");

    // The context is the first argument; r3 is the first argument register.
    Register cxreg = r3;
    regs.take(cxreg);

    // If it isn't a tail call, then the return address needs to be saved
    if (f.expectTailCall == NonTailCall)
        masm.pushReturnAddress();

    // Link up exit frame
    masm.enterExitFrame(&f);
    masm.loadJSContext(cxreg);

    // Save the base of the argument set stored on the stack.
    // IMPORTANT: argsBase must NOT be an ABI argument register (r3-r10)
    // because passABIArg/MoveResolver may write arg registers before
    // resolving memory loads that use argsBase as a base register.
    // Using callee-saved r19 avoids this conflict.
    Register argsBase = InvalidReg;
    if (f.explicitArgs) {
        argsBase = r19;
        // r19 is callee-saved, not in WrapperMask — don't take from regs
        masm.ma_addu(argsBase, StackPointer, Imm32(ExitFrameLayout::SizeWithFooter()));
    }

    masm.alignStackPointer();

    // Reserve space for outparam
    uint32_t outParamSize = 0;
    switch (f.outParam) {
      case Type_Value:
        outParamSize = sizeof(Value);
        masm.reserveStack(outParamSize);
        break;

      case Type_Handle:
        {
            uint32_t pushed = masm.framePushed();
            masm.PushEmptyRooted(f.outParamRootType);
            outParamSize = masm.framePushed() - pushed;
        }
        break;

      case Type_Bool:
      case Type_Int32:
        MOZ_ASSERT(sizeof(uintptr_t) == sizeof(uint32_t));
      case Type_Pointer:
        outParamSize = sizeof(uintptr_t);
        masm.reserveStack(outParamSize);
        break;

      case Type_Double:
        outParamSize = sizeof(double);
        masm.reserveStack(outParamSize);
        break;
      default:
        MOZ_ASSERT(f.outParam == Type_Void);
        break;
    }

    uint32_t outParamOffset = 0;
    if (f.outParam != Type_Void) {
        MOZ_ASSERT(outParamSize <= sizeof(double));
        outParamOffset += sizeof(double) - outParamSize;
    }
    outParamOffset += f.doubleByRefArgs() * sizeof(double);

    Register doubleArgs = SecondScratchRegister; // r12 — must NOT be an ABI arg register (r3-r10)
    masm.reserveStack(outParamOffset);
    masm.movePtr(StackPointer, doubleArgs);

    if (!generateTLEnterVM(cx, masm, f))
        return nullptr;

    masm.setupAlignedABICall();
    masm.passABIArg(cxreg);

    size_t argDisp = 0;
    size_t doubleArgDisp = 0;

    // Copy arguments
    for (uint32_t explicitArg = 0; explicitArg < f.explicitArgs; explicitArg++) {
        switch (f.argProperties(explicitArg)) {
          case VMFunction::WordByValue:
            masm.passABIArg(MoveOperand(argsBase, argDisp), MoveOp::GENERAL);
            argDisp += sizeof(uint32_t);
            break;
          case VMFunction::DoubleByValue:
            MOZ_ASSERT(f.argPassedInFloatReg(explicitArg));
            masm.passABIArg(MoveOperand(argsBase, argDisp), MoveOp::DOUBLE);
            argDisp += sizeof(double);
            break;
          case VMFunction::WordByRef:
            masm.passABIArg(MoveOperand(argsBase, argDisp, MoveOperand::EFFECTIVE_ADDRESS),
                            MoveOp::GENERAL);
            argDisp += sizeof(uint32_t);
            break;
          case VMFunction::DoubleByRef:
            // Use integer load/store to copy Value — lfd/stfd may
            // canonicalize NaN payloads on PPC, corrupting JS tags.
            masm.ma_lw(ScratchRegister, Address(argsBase, argDisp));
            masm.ma_sw(ScratchRegister, Address(doubleArgs, doubleArgDisp));
            masm.ma_lw(ScratchRegister, Address(argsBase, argDisp + 4));
            masm.ma_sw(ScratchRegister, Address(doubleArgs, doubleArgDisp + 4));
            masm.passABIArg(MoveOperand(doubleArgs, doubleArgDisp, MoveOperand::EFFECTIVE_ADDRESS),
                            MoveOp::GENERAL);
            doubleArgDisp += sizeof(double);
            argDisp += sizeof(double);
            break;
        }
    }

    MOZ_ASSERT_IF(f.outParam != Type_Void,
                  doubleArgDisp + sizeof(double) == outParamOffset + outParamSize);

    // Copy the implicit outparam, if any
    if (f.outParam != Type_Void) {
        masm.passABIArg(MoveOperand(doubleArgs, outParamOffset, MoveOperand::EFFECTIVE_ADDRESS),
                            MoveOp::GENERAL);
    }

    masm.callWithABI(f.wrapped);

    if (!generateTLExitVM(cx, masm, f))
        return nullptr;

    // Test for failure
    switch (f.failType()) {
      case Type_Object:
        masm.branchTestPtr(Assembler::Zero, ReturnReg, ReturnReg, masm.failureLabel());
        break;
      case Type_Bool:
        masm.branchIfFalseBool(ReturnReg, masm.failureLabel());
        break;
      default:
        MOZ_CRASH("unknown failure kind");
    }

    masm.freeStack(outParamOffset);

    // Load outparam and free stack
    switch (f.outParam) {
      case Type_Handle:
        masm.popRooted(f.outParamRootType, ReturnReg, JSReturnOperand);
        break;

      case Type_Value:
        masm.loadValue(Address(StackPointer, 0), JSReturnOperand);
        masm.freeStack(sizeof(Value));
        break;

      case Type_Int32:
        MOZ_ASSERT(sizeof(uintptr_t) == sizeof(uint32_t));
      case Type_Pointer:
        masm.load32(Address(StackPointer, 0), ReturnReg);
        masm.freeStack(sizeof(uintptr_t));
        break;

      case Type_Bool:
        masm.load8ZeroExtend(Address(StackPointer, 0), ReturnReg);
        masm.freeStack(sizeof(uintptr_t));
        break;

      case Type_Double:
        if (cx->runtime()->jitSupportsFloatingPoint) {
            masm.as_lfd(ReturnDoubleReg, StackPointer, 0);
        } else {
            masm.assumeUnreachable("Unable to load into float reg, with no FP support.");
        }
        masm.freeStack(sizeof(double));
        break;

      default:
        MOZ_ASSERT(f.outParam == Type_Void);
        break;
    }

    masm.restoreStackPointer();

    masm.leaveExitFrame();
    // (return address check removed — was false-positive)
    masm.retn(Imm32(sizeof(ExitFrameLayout) +
                    f.explicitStackSlots() * sizeof(uintptr_t) +
                    f.extraValuesToPop * sizeof(Value)));

    Linker linker(masm);
    JitCode* wrapper = linker.newCode<NoGC>(cx, OTHER_CODE);
    if (!wrapper)
        return nullptr;

    if (!functionWrappers_->relookupOrAdd(p, &f, wrapper))
        return nullptr;

    return wrapper;
}

JitCode*
JitRuntime::generatePreBarrier(JSContext* cx, MIRType type)
{
    MacroAssembler masm(cx);

    // Save LR manually (PPC LR is not a GPR, can't be in LiveRegisterSet)
    masm.as_mflr(ScratchRegister);
    masm.push(ScratchRegister);

    LiveRegisterSet save;
    if (cx->runtime()->jitSupportsFloatingPoint) {
        save.set() = RegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                           FloatRegisterSet(FloatRegisters::VolatileMask));
    } else {
        save.set() = RegisterSet(GeneralRegisterSet(Registers::VolatileMask),
                           FloatRegisterSet());
    }
    masm.PushRegsInMask(save);

    MOZ_ASSERT(PreBarrierReg == r4);
    masm.movePtr(ImmPtr(cx->runtime()), r3);

    masm.setupUnalignedABICall(r5);
    masm.passABIArg(r3);
    masm.passABIArg(r4);
    masm.callWithABI(IonMarkFunction(type));

    masm.PopRegsInMask(save);

    // Restore LR and return
    masm.pop(ScratchRegister);
    masm.as_mtlr(ScratchRegister);
    masm.ret();

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);
    return code;
}

typedef bool (*HandleDebugTrapFn)(JSContext*, BaselineFrame*, uint8_t*, bool*);
static const VMFunction HandleDebugTrapInfo =
    FunctionInfo<HandleDebugTrapFn>(HandleDebugTrap, "HandleDebugTrap");

JitCode*
JitRuntime::generateDebugTrapHandler(JSContext* cx)
{
    MacroAssembler masm(cx);

    Register scratch1 = r8;
    Register scratch2 = r9;

    // Load BaselineFrame pointer in scratch1
    masm.movePtr(BaselineFrameReg, scratch1);
    masm.subPtr(Imm32(BaselineFrame::Size()), scratch1);

    // Enter a stub frame; set ICStubReg to nullptr since this pointer is
    // marked during GC
    masm.movePtr(ImmPtr(nullptr), ICStubReg);
    EmitBaselineEnterStubFrame(masm, scratch2);

    JitCode* code = cx->runtime()->jitRuntime()->getVMWrapper(HandleDebugTrapInfo);
    if (!code)
        return nullptr;

    // Push args for HandleDebugTrap(cx, frame, retAddr, mustReturn)
    // The VM wrapper handles cx. We push frame and retAddr.
    masm.subPtr(Imm32(2 * sizeof(uintptr_t)), StackPointer);
    masm.as_mflr(ScratchRegister);
    masm.storePtr(ScratchRegister, Address(StackPointer, sizeof(uintptr_t)));
    masm.storePtr(scratch1, Address(StackPointer, 0));

    EmitBaselineCallVM(code, masm);

    EmitBaselineLeaveStubFrame(masm);

    // If the stub returns |true|, we have to perform a forced return.
    // If |false|, just return from the trap stub.
    Label forcedReturn;
    masm.branchTest32(Assembler::NonZero, ReturnReg, ReturnReg, &forcedReturn);

    // LR was restored by EmitBaselineLeaveStubFrame, return to trap site
    masm.ret();

    masm.bind(&forcedReturn);
    masm.loadValue(Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfReturnValue()),
                   JSReturnOperand);
    masm.movePtr(BaselineFrameReg, StackPointer);
    masm.pop(BaselineFrameReg);

    // Update profiling state before returning
    {
        Label skipProfilingInstrumentation;
        AbsoluteAddress addressOfEnabled(cx->runtime()->spsProfiler.addressOfEnabled());
        masm.branch32(Assembler::Equal, addressOfEnabled, Imm32(0), &skipProfilingInstrumentation);
        masm.profilerExitFrame();
        masm.bind(&skipProfilingInstrumentation);
    }

    masm.ret();

    Linker linker(masm);
    JitCode* codeDbg = linker.newCode<NoGC>(cx, OTHER_CODE);
    return codeDbg;
}

JitCode*
JitRuntime::generateExceptionTailStub(JSContext* cx, void* handler)
{
    MacroAssembler masm(cx);

    masm.handleFailureWithHandlerTail(handler);

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);
    return code;
}

JitCode*
JitRuntime::generateBailoutTailStub(JSContext* cx)
{
    MacroAssembler masm(cx);

    // r4 = scratch, r5 = bailoutInfo (matching handleFailureWithHandlerTail)
    masm.generateBailoutTail(r4, r5);

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);
    return code;
}

JitCode*
JitRuntime::generateProfilerExitFrameTailStub(JSContext* cx)
{
    MacroAssembler masm(cx);

    // Use volatile registers as scratch (safe in JIT stubs)
    Register scratch1 = r5;
    Register scratch2 = r6;
    Register scratch3 = r7;
    Register scratch4 = r8;

    Register actReg = scratch4;
    AbsoluteAddress activationAddr(GetJitContext()->runtime->addressOfProfilingActivation());
    masm.loadPtr(activationAddr, actReg);

    Address lastProfilingFrame(actReg, JitActivation::offsetOfLastProfilingFrame());
    Address lastProfilingCallSite(actReg, JitActivation::offsetOfLastProfilingCallSite());

#ifdef DEBUG
    {
        masm.loadPtr(lastProfilingFrame, scratch1);
        Label checkOk;
        masm.branchPtr(Assembler::Equal, scratch1, ImmWord(0), &checkOk);
        masm.branchPtr(Assembler::Equal, StackPointer, scratch1, &checkOk);
        masm.assumeUnreachable(
            "Mismatch between stored lastProfilingFrame and current stack pointer.");
        masm.bind(&checkOk);
    }
#endif

    // Load frame descriptor, extract type and size
    masm.loadPtr(Address(StackPointer, JitFrameLayout::offsetOfDescriptor()), scratch1);

    masm.ma_and(scratch2, scratch1, Imm32((1 << FRAMETYPE_BITS) - 1));
    masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch1);

    Label handle_IonJS;
    Label handle_BaselineStub;
    Label handle_Rectifier;
    Label handle_IonAccessorIC;
    Label handle_Entry;
    Label end;

    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_IonJS), &handle_IonJS);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_BaselineJS), &handle_IonJS);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_BaselineStub), &handle_BaselineStub);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_Rectifier), &handle_Rectifier);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_IonAccessorIC), &handle_IonAccessorIC);
    masm.branch32(Assembler::Equal, scratch2, Imm32(JitFrame_Entry), &handle_Entry);

    masm.assumeUnreachable("Invalid caller frame type when exiting from Ion frame.");

    //
    // JitFrame_IonJS / JitFrame_BaselineJS
    //
    masm.bind(&handle_IonJS);
    {
        masm.loadPtr(Address(StackPointer, JitFrameLayout::offsetOfReturnAddress()), scratch2);
        masm.storePtr(scratch2, lastProfilingCallSite);

        masm.as_add(scratch2, StackPointer, scratch1);
        masm.addPtr(Imm32(JitFrameLayout::Size()), scratch2);
        masm.storePtr(scratch2, lastProfilingFrame);
        masm.ret();
    }

    //
    // JitFrame_BaselineStub
    //
    masm.bind(&handle_BaselineStub);
    {
        masm.as_add(scratch3, StackPointer, scratch1);

        Address stubFrameReturnAddr(scratch3,
                                    JitFrameLayout::Size() +
                                    BaselineStubFrameLayout::offsetOfReturnAddress());
        masm.loadPtr(stubFrameReturnAddr, scratch2);
        masm.storePtr(scratch2, lastProfilingCallSite);

        Address stubFrameSavedFramePtr(scratch3,
                                       JitFrameLayout::Size() - (2 * sizeof(void*)));
        masm.loadPtr(stubFrameSavedFramePtr, scratch2);
        masm.addPtr(Imm32(sizeof(void*)), scratch2);
        masm.storePtr(scratch2, lastProfilingFrame);
        masm.ret();
    }

    //
    // JitFrame_Rectifier
    //
    masm.bind(&handle_Rectifier);
    {
        masm.as_add(scratch2, StackPointer, scratch1);
        masm.addPtr(Imm32(JitFrameLayout::Size()), scratch2);

        masm.loadPtr(Address(scratch2, RectifierFrameLayout::offsetOfDescriptor()), scratch3);
        masm.ma_srl(scratch1, scratch3, Imm32(FRAMESIZE_SHIFT));
        masm.and32(Imm32((1 << FRAMETYPE_BITS) - 1), scratch3);

        Label handle_Rectifier_BaselineStub;
        masm.branch32(Assembler::NotEqual, scratch3, Imm32(JitFrame_IonJS),
                      &handle_Rectifier_BaselineStub);

        // Rectifier <- IonJS
        masm.loadPtr(Address(scratch2, RectifierFrameLayout::offsetOfReturnAddress()), scratch3);
        masm.storePtr(scratch3, lastProfilingCallSite);

        masm.as_add(scratch3, scratch2, scratch1);
        masm.addPtr(Imm32(RectifierFrameLayout::Size()), scratch3);
        masm.storePtr(scratch3, lastProfilingFrame);
        masm.ret();

        // Rectifier <- BaselineStub <- BaselineJS
        masm.bind(&handle_Rectifier_BaselineStub);
#ifdef DEBUG
        {
            Label checkOk;
            masm.branch32(Assembler::Equal, scratch3, Imm32(JitFrame_BaselineStub), &checkOk);
            masm.assumeUnreachable("Unrecognized frame preceding baselineStub.");
            masm.bind(&checkOk);
        }
#endif
        masm.as_add(scratch3, scratch2, scratch1);
        Address convergentRetAddr(scratch3, RectifierFrameLayout::Size() +
                                            BaselineStubFrameLayout::offsetOfReturnAddress());
        masm.loadPtr(convergentRetAddr, scratch2);
        masm.storePtr(scratch2, lastProfilingCallSite);

        Address convergentFramePtr(scratch3,
                                   RectifierFrameLayout::Size() - (2 * sizeof(void*)));
        masm.loadPtr(convergentFramePtr, scratch2);
        masm.addPtr(Imm32(sizeof(void*)), scratch2);
        masm.storePtr(scratch2, lastProfilingFrame);
        masm.ret();
    }

    //
    // JitFrame_IonAccessorIC
    //
    masm.bind(&handle_IonAccessorIC);
    {
        masm.as_add(scratch2, StackPointer, scratch1);
        masm.addPtr(Imm32(JitFrameLayout::Size()), scratch2);

        masm.loadPtr(Address(scratch2, IonAccessorICFrameLayout::offsetOfDescriptor()), scratch3);
#ifdef DEBUG
        {
            masm.movePtr(scratch3, scratch1);
            masm.and32(Imm32((1 << FRAMETYPE_BITS) - 1), scratch1);
            Label checkOk;
            masm.branch32(Assembler::Equal, scratch1, Imm32(JitFrame_IonJS), &checkOk);
            masm.assumeUnreachable("IonAccessorIC frame must be preceded by IonJS frame");
            masm.bind(&checkOk);
        }
#endif
        masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), scratch3);

        masm.loadPtr(Address(scratch2, IonAccessorICFrameLayout::offsetOfReturnAddress()), scratch1);
        masm.storePtr(scratch1, lastProfilingCallSite);

        masm.as_add(scratch1, scratch2, scratch3);
        masm.addPtr(Imm32(IonAccessorICFrameLayout::Size()), scratch1);
        masm.storePtr(scratch1, lastProfilingFrame);
        masm.ret();
    }

    //
    // JitFrame_Entry
    //
    masm.bind(&handle_Entry);
    {
        masm.movePtr(ImmPtr(nullptr), scratch1);
        masm.storePtr(scratch1, lastProfilingCallSite);
        masm.storePtr(scratch1, lastProfilingFrame);
        masm.ret();
    }

    Linker linker(masm);
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);
    return code;
}
