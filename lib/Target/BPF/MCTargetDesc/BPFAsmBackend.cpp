//===-- BPFAsmBackend.cpp - BPF Assembler Backend -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/BPFMCTargetDesc.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include <cassert>
#include <cstdint>

using namespace llvm;

namespace {

class BPFAsmBackend : public MCAsmBackend {
public:
  bool IsLittleEndian;

  BPFAsmBackend(bool IsLittleEndian)
    : MCAsmBackend(), IsLittleEndian(IsLittleEndian) {}
  ~BPFAsmBackend() override = default;

  void applyFixup(const MCAssembler &Asm, const MCFixup &Fixup,
                  const MCValue &Target, MutableArrayRef<char> Data,
                  uint64_t Value, bool IsResolved) const override;

  MCObjectWriter *createObjectWriter(raw_pwrite_stream &OS) const override;

  // No instruction requires relaxation
  bool fixupNeedsRelaxation(const MCFixup &Fixup, uint64_t Value,
                            const MCRelaxableFragment *DF,
                            const MCAsmLayout &Layout) const override {
    return false;
  }

  unsigned getNumFixupKinds() const override { return 1; }

  bool mayNeedRelaxation(const MCInst &Inst) const override { return false; }

  void relaxInstruction(const MCInst &Inst, const MCSubtargetInfo &STI,
                        MCInst &Res) const override {}

  bool writeNopData(uint64_t Count, MCObjectWriter *OW) const override;
};

} // end anonymous namespace

bool BPFAsmBackend::writeNopData(uint64_t Count, MCObjectWriter *OW) const {
  if ((Count % 8) != 0)
    return false;

  for (uint64_t i = 0; i < Count; i += 8)
    OW->write64(0x15000000);

  return true;
}

void BPFAsmBackend::applyFixup(const MCAssembler &Asm, const MCFixup &Fixup,
                               const MCValue &Target,
                               MutableArrayRef<char> Data, uint64_t Value,
                               bool IsResolved) const {
  if (Fixup.getKind() == FK_SecRel_4 || Fixup.getKind() == FK_SecRel_8) {
    assert(Value == 0);
  } else if (Fixup.getKind() == FK_Data_4 || Fixup.getKind() == FK_Data_8) {
    unsigned Size = Fixup.getKind() == FK_Data_4 ? 4 : 8;

    for (unsigned i = 0; i != Size; ++i) {
      unsigned Idx = IsLittleEndian ? i : Size - i - 1;
      Data[Fixup.getOffset() + Idx] = uint8_t(Value >> (i * 8));
    }
  } else {
    assert(Fixup.getKind() == FK_PCRel_2);
    Value = (uint16_t)((Value - 8) / 8);
    if (IsLittleEndian) {
      Data[Fixup.getOffset() + 2] = Value & 0xFF;
      Data[Fixup.getOffset() + 3] = Value >> 8;
    } else {
      Data[Fixup.getOffset() + 2] = Value >> 8;
      Data[Fixup.getOffset() + 3] = Value & 0xFF;
    }
  }
}

MCObjectWriter *BPFAsmBackend::createObjectWriter(raw_pwrite_stream &OS) const {
  return createBPFELFObjectWriter(OS, 0, IsLittleEndian);
}

MCAsmBackend *llvm::createBPFAsmBackend(const Target &T,
                                        const MCRegisterInfo &MRI,
                                        const Triple &TT, StringRef CPU,
                                        const MCTargetOptions&) {
  return new BPFAsmBackend(/*IsLittleEndian=*/true);
}

MCAsmBackend *llvm::createBPFbeAsmBackend(const Target &T,
                                          const MCRegisterInfo &MRI,
                                          const Triple &TT, StringRef CPU,
                                          const MCTargetOptions&) {
  return new BPFAsmBackend(/*IsLittleEndian=*/false);
}
