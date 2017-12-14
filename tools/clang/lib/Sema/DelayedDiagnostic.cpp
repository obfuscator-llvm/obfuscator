//===--- DelayedDiagnostic.cpp - Delayed declarator diagnostics -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the DelayedDiagnostic class implementation, which
// is used to record diagnostics that are being conditionally produced
// during declarator parsing.
//
// This file also defines AccessedEntity.
//
//===----------------------------------------------------------------------===//
#include "clang/Sema/DelayedDiagnostic.h"
#include <string.h>
using namespace clang;
using namespace sema;

DelayedDiagnostic
DelayedDiagnostic::makeAvailability(AvailabilityResult AR,
                                    SourceLocation Loc,
                                    const NamedDecl *ReferringDecl,
                                    const NamedDecl *OffendingDecl,
                                    const ObjCInterfaceDecl *UnknownObjCClass,
                                    const ObjCPropertyDecl  *ObjCProperty,
                                    StringRef Msg,
                                    bool ObjCPropertyAccess) {
  DelayedDiagnostic DD;
  DD.Kind = Availability;
  DD.Triggered = false;
  DD.Loc = Loc;
  DD.AvailabilityData.ReferringDecl = ReferringDecl;
  DD.AvailabilityData.OffendingDecl = OffendingDecl;
  DD.AvailabilityData.UnknownObjCClass = UnknownObjCClass;
  DD.AvailabilityData.ObjCProperty = ObjCProperty;
  char *MessageData = nullptr;
  if (Msg.size()) {
    MessageData = new char [Msg.size()];
    memcpy(MessageData, Msg.data(), Msg.size());
  }

  DD.AvailabilityData.Message = MessageData;
  DD.AvailabilityData.MessageLen = Msg.size();
  DD.AvailabilityData.AR = AR;
  DD.AvailabilityData.ObjCPropertyAccess = ObjCPropertyAccess;
  return DD;
}

void DelayedDiagnostic::Destroy() {
  switch (Kind) {
  case Access: 
    getAccessData().~AccessedEntity(); 
    break;

  case Availability:
    delete[] AvailabilityData.Message;
    break;

  case ForbiddenType:
    break;
  }
}
