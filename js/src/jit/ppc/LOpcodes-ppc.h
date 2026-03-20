/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ppc_LOpcodes_ppc_h
#define jit_ppc_LOpcodes_ppc_h

#include "jit/shared/LOpcodes-shared.h"

#define LIR_CPU_OPCODE_LIST(_)      _(BoxFloatingPoint)             _(ModMaskI)                     _(UDivOrMod)                    _(Int64ToFloatingPoint)

#endif // jit_ppc_LOpcodes_ppc_h
