//===-- MICmnBase.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

// In-house headers:
#include "MIDataTypes.h"
#include "MIUtilString.h"

// Declarations:
class CMICmnLog;

//++
//============================================================================
// Details: MI common code implementation base class.
//--
class CMICmnBase {
  // Methods:
public:
  /* ctor */ CMICmnBase();

  bool HaveErrorDescription() const;
  const CMIUtilString &GetErrorDescription() const;
  void SetErrorDescription(const CMIUtilString &vrTxt) const;
  void SetErrorDescriptionn(const char *vFormat, ...) const;
  void SetErrorDescriptionNoLog(const CMIUtilString &vrTxt) const;
  void ClrErrorDescription() const;

  // Overrideable:
public:
  /* dtor */ virtual ~CMICmnBase();

  // Attributes:
protected:
  mutable CMIUtilString m_strMILastErrorDescription;
  bool m_bInitialized; // True = yes successfully initialized, false = no yet or
                       // failed
  CMICmnLog *m_pLog;   // Allow all derived classes to use the logger
  MIint m_clientUsageRefCnt; // Count of client using *this object so not
                             // shutdown() object to early
};
