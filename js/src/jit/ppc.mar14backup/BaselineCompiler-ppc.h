/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#ifndef jit_ppc_BaselineCompiler_ppc_h
#define jit_ppc_BaselineCompiler_ppc_h
#include "jit/shared/BaselineCompiler-shared.h"
namespace js { namespace jit {
class BaselineCompilerPPC : public BaselineCompilerShared {
  protected:
    BaselineCompilerPPC(JSContext* cx, TempAllocator& alloc, JSScript* script);
};
typedef BaselineCompilerPPC BaselineCompilerSpecific;
} }
#endif
