# Regression cases

Every `*.txt` here is a replay file that `Regression.AllSavedCasesReplayCleanly`
(tests/test_replay.cpp) runs through the engine with the consistency checker.
If a sibling `<name>.expected` exists, the output must match it byte for byte.

**Workflow when a property test fails**
1. The failure message prints a file path (`lob_failure_seed<N>.txt`) and a
   `lob_replay <file> --check` command that reproduces it.
2. Fix the bug, then copy the file here with a descriptive name
   (`<what_broke>.txt`) and a one-line comment saying what it caught.
3. Optionally generate `.expected` with `lob_replay <file> --check > <name>.expected`
   **after reviewing the output by hand**.

The files currently here are hand-written edge cases (the random tests have not
yet found a bug to save), chosen because these are the places matching engines
typically go wrong.
