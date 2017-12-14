//===-- MipsMCAsmInfo.cpp - Mips Asm Properties ---------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the declarations of the MipsMCAsmInfo properties.
//
//===----------------------------------------------------------------------===//

#include "MipsMCAsmInfo.h"
#include "llvm/ADT/Triple.h"

using namespace llvm;

void MipsMCAsmInfo::anchor() { }

MipsMCAsmInfo::MipsMCAsmInfo(const Triple &TheTriple) {
  IsLittleEndian = TheTriple.isLittleEndian();

  if ((TheTriple.getArch() == Triple::mips64el) ||
      (TheTriple.getArch() == Triple::mips64)) {
    CodePointerSize = CalleeSaveStackSlotSize = 8;
  }

  // FIXME: This condition isn't quite right but it's the best we can do until
  //        this object can identify the ABI. It will misbehave when using O32
  //        on a mips64*-* triple.
  if ((TheTriple.getArch() == Triple::mipsel) ||
      (TheTriple.getArch() == Triple::mips)) {
    PrivateGlobalPrefix = "$";
    PrivateLabelPrefix = "$";
  }

  AlignmentIsInBytes          = false;
  Data16bitsDirective         = "\t.2byte\t";
  Data32bitsDirective         = "\t.4byte\t";
  Data64bitsDirective         = "\t.8byte\t";
  CommentString               = "#";
  ZeroDirective               = "\t.space\t";
  GPRel32Directive            = "\t.gpword\t";
  GPRel64Directive            = "\t.gpdword\t";
  DTPRel32Directive           = "\t.dtprelword\t";
  DTPRel64Directive           = "\t.dtpreldword\t";
  TPRel32Directive            = "\t.tprelword\t";
  TPRel64Directive            = "\t.tpreldword\t";
  UseAssignmentForEHBegin = true;
  SupportsDebugInformation = true;
  ExceptionsType = ExceptionHandling::DwarfCFI;
  DwarfRegNumForCFI = true;
  HasMipsExpressions = true;

  // Enable IAS by default for O32.
  if (TheTriple.getArch() == Triple::mips ||
      TheTriple.getArch() == Triple::mipsel)
    UseIntegratedAssembler = true;

  // Enable IAS by default for Debian mips64/mips64el.
  if (TheTriple.getEnvironment() == Triple::GNUABI64)
    UseIntegratedAssembler = true;
}
