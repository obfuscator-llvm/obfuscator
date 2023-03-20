//===-- MICmnMIValueTuple.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

// In-house headers:
#include "MICmnMIValue.h"
#include "MICmnMIValueConst.h"
#include "MICmnMIValueResult.h"

//++
//============================================================================
// Details: MI common code MI Result class. Part of the CMICmnMIValueTupleRecord
//          set of objects.
//          The syntax is as follows:
//          result-record ==>  [ token ] "^" result-class ( "," result )* nl
//          token = any sequence of digits
//          * = 0 to many
//          nl = CR | CR_LF
//          result-class ==> "done" | "running" | "connected" | "error" | "exit"
//          result ==> variable "=" value
//          value ==> const | tuple | list
//          const ==> c-string (7 bit iso c string content)
//          tuple ==>  "{}" | "{" result ( "," result )* "}"
//          list ==>  "[]" | "[" value ( "," value )* "]" | "[" result ( ","
//          result )* "]"
//          More information see:
//          http://ftp.gnu.org/old-gnu/Manuals/gdb-5.1.1/html_chapter/gdb_22.html
//--
class CMICmnMIValueTuple : public CMICmnMIValue {
  // Methods:
public:
  /* ctor */ CMICmnMIValueTuple();
  /* ctor */ CMICmnMIValueTuple(const CMICmnMIValueResult &vResult);
  /* ctor */ CMICmnMIValueTuple(const CMICmnMIValueResult &vResult,
                                const bool vbUseSpacing);
  //
  void Add(const CMICmnMIValueResult &vResult);
  void Add(const CMICmnMIValueResult &vResult, const bool vbUseSpacing);
  void Add(const CMICmnMIValueConst &vValue, const bool vbUseSpacing);
  CMIUtilString ExtractContentNoBrackets() const;

  // Overridden:
public:
  // From CMICmnBase
  /* dtor */ ~CMICmnMIValueTuple() override;

  // Methods:
private:
  void BuildTuple();
  void BuildTuple(const CMICmnMIValueResult &vResult);
  void BuildTuple(const CMIUtilString &vValue);

  // Attributes:
private:
  bool m_bSpaceAfterComma; // True = put space separators into the string, false
                           // = no spaces used
};
