# SPDX-License-Identifier: MIT
"""ctest entry point: run one test module with unittest; exit 77 (ctest "skipped") when every test was skipped
(no Wayland session, displays asleep, ... including a skipped setUpClass), 1 on failures or unexpected successes, 0 otherwise."""
import sys
import unittest

if __name__ == "__main__":
    name = sys.argv[1]
    suite = unittest.defaultTestLoader.loadTestsFromName(name)
    result = unittest.TextTestRunner(verbosity=2, stream=sys.stdout).run(suite)
    if not result.wasSuccessful():
        sys.exit(1)
    if result.skipped and result.testsRun <= len(result.skipped):
        sys.exit(77)
    sys.exit(0)
