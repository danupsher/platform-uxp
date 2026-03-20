/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#ifndef jit_ppc_MoveEmitter_ppc_h
#define jit_ppc_MoveEmitter_ppc_h
#include "jit/MacroAssembler.h"
#include "jit/MoveResolver.h"
namespace js { namespace jit {
class MoveEmitterPPC
{
    MacroAssembler& masm;
    uint32_t pushedAtCycle_;
    bool inCycle_;
    Address cycleSlot() const {
        return Address(StackPointer, 0);
    }
  public:
    explicit MoveEmitterPPC(MacroAssembler& masm)
      : masm(masm), pushedAtCycle_(0), inCycle_(false)
    { }
    ~MoveEmitterPPC() { }
    void emit(const MoveResolver& moves);
    void finish();
    void setScratchRegister(Register reg) { }
  private:
    void emitMove(const MoveOperand& from, const MoveOperand& to);
    void emitDoubleMove(const MoveOperand& from, const MoveOperand& to);
    void emitFloat32Move(const MoveOperand& from, const MoveOperand& to);
    void emitInt32Move(const MoveOperand& from, const MoveOperand& to);
    void emitGeneralMove(const MoveOperand& from, const MoveOperand& to);
    void breakCycle(const MoveOperand& from, const MoveOperand& to, MoveOp::Type type, uint32_t slot);
    void completeCycle(const MoveOperand& from, const MoveOperand& to, MoveOp::Type type, uint32_t slot);
    Address toAddress(const MoveOperand& operand) const;
};
typedef MoveEmitterPPC MoveEmitter;
} }
#endif
