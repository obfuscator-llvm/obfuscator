//===- ScheduleDAG.cpp - Implement the ScheduleDAG class ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file Implements the ScheduleDAG class, which is a base class used by
/// scheduling implementation classes.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/ScheduleHazardRecognizer.h"
#include "llvm/CodeGen/SelectionDAGNodes.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "pre-RA-sched"

#ifndef NDEBUG
static cl::opt<bool> StressSchedOpt(
  "stress-sched", cl::Hidden, cl::init(false),
  cl::desc("Stress test instruction scheduling"));
#endif

void SchedulingPriorityQueue::anchor() {}

ScheduleDAG::ScheduleDAG(MachineFunction &mf)
    : TM(mf.getTarget()), TII(mf.getSubtarget().getInstrInfo()),
      TRI(mf.getSubtarget().getRegisterInfo()), MF(mf),
      MRI(mf.getRegInfo()) {
#ifndef NDEBUG
  StressSched = StressSchedOpt;
#endif
}

ScheduleDAG::~ScheduleDAG() = default;

void ScheduleDAG::clearDAG() {
  SUnits.clear();
  EntrySU = SUnit();
  ExitSU = SUnit();
}

const MCInstrDesc *ScheduleDAG::getNodeDesc(const SDNode *Node) const {
  if (!Node || !Node->isMachineOpcode()) return nullptr;
  return &TII->get(Node->getMachineOpcode());
}

LLVM_DUMP_METHOD
raw_ostream &SDep::print(raw_ostream &OS, const TargetRegisterInfo *TRI) const {
  switch (getKind()) {
  case Data:   OS << "Data"; break;
  case Anti:   OS << "Anti"; break;
  case Output: OS << "Out "; break;
  case Order:  OS << "Ord "; break;
  }

  switch (getKind()) {
  case Data:
    OS << " Latency=" << getLatency();
    if (TRI && isAssignedRegDep())
      OS << " Reg=" << PrintReg(getReg(), TRI);
    break;
  case Anti:
  case Output:
    OS << " Latency=" << getLatency();
    break;
  case Order:
    OS << " Latency=" << getLatency();
    switch(Contents.OrdKind) {
    case Barrier:      OS << " Barrier"; break;
    case MayAliasMem:
    case MustAliasMem: OS << " Memory"; break;
    case Artificial:   OS << " Artificial"; break;
    case Weak:         OS << " Weak"; break;
    case Cluster:      OS << " Cluster"; break;
    }
    break;
  }

  return OS;
}

bool SUnit::addPred(const SDep &D, bool Required) {
  // If this node already has this dependence, don't add a redundant one.
  for (SDep &PredDep : Preds) {
    // Zero-latency weak edges may be added purely for heuristic ordering. Don't
    // add them if another kind of edge already exists.
    if (!Required && PredDep.getSUnit() == D.getSUnit())
      return false;
    if (PredDep.overlaps(D)) {
      // Extend the latency if needed. Equivalent to
      // removePred(PredDep) + addPred(D).
      if (PredDep.getLatency() < D.getLatency()) {
        SUnit *PredSU = PredDep.getSUnit();
        // Find the corresponding successor in N.
        SDep ForwardD = PredDep;
        ForwardD.setSUnit(this);
        for (SDep &SuccDep : PredSU->Succs) {
          if (SuccDep == ForwardD) {
            SuccDep.setLatency(D.getLatency());
            break;
          }
        }
        PredDep.setLatency(D.getLatency());
      }
      return false;
    }
  }
  // Now add a corresponding succ to N.
  SDep P = D;
  P.setSUnit(this);
  SUnit *N = D.getSUnit();
  // Update the bookkeeping.
  if (D.getKind() == SDep::Data) {
    assert(NumPreds < std::numeric_limits<unsigned>::max() &&
           "NumPreds will overflow!");
    assert(N->NumSuccs < std::numeric_limits<unsigned>::max() &&
           "NumSuccs will overflow!");
    ++NumPreds;
    ++N->NumSuccs;
  }
  if (!N->isScheduled) {
    if (D.isWeak()) {
      ++WeakPredsLeft;
    }
    else {
      assert(NumPredsLeft < std::numeric_limits<unsigned>::max() &&
             "NumPredsLeft will overflow!");
      ++NumPredsLeft;
    }
  }
  if (!isScheduled) {
    if (D.isWeak()) {
      ++N->WeakSuccsLeft;
    }
    else {
      assert(N->NumSuccsLeft < std::numeric_limits<unsigned>::max() &&
             "NumSuccsLeft will overflow!");
      ++N->NumSuccsLeft;
    }
  }
  Preds.push_back(D);
  N->Succs.push_back(P);
  if (P.getLatency() != 0) {
    this->setDepthDirty();
    N->setHeightDirty();
  }
  return true;
}

void SUnit::removePred(const SDep &D) {
  // Find the matching predecessor.
  SmallVectorImpl<SDep>::iterator I = llvm::find(Preds, D);
  if (I == Preds.end())
    return;
  // Find the corresponding successor in N.
  SDep P = D;
  P.setSUnit(this);
  SUnit *N = D.getSUnit();
  SmallVectorImpl<SDep>::iterator Succ = llvm::find(N->Succs, P);
  assert(Succ != N->Succs.end() && "Mismatching preds / succs lists!");
  N->Succs.erase(Succ);
  Preds.erase(I);
  // Update the bookkeeping.
  if (P.getKind() == SDep::Data) {
    assert(NumPreds > 0 && "NumPreds will underflow!");
    assert(N->NumSuccs > 0 && "NumSuccs will underflow!");
    --NumPreds;
    --N->NumSuccs;
  }
  if (!N->isScheduled) {
    if (D.isWeak())
      --WeakPredsLeft;
    else {
      assert(NumPredsLeft > 0 && "NumPredsLeft will underflow!");
      --NumPredsLeft;
    }
  }
  if (!isScheduled) {
    if (D.isWeak())
      --N->WeakSuccsLeft;
    else {
      assert(N->NumSuccsLeft > 0 && "NumSuccsLeft will underflow!");
      --N->NumSuccsLeft;
    }
  }
  if (P.getLatency() != 0) {
    this->setDepthDirty();
    N->setHeightDirty();
  }
}

void SUnit::setDepthDirty() {
  if (!isDepthCurrent) return;
  SmallVector<SUnit*, 8> WorkList;
  WorkList.push_back(this);
  do {
    SUnit *SU = WorkList.pop_back_val();
    SU->isDepthCurrent = false;
    for (SDep &SuccDep : SU->Succs) {
      SUnit *SuccSU = SuccDep.getSUnit();
      if (SuccSU->isDepthCurrent)
        WorkList.push_back(SuccSU);
    }
  } while (!WorkList.empty());
}

void SUnit::setHeightDirty() {
  if (!isHeightCurrent) return;
  SmallVector<SUnit*, 8> WorkList;
  WorkList.push_back(this);
  do {
    SUnit *SU = WorkList.pop_back_val();
    SU->isHeightCurrent = false;
    for (SDep &PredDep : SU->Preds) {
      SUnit *PredSU = PredDep.getSUnit();
      if (PredSU->isHeightCurrent)
        WorkList.push_back(PredSU);
    }
  } while (!WorkList.empty());
}

void SUnit::setDepthToAtLeast(unsigned NewDepth) {
  if (NewDepth <= getDepth())
    return;
  setDepthDirty();
  Depth = NewDepth;
  isDepthCurrent = true;
}

void SUnit::setHeightToAtLeast(unsigned NewHeight) {
  if (NewHeight <= getHeight())
    return;
  setHeightDirty();
  Height = NewHeight;
  isHeightCurrent = true;
}

/// Calculates the maximal path from the node to the exit.
void SUnit::ComputeDepth() {
  SmallVector<SUnit*, 8> WorkList;
  WorkList.push_back(this);
  do {
    SUnit *Cur = WorkList.back();

    bool Done = true;
    unsigned MaxPredDepth = 0;
    for (const SDep &PredDep : Cur->Preds) {
      SUnit *PredSU = PredDep.getSUnit();
      if (PredSU->isDepthCurrent)
        MaxPredDepth = std::max(MaxPredDepth,
                                PredSU->Depth + PredDep.getLatency());
      else {
        Done = false;
        WorkList.push_back(PredSU);
      }
    }

    if (Done) {
      WorkList.pop_back();
      if (MaxPredDepth != Cur->Depth) {
        Cur->setDepthDirty();
        Cur->Depth = MaxPredDepth;
      }
      Cur->isDepthCurrent = true;
    }
  } while (!WorkList.empty());
}

/// Calculates the maximal path from the node to the entry.
void SUnit::ComputeHeight() {
  SmallVector<SUnit*, 8> WorkList;
  WorkList.push_back(this);
  do {
    SUnit *Cur = WorkList.back();

    bool Done = true;
    unsigned MaxSuccHeight = 0;
    for (const SDep &SuccDep : Cur->Succs) {
      SUnit *SuccSU = SuccDep.getSUnit();
      if (SuccSU->isHeightCurrent)
        MaxSuccHeight = std::max(MaxSuccHeight,
                                 SuccSU->Height + SuccDep.getLatency());
      else {
        Done = false;
        WorkList.push_back(SuccSU);
      }
    }

    if (Done) {
      WorkList.pop_back();
      if (MaxSuccHeight != Cur->Height) {
        Cur->setHeightDirty();
        Cur->Height = MaxSuccHeight;
      }
      Cur->isHeightCurrent = true;
    }
  } while (!WorkList.empty());
}

void SUnit::biasCriticalPath() {
  if (NumPreds < 2)
    return;

  SUnit::pred_iterator BestI = Preds.begin();
  unsigned MaxDepth = BestI->getSUnit()->getDepth();
  for (SUnit::pred_iterator I = std::next(BestI), E = Preds.end(); I != E;
       ++I) {
    if (I->getKind() == SDep::Data && I->getSUnit()->getDepth() > MaxDepth)
      BestI = I;
  }
  if (BestI != Preds.begin())
    std::swap(*Preds.begin(), *BestI);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD
raw_ostream &SUnit::print(raw_ostream &OS,
                          const SUnit *Entry, const SUnit *Exit) const {
  if (this == Entry)
    OS << "EntrySU";
  else if (this == Exit)
    OS << "ExitSU";
  else
    OS << "SU(" << NodeNum << ")";
  return OS;
}

LLVM_DUMP_METHOD
raw_ostream &SUnit::print(raw_ostream &OS, const ScheduleDAG *G) const {
  return print(OS, &G->EntrySU, &G->ExitSU);
}

LLVM_DUMP_METHOD
void SUnit::dump(const ScheduleDAG *G) const {
  print(dbgs(), G);
  dbgs() << ": ";
  G->dumpNode(this);
}

LLVM_DUMP_METHOD void SUnit::dumpAll(const ScheduleDAG *G) const {
  dump(G);

  dbgs() << "  # preds left       : " << NumPredsLeft << "\n";
  dbgs() << "  # succs left       : " << NumSuccsLeft << "\n";
  if (WeakPredsLeft)
    dbgs() << "  # weak preds left  : " << WeakPredsLeft << "\n";
  if (WeakSuccsLeft)
    dbgs() << "  # weak succs left  : " << WeakSuccsLeft << "\n";
  dbgs() << "  # rdefs left       : " << NumRegDefsLeft << "\n";
  dbgs() << "  Latency            : " << Latency << "\n";
  dbgs() << "  Depth              : " << getDepth() << "\n";
  dbgs() << "  Height             : " << getHeight() << "\n";

  if (Preds.size() != 0) {
    dbgs() << "  Predecessors:\n";
    for (const SDep &Dep : Preds) {
      dbgs() << "    ";
      Dep.getSUnit()->print(dbgs(), G); dbgs() << ": ";
      Dep.print(dbgs(), G->TRI); dbgs() << '\n';
    }
  }
  if (Succs.size() != 0) {
    dbgs() << "  Successors:\n";
    for (const SDep &Dep : Succs) {
      dbgs() << "    ";
      Dep.getSUnit()->print(dbgs(), G); dbgs() << ": ";
      Dep.print(dbgs(), G->TRI); dbgs() << '\n';
    }
  }
}
#endif

#ifndef NDEBUG
unsigned ScheduleDAG::VerifyScheduledDAG(bool isBottomUp) {
  bool AnyNotSched = false;
  unsigned DeadNodes = 0;
  for (const SUnit &SUnit : SUnits) {
    if (!SUnit.isScheduled) {
      if (SUnit.NumPreds == 0 && SUnit.NumSuccs == 0) {
        ++DeadNodes;
        continue;
      }
      if (!AnyNotSched)
        dbgs() << "*** Scheduling failed! ***\n";
      SUnit.dump(this);
      dbgs() << "has not been scheduled!\n";
      AnyNotSched = true;
    }
    if (SUnit.isScheduled &&
        (isBottomUp ? SUnit.getHeight() : SUnit.getDepth()) >
          unsigned(std::numeric_limits<int>::max())) {
      if (!AnyNotSched)
        dbgs() << "*** Scheduling failed! ***\n";
      SUnit.dump(this);
      dbgs() << "has an unexpected "
           << (isBottomUp ? "Height" : "Depth") << " value!\n";
      AnyNotSched = true;
    }
    if (isBottomUp) {
      if (SUnit.NumSuccsLeft != 0) {
        if (!AnyNotSched)
          dbgs() << "*** Scheduling failed! ***\n";
        SUnit.dump(this);
        dbgs() << "has successors left!\n";
        AnyNotSched = true;
      }
    } else {
      if (SUnit.NumPredsLeft != 0) {
        if (!AnyNotSched)
          dbgs() << "*** Scheduling failed! ***\n";
        SUnit.dump(this);
        dbgs() << "has predecessors left!\n";
        AnyNotSched = true;
      }
    }
  }
  assert(!AnyNotSched);
  return SUnits.size() - DeadNodes;
}
#endif

void ScheduleDAGTopologicalSort::InitDAGTopologicalSorting() {
  // The idea of the algorithm is taken from
  // "Online algorithms for managing the topological order of
  // a directed acyclic graph" by David J. Pearce and Paul H.J. Kelly
  // This is the MNR algorithm, which was first introduced by
  // A. Marchetti-Spaccamela, U. Nanni and H. Rohnert in
  // "Maintaining a topological order under edge insertions".
  //
  // Short description of the algorithm:
  //
  // Topological ordering, ord, of a DAG maps each node to a topological
  // index so that for all edges X->Y it is the case that ord(X) < ord(Y).
  //
  // This means that if there is a path from the node X to the node Z,
  // then ord(X) < ord(Z).
  //
  // This property can be used to check for reachability of nodes:
  // if Z is reachable from X, then an insertion of the edge Z->X would
  // create a cycle.
  //
  // The algorithm first computes a topological ordering for the DAG by
  // initializing the Index2Node and Node2Index arrays and then tries to keep
  // the ordering up-to-date after edge insertions by reordering the DAG.
  //
  // On insertion of the edge X->Y, the algorithm first marks by calling DFS
  // the nodes reachable from Y, and then shifts them using Shift to lie
  // immediately after X in Index2Node.
  unsigned DAGSize = SUnits.size();
  std::vector<SUnit*> WorkList;
  WorkList.reserve(DAGSize);

  Index2Node.resize(DAGSize);
  Node2Index.resize(DAGSize);

  // Initialize the data structures.
  if (ExitSU)
    WorkList.push_back(ExitSU);
  for (SUnit &SU : SUnits) {
    int NodeNum = SU.NodeNum;
    unsigned Degree = SU.Succs.size();
    // Temporarily use the Node2Index array as scratch space for degree counts.
    Node2Index[NodeNum] = Degree;

    // Is it a node without dependencies?
    if (Degree == 0) {
      assert(SU.Succs.empty() && "SUnit should have no successors");
      // Collect leaf nodes.
      WorkList.push_back(&SU);
    }
  }

  int Id = DAGSize;
  while (!WorkList.empty()) {
    SUnit *SU = WorkList.back();
    WorkList.pop_back();
    if (SU->NodeNum < DAGSize)
      Allocate(SU->NodeNum, --Id);
    for (const SDep &PredDep : SU->Preds) {
      SUnit *SU = PredDep.getSUnit();
      if (SU->NodeNum < DAGSize && !--Node2Index[SU->NodeNum])
        // If all dependencies of the node are processed already,
        // then the node can be computed now.
        WorkList.push_back(SU);
    }
  }

  Visited.resize(DAGSize);

#ifndef NDEBUG
  // Check correctness of the ordering
  for (SUnit &SU : SUnits)  {
    for (const SDep &PD : SU.Preds) {
      assert(Node2Index[SU.NodeNum] > Node2Index[PD.getSUnit()->NodeNum] &&
      "Wrong topological sorting");
    }
  }
#endif
}

void ScheduleDAGTopologicalSort::AddPred(SUnit *Y, SUnit *X) {
  int UpperBound, LowerBound;
  LowerBound = Node2Index[Y->NodeNum];
  UpperBound = Node2Index[X->NodeNum];
  bool HasLoop = false;
  // Is Ord(X) < Ord(Y) ?
  if (LowerBound < UpperBound) {
    // Update the topological order.
    Visited.reset();
    DFS(Y, UpperBound, HasLoop);
    assert(!HasLoop && "Inserted edge creates a loop!");
    // Recompute topological indexes.
    Shift(Visited, LowerBound, UpperBound);
  }
}

void ScheduleDAGTopologicalSort::RemovePred(SUnit *M, SUnit *N) {
  // InitDAGTopologicalSorting();
}

void ScheduleDAGTopologicalSort::DFS(const SUnit *SU, int UpperBound,
                                     bool &HasLoop) {
  std::vector<const SUnit*> WorkList;
  WorkList.reserve(SUnits.size());

  WorkList.push_back(SU);
  do {
    SU = WorkList.back();
    WorkList.pop_back();
    Visited.set(SU->NodeNum);
    for (const SDep &SuccDep
         : make_range(SU->Succs.rbegin(), SU->Succs.rend())) {
      unsigned s = SuccDep.getSUnit()->NodeNum;
      // Edges to non-SUnits are allowed but ignored (e.g. ExitSU).
      if (s >= Node2Index.size())
        continue;
      if (Node2Index[s] == UpperBound) {
        HasLoop = true;
        return;
      }
      // Visit successors if not already and in affected region.
      if (!Visited.test(s) && Node2Index[s] < UpperBound) {
        WorkList.push_back(SuccDep.getSUnit());
      }
    }
  } while (!WorkList.empty());
}

std::vector<int> ScheduleDAGTopologicalSort::GetSubGraph(const SUnit &StartSU,
                                                         const SUnit &TargetSU,
                                                         bool &Success) {
  std::vector<const SUnit*> WorkList;
  int LowerBound = Node2Index[StartSU.NodeNum];
  int UpperBound = Node2Index[TargetSU.NodeNum];
  bool Found = false;
  BitVector VisitedBack;
  std::vector<int> Nodes;

  if (LowerBound > UpperBound) {
    Success = false;
    return Nodes;
  }

  WorkList.reserve(SUnits.size());
  Visited.reset();

  // Starting from StartSU, visit all successors up
  // to UpperBound.
  WorkList.push_back(&StartSU);
  do {
    const SUnit *SU = WorkList.back();
    WorkList.pop_back();
    for (int I = SU->Succs.size()-1; I >= 0; --I) {
      const SUnit *Succ = SU->Succs[I].getSUnit();
      unsigned s = Succ->NodeNum;
      // Edges to non-SUnits are allowed but ignored (e.g. ExitSU).
      if (Succ->isBoundaryNode())
        continue;
      if (Node2Index[s] == UpperBound) {
        Found = true;
        continue;
      }
      // Visit successors if not already and in affected region.
      if (!Visited.test(s) && Node2Index[s] < UpperBound) {
        Visited.set(s);
        WorkList.push_back(Succ);
      }
    }
  } while (!WorkList.empty());

  if (!Found) {
    Success = false;
    return Nodes;
  }

  WorkList.clear();
  VisitedBack.resize(SUnits.size());
  Found = false;

  // Starting from TargetSU, visit all predecessors up
  // to LowerBound. SUs that are visited by the two
  // passes are added to Nodes.
  WorkList.push_back(&TargetSU);
  do {
    const SUnit *SU = WorkList.back();
    WorkList.pop_back();
    for (int I = SU->Preds.size()-1; I >= 0; --I) {
      const SUnit *Pred = SU->Preds[I].getSUnit();
      unsigned s = Pred->NodeNum;
      // Edges to non-SUnits are allowed but ignored (e.g. EntrySU).
      if (Pred->isBoundaryNode())
        continue;
      if (Node2Index[s] == LowerBound) {
        Found = true;
        continue;
      }
      if (!VisitedBack.test(s) && Visited.test(s)) {
        VisitedBack.set(s);
        WorkList.push_back(Pred);
        Nodes.push_back(s);
      }
    }
  } while (!WorkList.empty());

  assert(Found && "Error in SUnit Graph!");
  Success = true;
  return Nodes;
}

void ScheduleDAGTopologicalSort::Shift(BitVector& Visited, int LowerBound,
                                       int UpperBound) {
  std::vector<int> L;
  int shift = 0;
  int i;

  for (i = LowerBound; i <= UpperBound; ++i) {
    // w is node at topological index i.
    int w = Index2Node[i];
    if (Visited.test(w)) {
      // Unmark.
      Visited.reset(w);
      L.push_back(w);
      shift = shift + 1;
    } else {
      Allocate(w, i - shift);
    }
  }

  for (unsigned LI : L) {
    Allocate(LI, i - shift);
    i = i + 1;
  }
}

bool ScheduleDAGTopologicalSort::WillCreateCycle(SUnit *TargetSU, SUnit *SU) {
  // Is SU reachable from TargetSU via successor edges?
  if (IsReachable(SU, TargetSU))
    return true;
  for (const SDep &PredDep : TargetSU->Preds)
    if (PredDep.isAssignedRegDep() &&
        IsReachable(SU, PredDep.getSUnit()))
      return true;
  return false;
}

bool ScheduleDAGTopologicalSort::IsReachable(const SUnit *SU,
                                             const SUnit *TargetSU) {
  // If insertion of the edge SU->TargetSU would create a cycle
  // then there is a path from TargetSU to SU.
  int UpperBound, LowerBound;
  LowerBound = Node2Index[TargetSU->NodeNum];
  UpperBound = Node2Index[SU->NodeNum];
  bool HasLoop = false;
  // Is Ord(TargetSU) < Ord(SU) ?
  if (LowerBound < UpperBound) {
    Visited.reset();
    // There may be a path from TargetSU to SU. Check for it.
    DFS(TargetSU, UpperBound, HasLoop);
  }
  return HasLoop;
}

void ScheduleDAGTopologicalSort::Allocate(int n, int index) {
  Node2Index[n] = index;
  Index2Node[index] = n;
}

ScheduleDAGTopologicalSort::
ScheduleDAGTopologicalSort(std::vector<SUnit> &sunits, SUnit *exitsu)
  : SUnits(sunits), ExitSU(exitsu) {}

ScheduleHazardRecognizer::~ScheduleHazardRecognizer() = default;
