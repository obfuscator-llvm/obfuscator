//= AArch64WinCOFFObjectWriter.cpp - AArch64 Windows COFF Object Writer C++ =//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//

#include "MCTargetDesc/AArch64FixupKinds.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCFixupKindInfo.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/MCWinCOFFObjectWriter.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>

using namespace llvm;

namespace {

class AArch64WinCOFFObjectWriter : public MCWinCOFFObjectTargetWriter {
public:
  AArch64WinCOFFObjectWriter()
      : MCWinCOFFObjectTargetWriter(COFF::IMAGE_FILE_MACHINE_ARM64) {}

  ~AArch64WinCOFFObjectWriter() override = default;

  unsigned getRelocType(MCContext &Ctx, const MCValue &Target,
                        const MCFixup &Fixup, bool IsCrossSection,
                        const MCAsmBackend &MAB) const override;

  bool recordRelocation(const MCFixup &) const override;
};

} // end anonymous namespace

unsigned AArch64WinCOFFObjectWriter::getRelocType(
    MCContext &Ctx, const MCValue &Target, const MCFixup &Fixup,
    bool IsCrossSection, const MCAsmBackend &MAB) const {
  auto Modifier = Target.isAbsolute() ? MCSymbolRefExpr::VK_None
                                      : Target.getSymA()->getKind();

  switch (static_cast<unsigned>(Fixup.getKind())) {
  default: {
    const MCFixupKindInfo &Info = MAB.getFixupKindInfo(Fixup.getKind());
    report_fatal_error(Twine("unsupported relocation type: ") + Info.Name);
  }

  case FK_Data_4:
    switch (Modifier) {
    default:
      return COFF::IMAGE_REL_ARM64_ADDR32;
    case MCSymbolRefExpr::VK_COFF_IMGREL32:
      return COFF::IMAGE_REL_ARM64_ADDR32NB;
    case MCSymbolRefExpr::VK_SECREL:
      return COFF::IMAGE_REL_ARM64_SECREL;
    }

  case FK_Data_8:
    return COFF::IMAGE_REL_ARM64_ADDR64;

  case FK_SecRel_2:
    return COFF::IMAGE_REL_ARM64_SECTION;

  case FK_SecRel_4:
    return COFF::IMAGE_REL_ARM64_SECREL;

  case AArch64::fixup_aarch64_add_imm12:
    return COFF::IMAGE_REL_ARM64_PAGEOFFSET_12A;

  case AArch64::fixup_aarch64_ldst_imm12_scale1:
  case AArch64::fixup_aarch64_ldst_imm12_scale2:
  case AArch64::fixup_aarch64_ldst_imm12_scale4:
  case AArch64::fixup_aarch64_ldst_imm12_scale8:
  case AArch64::fixup_aarch64_ldst_imm12_scale16:
    return COFF::IMAGE_REL_ARM64_PAGEOFFSET_12L;

  case AArch64::fixup_aarch64_pcrel_adrp_imm21:
    return COFF::IMAGE_REL_ARM64_PAGEBASE_REL21;

  case AArch64::fixup_aarch64_pcrel_branch26:
  case AArch64::fixup_aarch64_pcrel_call26:
    return COFF::IMAGE_REL_ARM64_BRANCH26;
  }
}

bool AArch64WinCOFFObjectWriter::recordRelocation(const MCFixup &Fixup) const {
  return true;
}

namespace llvm {

MCObjectWriter *createAArch64WinCOFFObjectWriter(raw_pwrite_stream &OS) {
  MCWinCOFFObjectTargetWriter *MOTW = new AArch64WinCOFFObjectWriter();
  return createWinCOFFObjectWriter(MOTW, OS);
}

} // end namespace llvm
