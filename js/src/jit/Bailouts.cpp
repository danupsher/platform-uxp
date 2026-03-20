/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Bailouts.h"
#include <cstdio>
#ifdef JS_CODEGEN_PPC
#include <signal.h>
#include <sys/ucontext.h>

static void ppc_crash_handler(int sig, siginfo_t* info, void* ctx) {
    ucontext_t* uc = (ucontext_t*)ctx;
    const char* signame = (sig == SIGSEGV) ? "SIGSEGV" : (sig == SIGBUS) ? "SIGBUS" : "SIGNAL";
#if defined(__APPLE__)
    if (uc && uc->uc_mcontext) {
        ppc_thread_state_t* ss = &uc->uc_mcontext->ss;
        fprintf(stderr, "PPC-CRASH: %s addr=%p PC=%08x LR=%08x CTR=%08x CR=%08x XER=%08x\n",
                signame, info->si_addr, ss->srr0, ss->lr, ss->ctr, ss->cr, ss->xer);
        fprintf(stderr, "  r0=%08x  r1=%08x  r2=%08x  r3=%08x  r4=%08x  r5=%08x  r6=%08x  r7=%08x\n",
                ss->r0, ss->r1, ss->r2, ss->r3, ss->r4, ss->r5, ss->r6, ss->r7);
        fprintf(stderr, "  r8=%08x  r9=%08x r10=%08x r11=%08x r12=%08x r13=%08x r14=%08x r15=%08x\n",
                ss->r8, ss->r9, ss->r10, ss->r11, ss->r12, ss->r13, ss->r14, ss->r15);
        fprintf(stderr, " r16=%08x r17=%08x r18=%08x r19=%08x r20=%08x r21=%08x r22=%08x r23=%08x\n",
                ss->r16, ss->r17, ss->r18, ss->r19, ss->r20, ss->r21, ss->r22, ss->r23);
        fprintf(stderr, " r24=%08x r25=%08x r26=%08x r27=%08x r28=%08x r29=%08x r30=%08x r31=%08x\n",
                ss->r24, ss->r25, ss->r26, ss->r27, ss->r28, ss->r29, ss->r30, ss->r31);
        // Dump 32 instructions before and 8 after PC
        unsigned int* pc_ptr = (unsigned int*)(ss->srr0);
        fprintf(stderr, "  code @ PC-256..PC+128:\n");
        for (int ii = -64; ii < 32; ii++) {
            fprintf(stderr, "    %s[%+4d] %08x\n",
                    ii == 0 ? ">>>" : "   ", ii*4, pc_ptr[ii]);
        }
        // Dump code around the caller (LR)
        unsigned int* lr_ptr = (unsigned int*)(ss->lr);
        fprintf(stderr, "  caller code @ LR-64..LR+32:\n");
        for (int ii = -16; ii < 8; ii++) {
            fprintf(stderr, "    %s[%+4d] %08x\n",
                    ii == 0 ? ">>>" : "   ", ii*4, lr_ptr[ii]);
        }
        // Walk stack frames: on PPC, r1 points to the stack frame,
        // [r1+0] = back chain (previous frame's SP)
        // [r1+8] = saved LR (return address) in the CALLER's frame
        fprintf(stderr, "  stack walk (frame -> saved LR):\n");
        unsigned int* fp = (unsigned int*)(ss->r1);
        for (int frame = 0; frame < 20 && fp; frame++) {
            unsigned int back_chain = fp[0];
            // On PPC Darwin, LR is saved at offset 8 from the frame pointer
            unsigned int saved_lr = 0;
            if (back_chain > 0x10000 && back_chain < 0xc0000000) {
                unsigned int* caller_frame = (unsigned int*)back_chain;
                saved_lr = caller_frame[2]; // offset 8 = LR save area
            }
            fprintf(stderr, "    [%2d] SP=%08x LR=%08x\n",
                    frame, (unsigned)fp, saved_lr);
            if (back_chain < 0x10000 || back_chain > 0xc0000000 || back_chain <= (unsigned int)fp)
                break;
            fp = (unsigned int*)back_chain;
        }
        // Dump stack frame around SP to see Ion frame slots
        {
            // SP was modified by stub (stwu), so caller's SP = r1+4
            unsigned int callerSP = ss->r1 + 4;
            unsigned int* sp = (unsigned int*)callerSP;
            fprintf(stderr, "  stack frame (callerSP=%08x) slots 0x00..0xA0:\n", callerSP);
            for (int si = 0; si < 42; si++) {
                fprintf(stderr, "    [SP+%02x] %08x\n", si*4, sp[si]);
            }
            fflush(stderr);
        }
        // Dump memory around r3 to see JIT frame contents
        if (ss->r3 > 0x10000 && ss->r3 < 0xc0000000) {
            fprintf(stderr, "  mem @ r3=%08x:\n", ss->r3);
            unsigned int* mp = (unsigned int*)(ss->r3);
            for (int mi = -4; mi < 8; mi++) {
                fprintf(stderr, "    [r3%+3d] %08x\n", mi*4, mp[mi]);
            }
        }
        if (ss->r4 < 0x10000 && ss->r4 != 0) {
            fprintf(stderr, "  r4=%08x looks like int32 %d used as object ptr\n",
                    ss->r4, (int)ss->r4);
        }
    } else
#endif
    {
        fprintf(stderr, "PPC-CRASH: %s addr=%p (no context)\n", signame, info->si_addr);
    }
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void ppc_sample_handler(int sig, siginfo_t* info, void* ctx) {
    ucontext_t* uc = (ucontext_t*)ctx;
#if defined(__APPLE__)
    if (uc && uc->uc_mcontext) {
        ppc_thread_state_t* ss = &uc->uc_mcontext->ss;
        fprintf(stderr, "PPC-SAMPLE: PC=%08x LR=%08x SP=%08x r0=%08x r3=%08x r12=%08x r29=%08x r31=%08x\n",
                ss->srr0, ss->lr, ss->r1, ss->r0, ss->r3, ss->r12, ss->r29, ss->r31);
        fflush(stderr);
    }
#endif
}

__attribute__((constructor)) static void install_ppc_crash_handler() {
    struct sigaction sa;
    sa.sa_sigaction = ppc_crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    // SIGUSR1 for non-destructive PC sampling
    struct sigaction sa2;
    sa2.sa_sigaction = ppc_sample_handler;
    sa2.sa_flags = SA_SIGINFO;
    sigemptyset(&sa2.sa_mask);
    sigaction(SIGUSR1, &sa2, nullptr);
    fprintf(stderr, "PPC-CRASH: handler installed handler=%p\n", (void*)ppc_crash_handler);
    fflush(stderr);
}
#endif
#if defined(JS_CODEGEN_PPC)
#include "jit/ppc/Bailouts-ppc.h"
#endif

#include "mozilla/ScopeExit.h"

#include "jscntxt.h"

#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "jit/JitCompartment.h"
#include "jit/JitSpewer.h"
#include "jit/Snapshots.h"
#include "vm/TraceLogging.h"

#include "jit/JitFrameIterator-inl.h"
#include "vm/Probes-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::IsInRange;

uint32_t
jit::Bailout(BailoutStack* sp, BaselineBailoutInfo** bailoutInfo)
{
#if defined(JS_CODEGEN_PPC)
    fprintf(stderr, "PPC-BAILOUT: sizeof(BailoutStack)=%zu bailoutDataSize would be %zu\n",
            sizeof(BailoutStack), sizeof(BailoutStack) - 2 * sizeof(uintptr_t));
    fprintf(stderr, "PPC-BAILOUT: sp=%p frameSize=%u snapshot=%u parent=%p\n",
            (void*)sp,
            (unsigned)sp->frameSize(),
            (unsigned)sp->snapshotOffset(),
            (void*)sp->parentStackPointer());
    // Dump first 4 words at parentStackPointer
    uint32_t* pp = (uint32_t*)sp->parentStackPointer();
    fprintf(stderr, "PPC-BAILOUT: [parent+0]=%08x [+4]=%08x [+8]=%08x [+C]=%08x\n",
            pp[0], pp[1], pp[2], pp[3]);
#endif
    JSContext* cx = GetJSContextFromMainThread();
    MOZ_ASSERT(bailoutInfo);

    // We don't have an exit frame.
    MOZ_ASSERT(IsInRange(FAKE_JIT_TOP_FOR_BAILOUT, 0, 0x1000) &&
               IsInRange(FAKE_JIT_TOP_FOR_BAILOUT + sizeof(CommonFrameLayout), 0, 0x1000),
               "Fake jitTop pointer should be within the first page.");
    cx->runtime()->jitTop = FAKE_JIT_TOP_FOR_BAILOUT;

    JitActivationIterator jitActivations(cx->runtime());
    BailoutFrameInfo bailoutData(jitActivations, sp);
#ifdef JS_CODEGEN_PPC
    // PPC: If BailoutFrameInfo detected a corrupted frame (bad calleeToken),
    // topIonScript_ will be null. Bail out gracefully instead of crashing.
    if (!bailoutData.ionScript()) {
        fprintf(stderr, "PPC-BAILOUT: corrupted frame, returning FATAL_ERROR\n");
        fflush(stderr);
        *bailoutInfo = nullptr;
        return BAILOUT_RETURN_FATAL_ERROR;
    }
    {
        JSScript* bscript = ScriptFromCalleeToken(((JitFrameLayout*)bailoutData.fp())->calleeToken());
        const char* fn = bscript->filename();
        fprintf(stderr, "PPC-BAILOUT: script=%s:%u frameSize=%u\n",
                fn ? fn : "?",
                (unsigned)bscript->lineno(),
                (unsigned)bailoutData.topFrameSize());
        fflush(stderr);
    }
#endif
    JitFrameIterator iter(jitActivations);
    MOZ_ASSERT(!iter.ionScript()->invalidated());
    CommonFrameLayout* currentFramePtr = iter.current();

    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    TraceLogTimestamp(logger, TraceLogger_Bailout);

    JitSpew(JitSpew_IonBailouts, "Took bailout! Snapshot offset: %d", iter.snapshotOffset());
#ifdef JS_CODEGEN_PPC
    {
        RootedScript script(cx, iter.script());
        if (script) {
            const char* fn = script->filename();
            if (fn && strcmp(fn, "self-hosted") == 0) {
                fprintf(stderr, "ION-SH-BAILOUT: %s:%u snapshot=%d\n",
                        fn, (unsigned)script->lineno(), iter.snapshotOffset());
                fflush(stderr);
            }
        }
    }
#endif

    MOZ_ASSERT(IsBaselineEnabled(cx));

    *bailoutInfo = nullptr;
    uint32_t retval = BailoutIonToBaseline(cx, bailoutData.activation(), iter, false, bailoutInfo,
                                           /* excInfo = */ nullptr);

    MOZ_ASSERT(retval == BAILOUT_RETURN_OK ||
               retval == BAILOUT_RETURN_FATAL_ERROR ||
               retval == BAILOUT_RETURN_OVERRECURSED);
    MOZ_ASSERT_IF(retval == BAILOUT_RETURN_OK, *bailoutInfo != nullptr);



    if (retval != BAILOUT_RETURN_OK) {
        JSScript* script = iter.script();
        probes::ExitScript(cx, script, script->functionNonDelazifying(),
                           /* popSPSFrame = */ false);
    }

    // This condition was wrong when we entered this bailout function, but it
    // might be true now. A GC might have reclaimed all the Jit code and
    // invalidated all frames which are currently on the stack. As we are
    // already in a bailout, we could not switch to an invalidation
    // bailout. When the code of an IonScript which is on the stack is
    // invalidated (see InvalidateActivation), we remove references to it and
    // increment the reference counter for each activation that appear on the
    // stack. As the bailed frame is one of them, we have to decrement it now.
    if (iter.ionScript()->invalidated())
        iter.ionScript()->decrementInvalidationCount(cx->runtime()->defaultFreeOp());

    // NB: Commentary on how |lastProfilingFrame| is set from bailouts.
    //
    // Once we return to jitcode, any following frames might get clobbered,
    // but the current frame will not (as it will be clobbered "in-place"
    // with a baseline frame that will share the same frame prefix).
    // However, there may be multiple baseline frames unpacked from this
    // single Ion frame, which means we will need to once again reset
    // |lastProfilingFrame| to point to the correct unpacked last frame
    // in |FinishBailoutToBaseline|.
    //
    // In the case of error, the jitcode will jump immediately to an
    // exception handler, which will unwind the frames and properly set
    // the |lastProfilingFrame| to point to the frame being resumed into
    // (see |AutoResetLastProfilerFrameOnReturnFromException|).
    //
    // In both cases, we want to temporarily set the |lastProfilingFrame|
    // to the current frame being bailed out, and then fix it up later.
    if (cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(cx->runtime()))
        cx->runtime()->jitActivation->setLastProfilingFrame(currentFramePtr);

    return retval;
}

uint32_t
jit::InvalidationBailout(InvalidationBailoutStack* sp, size_t* frameSizeOut,
                         BaselineBailoutInfo** bailoutInfo)
{
    sp->checkInvariants();

    JSContext* cx = GetJSContextFromMainThread();

    // We don't have an exit frame.
    cx->runtime()->jitTop = FAKE_JIT_TOP_FOR_BAILOUT;

    JitActivationIterator jitActivations(cx->runtime());
    BailoutFrameInfo bailoutData(jitActivations, sp);
    JitFrameIterator iter(jitActivations);
    CommonFrameLayout* currentFramePtr = iter.current();

    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    TraceLogTimestamp(logger, TraceLogger_Invalidation);

    JitSpew(JitSpew_IonBailouts, "Took invalidation bailout! Snapshot offset: %d", iter.snapshotOffset());

    // Note: the frame size must be computed before we return from this function.
    *frameSizeOut = iter.frameSize();

    MOZ_ASSERT(IsBaselineEnabled(cx));

    *bailoutInfo = nullptr;
    uint32_t retval = BailoutIonToBaseline(cx, bailoutData.activation(), iter, true, bailoutInfo,
                                           /* excInfo = */ nullptr);
    MOZ_ASSERT(retval == BAILOUT_RETURN_OK ||
               retval == BAILOUT_RETURN_FATAL_ERROR ||
               retval == BAILOUT_RETURN_OVERRECURSED);
    MOZ_ASSERT_IF(retval == BAILOUT_RETURN_OK, *bailoutInfo != nullptr);



    if (retval != BAILOUT_RETURN_OK) {
        // If the bailout failed, then bailout trampoline will pop the
        // current frame and jump straight to exception handling code when
        // this function returns.  Any SPS entry pushed for this frame will
        // be silently forgotten.
        //
        // We call ExitScript here to ensure that if the ionScript had SPS
        // instrumentation, then the SPS entry for it is popped.
        //
        // However, if the bailout was during argument check, then a
        // pseudostack frame would not have been pushed in the first
        // place, so don't pop anything in that case.
        JSScript* script = iter.script();
        probes::ExitScript(cx, script, script->functionNonDelazifying(),
                           /* popSPSFrame = */ false);

#ifdef JS_JITSPEW
        JitFrameLayout* frame = iter.jsFrame();
        JitSpew(JitSpew_IonInvalidate, "Bailout failed (%s)",
                (retval == BAILOUT_RETURN_FATAL_ERROR) ? "Fatal Error" : "Over Recursion");
        JitSpew(JitSpew_IonInvalidate, "   calleeToken %p", (void*) frame->calleeToken());
        JitSpew(JitSpew_IonInvalidate, "   frameSize %u", unsigned(frame->prevFrameLocalSize()));
        JitSpew(JitSpew_IonInvalidate, "   ra %p", (void*) frame->returnAddress());
#endif
    }

    iter.ionScript()->decrementInvalidationCount(cx->runtime()->defaultFreeOp());

    // Make the frame being bailed out the top profiled frame.
    if (cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(cx->runtime()))
        cx->runtime()->jitActivation->setLastProfilingFrame(currentFramePtr);

    return retval;
}

BailoutFrameInfo::BailoutFrameInfo(const JitActivationIterator& activations,
                                   const JitFrameIterator& frame)
  : machine_(frame.machineState())
{
    framePointer_ = (uint8_t*) frame.fp();
    topFrameSize_ = frame.frameSize();
    topIonScript_ = frame.ionScript();
    attachOnJitActivation(activations);

    const OsiIndex* osiIndex = frame.osiIndex();
    snapshotOffset_ = osiIndex->snapshotOffset();
}

uint32_t
jit::ExceptionHandlerBailout(JSContext* cx, const InlineFrameIterator& frame,
                             ResumeFromException* rfe,
                             const ExceptionBailoutInfo& excInfo,
                             bool* overrecursed)
{
    // We can be propagating debug mode exceptions without there being an
    // actual exception pending. For instance, when we return false from an
    // operation callback like a timeout handler.
    MOZ_ASSERT_IF(!excInfo.propagatingIonExceptionForDebugMode(), cx->isExceptionPending());

    uint8_t* prevJitTop = cx->runtime()->jitTop;
    auto restoreJitTop = mozilla::MakeScopeExit([&]() { cx->runtime()->jitTop = prevJitTop; });
    cx->runtime()->jitTop = FAKE_JIT_TOP_FOR_BAILOUT;

    gc::AutoSuppressGC suppress(cx);

    JitActivationIterator jitActivations(cx->runtime());
    BailoutFrameInfo bailoutData(jitActivations, frame.frame());
    JitFrameIterator iter(jitActivations);
    CommonFrameLayout* currentFramePtr = iter.current();

    BaselineBailoutInfo* bailoutInfo = nullptr;
    uint32_t retval;

    {
        // Currently we do not tolerate OOM here so as not to complicate the
        // exception handling code further.
        AutoEnterOOMUnsafeRegion oomUnsafe;

        retval = BailoutIonToBaseline(cx, bailoutData.activation(), iter, true,
                                      &bailoutInfo, &excInfo);
        if (retval == BAILOUT_RETURN_FATAL_ERROR && cx->isThrowingOutOfMemory())
            oomUnsafe.crash("ExceptionHandlerBailout");
    }

    if (retval == BAILOUT_RETURN_OK) {
        MOZ_ASSERT(bailoutInfo);

        // Overwrite the kind so HandleException after the bailout returns
        // false, jumping directly to the exception tail.
        if (excInfo.propagatingIonExceptionForDebugMode())
            bailoutInfo->bailoutKind = Bailout_IonExceptionDebugMode;

        rfe->kind = ResumeFromException::RESUME_BAILOUT;
        rfe->target = cx->runtime()->jitRuntime()->getBailoutTail()->raw();
        rfe->bailoutInfo = bailoutInfo;
    } else {
        // Bailout failed. If the overrecursion check failed, clear the
        // exception to turn this into an uncatchable error, continue popping
        // all inline frames and have the caller report the error.
        MOZ_ASSERT(!bailoutInfo);

        if (retval == BAILOUT_RETURN_OVERRECURSED) {
            *overrecursed = true;
            if (!excInfo.propagatingIonExceptionForDebugMode())
                cx->clearPendingException();
        } else {
            MOZ_ASSERT(retval == BAILOUT_RETURN_FATAL_ERROR);

            // Crash for now so as not to complicate the exception handling code
            // further.
            MOZ_CRASH();
        }
    }

    // Make the frame being bailed out the top profiled frame.
    if (cx->runtime()->jitRuntime()->isProfilerInstrumentationEnabled(cx->runtime()))
        cx->runtime()->jitActivation->setLastProfilingFrame(currentFramePtr);

    return retval;
}

// Initialize the decl env Object, call object, and any arguments obj of the
// current frame.
bool
jit::EnsureHasEnvironmentObjects(JSContext* cx, AbstractFramePtr fp)
{
    // Ion does not compile eval scripts.
    MOZ_ASSERT(!fp.isEvalFrame());

    if (fp.isFunctionFrame()) {
        // Ion does not handle extra var environments due to parameter
        // expressions yet.
        MOZ_ASSERT(!fp.callee()->needsExtraBodyVarEnvironment());

        if (!fp.hasInitialEnvironment() && fp.callee()->needsFunctionEnvironmentObjects()) {
            if (!fp.initFunctionEnvironmentObjects(cx))
                return false;
        }
    }

    return true;
}

void
jit::CheckFrequentBailouts(JSContext* cx, JSScript* script, BailoutKind bailoutKind)
{
    if (script->hasIonScript()) {
        // Invalidate if this script keeps bailing out without invalidation. Next time
        // we compile this script LICM will be disabled.
        IonScript* ionScript = script->ionScript();

        if (ionScript->bailoutExpected()) {
            // If we bailout because of the first execution of a basic block,
            // then we should record which basic block we are returning in,
            // which should prevent this from happening again.  Also note that
            // the first execution bailout can be related to an inlined script,
            // so there is no need to penalize the caller.
#ifdef JS_CODEGEN_PPC
            if (!script->hadFrequentBailouts())
#else
            if (bailoutKind != Bailout_FirstExecution && !script->hadFrequentBailouts())
#endif
                script->setHadFrequentBailouts();

            JitSpew(JitSpew_IonInvalidate, "Invalidating due to too many bailouts");

#ifdef JS_CODEGEN_PPC
            // PPC: Invalidate() walks the stack and patches return addresses during bailout,
            // which corrupts frames and causes double-bailout crashes.
            // Use ForbidCompilation() instead — it prevents recompilation without walking the stack.
            jit::ForbidCompilation(cx, script);
#else
            Invalidate(cx, script);
#endif
        }
    }
}

void
BailoutFrameInfo::attachOnJitActivation(const JitActivationIterator& jitActivations)
{
    MOZ_ASSERT(jitActivations.jitTop() == FAKE_JIT_TOP_FOR_BAILOUT);
    activation_ = jitActivations->asJit();
    activation_->setBailoutData(this);
}

BailoutFrameInfo::~BailoutFrameInfo()
{
    if (activation_)
        activation_->cleanBailoutData();
}
