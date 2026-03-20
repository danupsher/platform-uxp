/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#include "jit/ppc/BaselineCompiler-ppc.h"
using namespace js;
using namespace js::jit;
BaselineCompilerPPC::BaselineCompilerPPC(JSContext* cx, TempAllocator& alloc, JSScript* script)
  : BaselineCompilerShared(cx, alloc, script)
{ }
