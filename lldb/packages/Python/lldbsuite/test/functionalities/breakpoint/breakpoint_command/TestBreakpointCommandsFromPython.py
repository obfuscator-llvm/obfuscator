"""
Test that you can set breakpoint commands successfully with the Python API's:
"""

from __future__ import print_function


import os
import re
import sys
import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil
import side_effect


class PythonBreakpointCommandSettingTestCase(TestBase):

    mydir = TestBase.compute_mydir(__file__)
    NO_DEBUG_INFO_TESTCASE = True

    @add_test_categories(['pyapi'])
    def test_step_out_python(self):
        """Test stepping out using avoid-no-debug with dsyms."""
        self.build()
        self.do_set_python_command_from_python()

    def setUp(self):
        TestBase.setUp(self)
        self.main_source = "main.c"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)

    def do_set_python_command_from_python(self):
        exe = self.getBuildArtifact("a.out")
        error = lldb.SBError()

        self.target = self.dbg.CreateTarget(exe)
        self.assertTrue(self.target, VALID_TARGET)

        body_bkpt = self.target.BreakpointCreateBySourceRegex(
            "Set break point at this line.", self.main_source_spec)
        self.assertTrue(body_bkpt, VALID_BREAKPOINT)

        func_bkpt = self.target.BreakpointCreateBySourceRegex(
            "Set break point at this line.", self.main_source_spec)
        self.assertTrue(func_bkpt, VALID_BREAKPOINT)

        # Also test that setting a source regex breakpoint with an empty file
        # spec list sets it on all files:
        no_files_bkpt = self.target.BreakpointCreateBySourceRegex(
            "Set a breakpoint here", lldb.SBFileSpecList(), lldb.SBFileSpecList())
        self.assertTrue(no_files_bkpt, VALID_BREAKPOINT)
        num_locations = no_files_bkpt.GetNumLocations()
        self.assertTrue(
            num_locations >= 2,
            "Got at least two breakpoint locations")
        got_one_in_A = False
        got_one_in_B = False
        for idx in range(0, num_locations):
            comp_unit = no_files_bkpt.GetLocationAtIndex(idx).GetAddress().GetSymbolContext(
                lldb.eSymbolContextCompUnit).GetCompileUnit().GetFileSpec()
            print("Got comp unit: ", comp_unit.GetFilename())
            if comp_unit.GetFilename() == "a.c":
                got_one_in_A = True
            elif comp_unit.GetFilename() == "b.c":
                got_one_in_B = True

        self.assertTrue(got_one_in_A, "Failed to match the pattern in A")
        self.assertTrue(got_one_in_B, "Failed to match the pattern in B")
        self.target.BreakpointDelete(no_files_bkpt.GetID())

        error = lldb.SBError()
        error = body_bkpt.SetScriptCallbackBody(
                "import side_effect; side_effect.callback = 'callback was here'")
        self.assertTrue(
            error.Success(),
            "Failed to set the script callback body: %s." %
            (error.GetCString()))

        self.dbg.HandleCommand(
            "command script import --allow-reload ./bktptcmd.py")
        func_bkpt.SetScriptCallbackFunction("bktptcmd.function")

        # Clear out canary variables
        side_effect.bktptcmd = None
        side_effect.callback = None

        # Now launch the process, and do not stop at entry point.
        self.process = self.target.LaunchSimple(
            None, None, self.get_process_working_directory())

        self.assertTrue(self.process, PROCESS_IS_VALID)

        # Now finish, and make sure the return value is correct.
        threads = lldbutil.get_threads_stopped_at_breakpoint(
            self.process, body_bkpt)
        self.assertTrue(len(threads) == 1, "Stopped at inner breakpoint.")
        self.thread = threads[0]

        self.assertEquals("callback was here", side_effect.callback)
        self.assertEquals("function was here", side_effect.bktptcmd)
