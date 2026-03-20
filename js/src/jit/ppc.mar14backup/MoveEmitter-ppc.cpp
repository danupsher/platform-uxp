/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#include "jit/ppc/MoveEmitter-ppc.h"
#include "jit/MacroAssembler-inl.h"
using namespace js;
using namespace js::jit;
Address MoveEmitterPPC::toAddress(const MoveOperand& operand) const {
    if (operand.isMemoryOrEffectiveAddress())
        return Address(operand.base(), operand.disp());
    MOZ_CRASH("unexpected operand");
}
void MoveEmitterPPC::emit(const MoveResolver& moves) {
    for (size_t i = 0; i < moves.numMoves(); i++) {
        const MoveOp& move = moves.getMove(i);
        const MoveOperand& from = move.from();
        const MoveOperand& to = move.to();
        if (move.isCycleBegin()) {
            breakCycle(from, to, move.type(), i);
        }
        switch (move.type()) {
          case MoveOp::FLOAT32: emitFloat32Move(from, to); break;
          case MoveOp::DOUBLE:  emitDoubleMove(from, to); break;
          case MoveOp::INT32:   emitInt32Move(from, to); break;
          case MoveOp::GENERAL: emitGeneralMove(from, to); break;
          default: MOZ_CRASH("unexpected move type");
        }
        if (move.isCycleEnd())
            completeCycle(from, to, move.type(), i);
    }
}
void MoveEmitterPPC::finish() { }
void MoveEmitterPPC::emitMove(const MoveOperand& from, const MoveOperand& to) {
    emitGeneralMove(from, to);
}
void MoveEmitterPPC::emitGeneralMove(const MoveOperand& from, const MoveOperand& to) {
    if (from.isEffectiveAddress()) {
        // LEA: compute base + disp, don't load from memory
        if (to.isGeneralReg()) {
            if (from.disp() == 0)
                masm.ma_move(to.reg(), from.base());
            else
                masm.ma_addu(to.reg(), from.base(), Imm32(from.disp()));
        } else {
            if (from.disp() == 0)
                masm.ma_sw(from.base(), toAddress(to));
            else {
                masm.ma_addu(ScratchRegister, from.base(), Imm32(from.disp()));
                masm.ma_sw(ScratchRegister, toAddress(to));
            }
        }
    } else if (from.isGeneralReg()) {
        if (to.isGeneralReg())
            masm.ma_move(to.reg(), from.reg());
        else
            masm.ma_sw(from.reg(), toAddress(to));
    } else {
        if (to.isGeneralReg())
            masm.ma_lw(to.reg(), toAddress(from));
        else {
            masm.ma_lw(ScratchRegister, toAddress(from));
            masm.ma_sw(ScratchRegister, toAddress(to));
        }
    }
}
void MoveEmitterPPC::emitInt32Move(const MoveOperand& from, const MoveOperand& to) {
    emitGeneralMove(from, to);
}
void MoveEmitterPPC::emitDoubleMove(const MoveOperand& from, const MoveOperand& to) {
    if (from.isFloatReg()) {
        if (to.isFloatReg())
            masm.as_fmr(to.floatReg(), from.floatReg());
        else
            masm.ma_sd(from.floatReg(), toAddress(to));
    } else {
        if (to.isFloatReg())
            masm.ma_ld(to.floatReg(), toAddress(from));
        else {
            masm.ma_ld(ScratchDoubleReg, toAddress(from));
            masm.ma_sd(ScratchDoubleReg, toAddress(to));
        }
    }
}
void MoveEmitterPPC::emitFloat32Move(const MoveOperand& from, const MoveOperand& to) {
    if (from.isFloatReg()) {
        if (to.isFloatReg())
            masm.as_fmr(to.floatReg(), from.floatReg());
        else
            masm.ma_ss(from.floatReg(), toAddress(to));
    } else {
        if (to.isFloatReg())
            masm.ma_ls(to.floatReg(), toAddress(from));
        else {
            masm.ma_ls(ScratchFloat32Reg, toAddress(from));
            masm.ma_ss(ScratchFloat32Reg, toAddress(to));
        }
    }
}
void MoveEmitterPPC::breakCycle(const MoveOperand& from, const MoveOperand& to, MoveOp::Type type, uint32_t slot) {
    if (!inCycle_) {
        masm.subFromStackPtr(Imm32(sizeof(double)));
        inCycle_ = true;
        pushedAtCycle_ = masm.size();
    }
    switch (type) {
      case MoveOp::FLOAT32: masm.ma_ss(to.floatReg(), cycleSlot()); break;
      case MoveOp::DOUBLE:  masm.ma_sd(to.floatReg(), cycleSlot()); break;
      case MoveOp::INT32:
      case MoveOp::GENERAL: masm.ma_sw(to.reg(), cycleSlot()); break;
      default: MOZ_CRASH("unexpected type");
    }
}
void MoveEmitterPPC::completeCycle(const MoveOperand& from, const MoveOperand& to, MoveOp::Type type, uint32_t slot) {
    switch (type) {
      case MoveOp::FLOAT32: masm.ma_ls(to.floatReg(), cycleSlot()); break;
      case MoveOp::DOUBLE:  masm.ma_ld(to.floatReg(), cycleSlot()); break;
      case MoveOp::INT32:
      case MoveOp::GENERAL: masm.ma_lw(to.reg(), cycleSlot()); break;
      default: MOZ_CRASH("unexpected type");
    }
    if (inCycle_) {
        masm.addToStackPtr(Imm32(sizeof(double)));
        inCycle_ = false;
    }
}
