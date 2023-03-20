"""
Tests basic Main Thread Checker support (detecting a main-thread-only violation).
"""

import os
import time
import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil
from lldbsuite.test.lldbplatformutil import *
import json


class MTCSimpleTestCase(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @skipUnlessDarwin
    @skipIfDarwinEmbedded  # Test file depends on AppKit which is not present on iOS etc.
    def test(self):
        self.mtc_dylib_path = findMainThreadCheckerDylib()
        if self.mtc_dylib_path == "":
            self.skipTest("This test requires libMainThreadChecker.dylib.")

        self.build()
        self.mtc_tests()

    def setUp(self):
        # Call super's setUp().
        TestBase.setUp(self)

    @skipIf(archs=['i386'])
    def mtc_tests(self):
        # Load the test
        exe = self.getBuildArtifact("a.out")
        self.expect("file " + exe, patterns=["Current executable set to .*a.out"])

        self.runCmd("env DYLD_INSERT_LIBRARIES=%s" % self.mtc_dylib_path)
        self.runCmd("run")

        process = self.dbg.GetSelectedTarget().process
        thread = process.GetSelectedThread()
        frame = thread.GetSelectedFrame()

        self.expect("thread info", substrs=['stop reason = -[NSView superview] must be used from main thread only'])

        self.expect(
            "thread info -s",
            substrs=["instrumentation_class", "api_name", "class_name", "selector", "description"])
        self.assertEqual(thread.GetStopReason(), lldb.eStopReasonInstrumentation)
        output_lines = self.res.GetOutput().split('\n')
        json_line = '\n'.join(output_lines[2:])
        data = json.loads(json_line)
        self.assertEqual(data["instrumentation_class"], "MainThreadChecker")
        self.assertEqual(data["api_name"], "-[NSView superview]")
        self.assertEqual(data["class_name"], "NSView")
        self.assertEqual(data["selector"], "superview")
        self.assertEqual(data["description"], "-[NSView superview] must be used from main thread only")
