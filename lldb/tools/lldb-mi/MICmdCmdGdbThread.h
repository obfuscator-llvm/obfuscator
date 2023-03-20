//===-- MICmdCmdGdbThread.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Overview:    CMICmdCmdGdbThread      interface.
//
//              To implement new MI commands derive a new command class from the
//              command base
//              class. To enable the new command for interpretation add the new
//              command class
//              to the command factory. The files of relevance are:
//                  MICmdCommands.cpp
//                  MICmdBase.h / .cpp
//                  MICmdCmd.h / .cpp
//              For an introduction to adding a new command see
//              CMICmdCmdSupportInfoMiCmdQuery
//              command class as an example.

#pragma once

// In-house headers:
#include "MICmdBase.h"

//++
//============================================================================
// Details: MI command class. MI commands derived from the command base class.
//          *this class implements GDB command "thread".
//--
class CMICmdCmdGdbThread : public CMICmdBase {
  // Statics:
public:
  // Required by the CMICmdFactory when registering *this command
  static CMICmdBase *CreateSelf();

  // Methods:
public:
  /* ctor */ CMICmdCmdGdbThread();

  // Overridden:
public:
  // From CMICmdInvoker::ICmd
  bool Execute() override;
  bool Acknowledge() override;
  // From CMICmnBase
  /* dtor */ ~CMICmdCmdGdbThread() override;
};
