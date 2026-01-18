# Python Module Design Notes

This document captures important design decisions, debugging findings, and technical notes
for the Qore Python module to facilitate future debugging and development.

## Python Version Support

The module supports multiple Python versions with different internal behaviors:
- **Python 3.12**: Uses `python312_internals.h` for GIL tracking
- **Python 3.13**: Uses `python313_internals.h` with updated thread state management
- **Python 3.14+**: Uses `python314_internals.h` with free-threading support (`Py_GIL_DISABLED`)

## GIL (Global Interpreter Lock) Management

### Thread State Tracking

Python's internal thread state tracking changed significantly across versions:

1. **Python's TSS (Thread-Specific Storage)**: Python uses `autoTSSkey` to track the current
   thread state per-thread. However, `PyEval_ReleaseThread()` does NOT clear this TSS in
   Python 3.12+, leading to stale references.

2. **Our Tracking Variables** (in `python3XX_internals.h`):
   - `_qore_tss_tstate`: Thread-local tracking of current thread state
   - `_qore_tss_initialized`: Whether our tracking has been initialized for this thread
   - `_qore_gil_held`: Whether the current thread holds the GIL

3. **Why We Need Our Own Tracking**:
   - `PyGILState_Check()` can return incorrect results after GIL transitions
   - `PyGILState_GetThisThreadState()` can return stale thread states
   - Sub-interpreter thread states can be incorrectly reported by Python's TSS

4. **CRITICAL: TSS vs GIL Tracking Order**:
   - `_qore_tss_tstate` is used to track the current thread state
   - `_qore_gil_held` tracks whether the GIL is actually held
   - For GIL-enabled Python (3.12, 3.13, 3.14), NEVER set `_qore_tss_tstate` before acquiring the GIL
   - Setting TSS before GIL acquisition causes `_qore_PyCeval_GetGilLockedStatus()` to return
     true when we don't actually hold the GIL, leading to skipped `PyEval_RestoreThread()` calls
   - Free-threading mode (`Py_GIL_DISABLED`) requires TSS setup before thread state operations,
     so the rule is inverted there

### gilstate_counter Management

**Critical Bug Found (January 2026)**: The `gilstate_counter` field in `PyThreadState`
must be properly maintained. Python's `PyEval_RestoreThread()` asserts that
`gilstate_counter > 0`.

**Root Cause**: In `QorePythonGilHelper::set()`, when switching from the main interpreter's
thread state to a sub-interpreter's thread state after `Py_NewInterpreter()`:
- The constructor incremented the MAIN interpreter's `gilstate_counter`
- `set()` changed `new_thread_state` to point to the SUB-interpreter's thread state
- The destructor decremented the SUB-interpreter's `gilstate_counter`
- This caused sub-interpreter's counter to go from 1 to 0, corrupting the thread state
- Additionally, the main interpreter's counter was never decremented, causing a leak

**Fix**: In `set()`, we now:
1. Decrement the MAIN interpreter's counter to balance the constructor's increment
2. Increment the SUB interpreter's counter to balance the destructor's decrement

This ensures both interpreters maintain correct counter values.

### setContext() / releaseContext() Flow

These functions manage the Python execution context for Qore threads:

1. **setContext()** (in `QorePythonProgram.cpp`):
   - Gets or creates a thread state for the current thread and interpreter
   - Acquires the GIL if not already held
   - Handles cross-interpreter thread state switching
   - Returns `QorePythonThreadInfo` with saved state for restoration

2. **releaseContext()**:
   - Restores the previous thread state
   - Releases the GIL if it was acquired in setContext()
   - Handles cross-interpreter restoration

### Key Invariants

1. Every `setContext()` must have a matching `releaseContext()`
2. `gilstate_counter` must remain >= 1 for active thread states
3. Thread states must only be used with their owning interpreter
4. When an interpreter is deleted, all its thread states become invalid

## Sub-Interpreter Management

### Creation Flow

1. `QorePythonGilHelper` acquires GIL with main interpreter's thread state
2. For Python 3.13+: `releaseBeforeSubInterpreter()` prepares for sub-interpreter creation
3. `Py_NewInterpreter()` or `Py_NewInterpreterFromConfig()` creates sub-interpreter
4. New thread state is returned with `gilstate_counter = 1`
5. `QorePythonGilHelper::set()` switches to the new thread state
6. On destructor, GIL is released with sub-interpreter's thread state

### Thread State Caching

Thread states are cached in `py_thr_map` (per-PythonProgram, per-thread):
- Avoids creating new thread states for every Python call
- Thread states are reused across `setContext()/releaseContext()` cycles
- When a thread exits, its thread states are cleaned up

## Stale TSS Handling

### The Problem

When an interpreter is deleted:
1. All its thread states are freed by `zapthreads()`
2. But Python's TSS (`autoTSSkey`) may still point to the freed thread state
3. Next Python API call may use the stale/freed thread state

### The Solution (Python 3.13+)

Before creating a new thread state, check if TSS points to a stale one:
1. Get TSS thread state via `PyGILState_GetThisThreadState()`
2. Check if its interpreter still exists (iterate `PyInterpreterState_Head()` list)
3. If interpreter is invalid, the thread state is stale - clear `_status.bound_gilstate`
4. This allows `PyThreadState_New()` to properly bind the new thread state

### Python 3.12 Differences

Python 3.12 has similar TSS issues but different internal structures:
- Uses `gilstate_counter` instead of `_status.bound_gilstate` for some tracking
- Our tracking variables (`_qore_tss_tstate`, etc.) are authoritative
- `PyGILState_Check()` results should not be trusted after GIL transitions

## Python Shutdown Handling

### The Problem

When Python is shutting down (`python_shutdown` flag is set or `!Py_IsInitialized()`):
- `QorePythonGilHelper` constructor returns early without acquiring the GIL
- The `initialized` flag remains false
- Subsequent Python API calls will crash because there's no valid interpreter

### The Solution

`QorePythonGilHelper::isInitialized()` returns whether the GIL was successfully acquired.
Callers that create a `QorePythonGilHelper` and then call Python APIs must check this:

```cpp
QorePythonGilHelper pgh;
if (!pgh.isInitialized()) {
    // Python is shutting down - skip cleanup
    return;
}
// Safe to call Python APIs
```

### Protected Functions

The following functions check `isInitialized()`:
- `QorePythonProgram::deleteIntern()` - sub-interpreter cleanup

## Free-Threading Mode (Python 3.14+ with Py_GIL_DISABLED)

When Python is built with free-threading support:
1. There is no GIL - threads run truly concurrently
2. `PyGILState_*` APIs still exist but behave differently
3. Thread state attachment/detachment is critical
4. Reference counting uses atomic operations
5. Container operations have internal locking

Key differences in our code:
- `_qore_gil_held` is always false (no GIL to hold)
- `_qore_acquire_thread_state()` / `_qore_release_thread_state()` handle attachment
- `PyThreadState_Swap()` internally handles `_PyThreadState_Attach()`

## Debugging Tips

### Common Crashes and Their Causes

1. **"thread state already initialized"**: `PyThreadState_New()` tried to reuse
   `interp->_initial_thread` when `threads.head` is NULL but `_status.initialized` is 1.
   Usually caused by improper thread state cleanup.

2. **SIGSEGV in `_PyEvalFramePushAndInit`**: Thread state's data stack is corrupted,
   often due to `gilstate_counter` being 0 when `PyEval_RestoreThread()` is called.

3. **"non-NULL old thread state" in free-threading**: Tried to attach a thread state
   when one was already attached. Use `PyGILState_GetThisThreadState()` to check first.

### Useful Debug Output

Enable debug level 5 for detailed GIL tracking:
```
qore --debug=5 -l python your_script.q
```

Key debug messages to look for:
- `setContext() ENTRY`: Shows thread state pointers and tracking state
- `got thread context`: Shows `GIL:` (PyGILState_Check), `hG:` (our tracking), `refs:` (gilstate_counter)
- `creating new thread state`: New thread state being created
- `Python 3.XX is_python_created_thread`: Thread origin detection

### Valgrind Notes

When running valgrind:
- Use `qore -b` to disable signals (avoids spurious errors)
- Memory leaks in Python/Qore global state are expected for short-running tests
- Focus on "Invalid read/write" errors which indicate real issues

## File Organization

- `python3XX_internals.h`: Version-specific Python internals access
- `QorePythonProgram.cpp`: Main interpreter/thread state management
- `python-module.cpp`: `QorePythonGilHelper` and `QorePythonReleaseGilHelper` classes
- `QC_PythonProgram.qpp`: Qore API for PythonProgram class

## Version History

### January 2026
- Fixed `gilstate_counter` bug in `QorePythonGilHelper::set()` causing Python 3.12 crashes
- Added `Python::PythonVersion` constant
- Improved stale TSS handling for Python 3.13+
- Fixed TSS/GIL tracking order issue for Python 3.13+:
  - Changed `_qore_PyCeval_GetGilLockedStatus()` to use `_qore_gil_held` in Python 3.13
  - Removed early TSS setting in `setContext()` for GIL-enabled Python (only needed for free-threading)
- Fixed `gilstate_counter` leak in `QorePythonGilHelper::set()`:
  - Now decrements main interpreter's counter when switching to sub-interpreter
- Added `QorePythonGilHelper::isInitialized()` for shutdown-safe cleanup:
  - `deleteIntern()` now checks if GIL was acquired before calling Python APIs
