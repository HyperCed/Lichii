#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import subprocess
import sys
import tempfile
import unittest

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "macos_sample.py")
REPORT = """\
Call graph:
  100 Thread_0: CPUCore 0
    100 hv_vcpu_run
  100 Thread_1: CPUThread
    100 hv_vcpu_run
Total number in stack (200 samples)
"""


class MacosSampleTest(unittest.TestCase):
    def test_guest_accounting_for_both_cpu_thread_names(self):
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8") as report:
            report.write(REPORT)
            report.flush()
            result = subprocess.run(
                [sys.executable, "-I", SCRIPT, "--report", report.name],
                capture_output=True,
                text=True,
                check=True,
            )

        self.assertIn("100.0%  CPUCore 0", result.stdout)
        self.assertIn("100.0%  CPUThread", result.stdout)
        cpu_thread_section = result.stdout.split("CPUThread", 1)[1]
        self.assertIn("100.0%  hv_vcpu_run", cpu_thread_section)


if __name__ == "__main__":
    unittest.main()
