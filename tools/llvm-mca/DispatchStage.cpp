//===--------------------- DispatchStage.cpp --------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file models the dispatch component of an instruction pipeline.
///
/// The DispatchStage is responsible for updating instruction dependencies
/// and communicating to the simulated instruction scheduler that an instruction
/// is ready to be scheduled for execution.
///
//===----------------------------------------------------------------------===//

#include "DispatchStage.h"
#include "HWEventListener.h"
#include "Scheduler.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "llvm-mca"

namespace mca {

void DispatchStage::notifyInstructionDispatched(const InstRef &IR,
                                                ArrayRef<unsigned> UsedRegs) {
  LLVM_DEBUG(dbgs() << "[E] Instruction Dispatched: " << IR << '\n');
  notifyInstructionEvent(HWInstructionDispatchedEvent(IR, UsedRegs));
}

void DispatchStage::notifyStallEvent(const HWStallEvent &Event) {
  for (HWEventListener *Listener : getListeners())
    Listener->onStallEvent(Event);
}

bool DispatchStage::checkPRF(const InstRef &IR) {
  SmallVector<unsigned, 4> RegDefs;
  for (const std::unique_ptr<WriteState> &RegDef :
       IR.getInstruction()->getDefs())
    RegDefs.emplace_back(RegDef->getRegisterID());

  const unsigned RegisterMask = PRF.isAvailable(RegDefs);
  // A mask with all zeroes means: register files are available.
  if (RegisterMask) {
    notifyStallEvent(HWStallEvent(HWStallEvent::RegisterFileStall, IR));
    return false;
  }

  return true;
}

bool DispatchStage::checkRCU(const InstRef &IR) {
  const unsigned NumMicroOps = IR.getInstruction()->getDesc().NumMicroOps;
  if (RCU.isAvailable(NumMicroOps))
    return true;
  notifyStallEvent(HWStallEvent(HWStallEvent::RetireControlUnitStall, IR));
  return false;
}

bool DispatchStage::checkScheduler(const InstRef &IR) {
  HWStallEvent::GenericEventType Event;
  const bool Ready = SC.canBeDispatched(IR, Event);
  if (!Ready)
    notifyStallEvent(HWStallEvent(Event, IR));
  return Ready;
}

void DispatchStage::updateRAWDependencies(ReadState &RS,
                                          const MCSubtargetInfo &STI) {
  SmallVector<WriteRef, 4> DependentWrites;

  collectWrites(DependentWrites, RS.getRegisterID());
  RS.setDependentWrites(DependentWrites.size());
  LLVM_DEBUG(dbgs() << "Found " << DependentWrites.size()
                    << " dependent writes\n");
  // We know that this read depends on all the writes in DependentWrites.
  // For each write, check if we have ReadAdvance information, and use it
  // to figure out in how many cycles this read becomes available.
  const ReadDescriptor &RD = RS.getDescriptor();
  if (!RD.HasReadAdvanceEntries) {
    for (WriteRef &WR : DependentWrites)
      WR.getWriteState()->addUser(&RS, /* ReadAdvance */ 0);
    return;
  }

  const MCSchedModel &SM = STI.getSchedModel();
  const MCSchedClassDesc *SC = SM.getSchedClassDesc(RD.SchedClassID);
  for (WriteRef &WR : DependentWrites) {
    WriteState &WS = *WR.getWriteState();
    unsigned WriteResID = WS.getWriteResourceID();
    int ReadAdvance = STI.getReadAdvanceCycles(SC, RD.UseIndex, WriteResID);
    WS.addUser(&RS, ReadAdvance);
  }
}

void DispatchStage::dispatch(InstRef IR) {
  assert(!CarryOver && "Cannot dispatch another instruction!");
  Instruction &IS = *IR.getInstruction();
  const InstrDesc &Desc = IS.getDesc();
  const unsigned NumMicroOps = Desc.NumMicroOps;
  if (NumMicroOps > DispatchWidth) {
    assert(AvailableEntries == DispatchWidth);
    AvailableEntries = 0;
    CarryOver = NumMicroOps - DispatchWidth;
  } else {
    assert(AvailableEntries >= NumMicroOps);
    AvailableEntries -= NumMicroOps;
  }

  // A dependency-breaking instruction doesn't have to wait on the register
  // input operands, and it is often optimized at register renaming stage.
  // Update RAW dependencies if this instruction is not a dependency-breaking
  // instruction. A dependency-breaking instruction is a zero-latency
  // instruction that doesn't consume hardware resources.
  // An example of dependency-breaking instruction on X86 is a zero-idiom XOR.
  if (!Desc.isZeroLatency())
    for (std::unique_ptr<ReadState> &RS : IS.getUses())
      updateRAWDependencies(*RS, STI);

  // By default, a dependency-breaking zero-latency instruction is expected to
  // be optimized at register renaming stage. That means, no physical register
  // is allocated to the instruction.
  SmallVector<unsigned, 4> RegisterFiles(PRF.getNumRegisterFiles());
  for (std::unique_ptr<WriteState> &WS : IS.getDefs())
    PRF.addRegisterWrite(WriteRef(IR.first, WS.get()), RegisterFiles,
                         !Desc.isZeroLatency());

  // Reserve slots in the RCU, and notify the instruction that it has been
  // dispatched to the schedulers for execution.
  IS.dispatch(RCU.reserveSlot(IR, NumMicroOps));

  // Notify listeners of the "instruction dispatched" event.
  notifyInstructionDispatched(IR, RegisterFiles);
}

void DispatchStage::preExecute(const InstRef &IR) {
  AvailableEntries = CarryOver >= DispatchWidth ? 0 : DispatchWidth - CarryOver;
  CarryOver = CarryOver >= DispatchWidth ? CarryOver - DispatchWidth : 0U;
}

bool DispatchStage::execute(InstRef &IR) {
  const InstrDesc &Desc = IR.getInstruction()->getDesc();
  if (!isAvailable(Desc.NumMicroOps) || !canDispatch(IR))
    return false;
  dispatch(IR);
  return true;
}

#ifndef NDEBUG
void DispatchStage::dump() const {
  PRF.dump();
  RCU.dump();
}
#endif
} // namespace mca
