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

## Ownership during interpreter finalization

A `PythonProgram` owns its Python subinterpreter even when it shares the parent
`QoreProgram`. The executing Python context takes precedence over the parent's
external module context when importing Qore wrappers. The constructor establishes
that context before evaluating Python source, so classes, functions, and constants
are cached with the interpreter that allocates them.
Python-created threads locate that owner from their attached interpreter before
entering Qore, because those threads have no Qore execution context yet.

Owned subinterpreters finish through `Py_EndInterpreter`, including free-threaded builds.
Calling `PyInterpreterState_Clear` and `PyInterpreterState_Delete` alone does not
run the complete module, `atexit`, thread, and allocator finalization sequence.
Qore records the thread states it creates and retires idle states before ending
the interpreter. Borrowed states from Python-created threads remain owned by
Python. Python 3.12 requires the initial state to survive until shutdown so that
`threading._shutdown` can release its main-thread completion sentinel. Python 3.13
and newer use a state belonging to the finalizing native thread when destruction
runs on a different thread. Reusing the original state on another thread prevents
Python 3.14 from collecting the interpreter's object graph.

`threading._shutdown()` of Python 3.12 and earlier, called by `Py_EndInterpreter`, treats the finalizing thread as
the thread that imported `threading` in the interpreter (its "main thread") if their identifiers are equal, and then
asserts that the main thread's sentinel lock is still held.  Qore threads end while the interpreter lives on, and
their states are deleted when they end, which releases the sentinel; a new thread can then get the identifier of the
thread that ended (musl and glibc reuse them), and `_shutdown()` failed the assertion ("Exception ignored in
threading._shutdown") and did not wait for the non-daemon threads.  Before ending the interpreter, `deleteIntern()`
calls `forgetEndedThreadingMainThread()`: if the main thread's sentinel is released and its identifier is the one of
the current thread, the main thread is marked as stopped (`_stop()`) and given no identifier, so that `_shutdown()`
treats the current thread as another thread, as it is.  Python 3.13 does not compare the identifiers.

Python 3.13 and earlier abort the process in `Py_EndInterpreter` ("not the last thread") if another thread still
has a state in the interpreter once the threads it waits for (the threads of the threading module that are not
daemon threads, joined by `threading._shutdown()`) have ended; Python 3.14 instead makes such threads exit when they
next take the GIL.  Before ending an interpreter with an older Python, `deleteIntern()` therefore checks the remaining
thread states (`hasThreadsNotJoinedAtEnd()`): their thread identifiers are collected first, without running Python
code (which could release the GIL to a thread that deletes its state), and then looked up in `threading._active`;
a state whose thread is not a non-daemon `threading.Thread` (a daemon thread, a dummy thread, a thread started with
`_thread`) cannot be waited for, and the interpreter is then left in memory with those threads still running its
code instead of being ended.  A call to Qore from such a thread afterwards raises `RuntimeError` (the program is no
longer valid); the interpreter's memory is never released.

After a Qore call releases the GIL, its auto-TSS binding points at the long-lived
main state. A different thread can then destroy the subinterpreter without leaving
a dangling auto-TSS pointer in the caller. Free-threaded context helpers instead
save and restore the actual attached state, including a detached (`nullptr`) state.

Qore objects and call references can outlive a Python interpreter. Their persistent
Python references register with the owning `QorePythonProgram`, retaining a weak
C++ program reference. Finalization detaches and releases these references while
that interpreter is still usable. Destroying a Qore wrapper later releases its
C++ data without touching a dead interpreter. Calls through surviving wrappers
report `PYTHON-INTERPRETER-DELETED`.

When an object passes to another interpreter, its Qore proxy stays between the
two Python runtimes. Calls through that proxy enter the owning interpreter, and
returning it to Qore preserves object identity. Only an object belonging to the
receiving interpreter can be unwrapped to its original `PyObject`. Sharing raw
objects would let one interpreter's garbage collector traverse another's heap.

Import specifications and stack-inspection callables are resolved in the active
interpreter. They are not cached across interpreters: `ModuleSpec`, Python
functions, and module dictionaries belong to the interpreter that creates them.
Import-spec allocation failures propagate as Python exceptions and leave imports
retryable.

The persistent-reference registry uses `py_thr_lck`; Python reference operations
run in the owning interpreter context. Each entry is removed before `Py_DECREF`,
and the lock is released before that call, because Python finalizers may destroy
other registered Qore objects. Cleanup fetches the next entry after each callback.

Python return values remain in RAII holders while their values are converted to
Qore. Conversion borrows its input and retains any references needed by the result.
Exception stack conversion owns each new reference returned by `PyFrame_GetBack`
and copies function names before releasing the frame that supplied them.
For Qore objects constructed from Python, the constructor's owning reference is
released after the saved-object list or user callback has acquired its own reference.
Wrapper heap types implement Python's garbage-collection protocol and traverse the
instance's reference to its type, allowing cycles through class constants to be
collected.

Python-hosted processes release the same bridge references and generated classes
without ending Python's main interpreter. Each generated Python callable owns its
method definition and copied name through its capsule. This allows a bound method
or imported function to survive Qore shutdown until Python collects it; a later
call raises `RuntimeError` before accessing the deleted Qore program. Capsule
references keep the C++ program record alive without keeping its interpreter or
Qore program alive.

## Threads Created by Python

Python can call into Qore on a thread that Python created (`threading.Thread`, an executor pool, or a native
thread using `PyGILState_Ensure()`); such a thread has no Qore thread data, which every Qore API needs, starting with
`QorePythonProgram::getContext()`.  Every function and slot through which Python calls into Qore therefore creates
a `QorePythonQoreEntryHelper` before it uses the Qore API (`python-module.h`): method calls, static method calls,
constructors (`py_init`), member reads (`py_getattro`), Qore callables and their release, Qore functions, namespace
attribute and class imports, the import loaders, and the `qoreloader` functions.

The helper calls `qore_python_register_thread()`, which registers a thread that is not yet known to Qore as a foreign
thread and keeps it registered while its Python thread state exists, as a Qore thread is registered while it runs:

- the Qore objects that Python code creates on the thread are kept in the thread's Program data
  (`saveQoreObjectFromPythonDefault()`) until the thread ends, so they survive the call that created them;
- Qore resources that the thread acquires (a `Mutex`, thread-local data) stay its own between calls.

Registering only for the duration of a call, as the method calls did before, deleted an object created by a
constructor call when the call returned, and released a lock acquired in one call before the next.

A `QorePythonThreadSentinel` object is stored in the thread state's dictionary (`PyThreadState_GetDict()`); its
`tp_dealloc` deregisters the thread when the state is cleared, which happens on the thread itself when it ends (and
for a `PyGILState_Ensure()` thread when its last `PyGILState_Release()` deletes the state), while the thread's Qore
data is still usable.  A state cleared on another thread (ex: by `Py_EndInterpreter()`) does not deregister the thread
it belongs to; a C++ `thread_local` object deregisters such a thread when the native thread exits.  On musl, a
`thread_local` destructor can run after the library's own thread-local storage has been released, in which case the
thread cannot be deregistered and its Qore thread entry is not reused.

### Sandbox of threads created by Python

A thread that Python creates has no Qore caller, so no sandbox would govern the Qore code that it calls; a thread
that Python starts from Qore code does not inherit the starting code's sandbox the way `q_start_thread()` threads do.
On such a thread, `QorePythonQoreEntryHelper` captures, for each call, the sandbox of the Qore Program that owns the
interpreter running on the thread (`QorePythonProgram::getContext()`), as it is when the call is made, and applies it
with `QoreSandboxCaptureHelper` as the outermost caller.  The sandbox is resolved per call because it is often
attached after the interpreter exists: a child Program inherits the python module of its parent, and so gets its
interpreter, when it is constructed, before the creator calls `Program::setSandboxManager()`.  If the Program has no
sandbox, the sandbox captured when the interpreter was created (the creating code's, `QorePythonProgram::sandbox`)
applies.  On a Qore thread, the sandbox of the thread's own callers governs the call, as for any synchronous call.

## The Process Environment

Qore serializes every access to the C environment with one process-wide read-write lock (`SystemEnvironment`,
`QoreEnvironmentReadLock`); Python does not take it.  The module bridges the two:

- `os.environ` is a Python dict copied from the C environment when the `posix` module of an interpreter initializes;
  setting or deleting one of its keys calls the `putenv()` / `unsetenv()` of the `os` module, which change the C
  environment.  When `qoreloader` is initialized in an interpreter (the main interpreter at module initialization, a
  subinterpreter when its `QorePythonProgram` imports `qoreloader`, and the host interpreter when Python imports
  `qoreloader`), `qore_python_install_environment_functions()` replaces `posix.putenv`, `posix.unsetenv`,
  `os.putenv` and `os.unsetenv` with functions that convert, check and audit their arguments as CPython 3.9+ does and
  change the environment with `SystemEnvironment::set()` / `unset()`, with the GIL released while the lock is waited
  for.  `os.environ` itself stays Python's copy: it does not see changes made with Qore's `setenv()`, as in plain
  Python it does not see changes made by C code.
- `PYTHONHOME` and `PYTHONPATH` are read with `QoreEnvironmentValue`.
- `Py_InitializeEx()` runs under a `QoreEnvironmentReadLock`: Python reads its configuration variables and copies the
  environment into `os.environ` while it initializes, and no other Python thread exists yet.  The `time` module,
  which reads `TZ` when it is created, is imported there as well.  Code that Python runs while it initializes
  (`sitecustomize`, `.pth` files) therefore runs with the lock held: forking from it is not supported (an assertion
  in debug builds, under the fork contract of the environment lock: no `fork()` while holding it), and a change it
  makes to the environment is not serialized with Qore (the module's environment functions are installed only when
  `qoreloader` is initialized; through them, a change would fail with `EDEADLK` while the lock is held).
- Every interpreter copies the environment when its `posix` module is created, including each subinterpreter while
  `Py_NewInterpreter()` runs, and the `time` module of every interpreter reads `TZ` (`tzset()`) when it is created.
  Both modules use multi-phase initialization (Python 3.10 and later are handled): their exec functions are slots of
  module definitions shared by all interpreters, and `python_wrap_environment_exec_slots()` (module initialization,
  in the main interpreter) replaces them with functions that hold a `QoreEnvironmentReadLock` while they call the
  original ones, with garbage collection disabled meanwhile; the originals are restored when the module is deleted.
  These exec functions run C code only, so the lock is never held while a thread waits for the GIL.  The lock is not
  held for all of `Py_NewInterpreter()`: the new interpreter runs Python code (importlib, `site`), which can release
  the GIL to another thread, and a thread that holds the GIL while it forks (`os.fork()` in the main interpreter of
  a Python process hosting Qore, or `_posixsubprocess` on platforms without `vfork()`, such as macOS) waits for the
  lock in Qore's fork handlers, so the creating thread would wait for the GIL while holding the lock, a deadlock.
- Python C code that calls `getenv()` or `setenv()` directly at other times (ex: `time.tzset()`,
  `time.localtime()`) is not serialized with Qore.

**Invariant:** no thread holds the environment lock while it waits for a GIL or for a Python thread state.

## fork()

The module registers fork handlers (`qore_register_fork_handler()`, in the module's initialization function) for its
process-wide locks:

| Lock | Rank | Why |
|---|---|---|
| `QorePythonProgram::py_thr_lck` | `QFR_MODULE_LEAF` (acquired) | holders only change containers and never wait for the GIL while holding it |
| `qpy_lock`, `qpy_global_lock` | `QFR_MODULE_LEAF` (acquired) | holders only change the program sets |
| `QorePythonProgram::main_ts_lck` | `QFR_MODULE_RESET` | holders wait for the GIL (interpreter creation), which a thread in `os.fork()` holds while the handlers run; the child takes it again if the forking thread held it |

The GIL interplay, measured on Linux with Python 3.14 (GIL build):

- `os.fork()` in the main interpreter (a Python process hosting Qore) runs with the GIL held while Qore's handlers
  acquire their locks; no holder of those locks waits for the GIL, so it does not deadlock: 200 forks while four
  Python threads called Qore all succeeded, and the children used Qore and Python.
- `os.fork()` in a subinterpreter (every interpreter of a Qore program or `PythonProgram`) killed the child with
  "Fatal Python error: _PyInterpreterState_DeleteExceptMain: not main interpreter" (CPython's after-fork handling
  only supports the main interpreter); it did not deadlock.  The subinterpreters are therefore created with fork()
  disallowed: with Python 3.12 and later, `createInterpreter()` uses `Py_NewInterpreterFromConfig()` with the
  configuration of `Py_NewInterpreter()` (`_PyInterpreterConfig_LEGACY_INIT`) except `allow_fork = 0`, so
  `os.fork()` and `os.forkpty()` raise `RuntimeError` ("fork not supported for isolated subinterpreters"); before
  3.12, which has no such configuration, `disallowFork()` replaces `fork` and `forkpty` of `posix` and `os` in the new
  interpreter with functions that raise `RuntimeError`.  Starting processes (`subprocess`, `allow_exec`) is still
  allowed: CPython's `_posixsubprocess` runs no Python code in the child unless `preexec_fn` is given, which CPython
  refuses in subinterpreters.
- A child created with a `fork()` that Python does not make (Qore's `fork()`, or any native fork) gets the GIL in the
  state the parent had: if another thread held it, it can never be acquired in the child.  If no thread held it,
  Python works in the child.  The module does not acquire the GIL before `fork()`: every `fork()` in the process,
  including each process start by the process module, would then wait for Python.
- The child handler `python_gil` (`QFR_MODULE_RESET`, child only, python-module.cpp) finds which case applies with
  `qore_python_fork_child_gil_lost()` (src/python_fork_gil.c): the GIL is lost if one of its mutexes is held (a thread
  was taking or dropping it), or if it is locked and its last holder is not a thread state of the forking thread.
  That file is C, compiled with CPython's internal headers (C headers, installed with the Python headers), which
  give the layout of the GIL of every version (the main interpreter's `ceval.gil` in 3.12+, `_PyRuntime.ceval.gil`
  before); CMake compiles it only if the headers are found (`QORE_PYTHON_HAVE_FORK_GIL_CHECK`).  If the GIL is lost,
  the handler sets `python_fork_lost` and `python_shutdown`: the module then uses Python in no way, as after Python
  has been shut down, so nothing waits for the GIL:
  - `QorePythonHelper` (every call into Python with an `ExceptionSink`) and `createInterpreter()` raise
    `PYTHON-FORK-ERROR`
  - `deleteIntern()` leaves the interpreter, `releasePersistentReference()` leaves the reference, and the module's
    shutdown does not finalize Python (memory is not released, which a child that cannot use Python can afford)
  - a thread returning to Python code (`~QorePythonReleaseGilHelper()`, after a Qore call made from Python, such as
    Python calling Qore's `fork()`) cannot continue: `qore_python_fork_lost_exit()` writes a message to stderr and
    calls `_exit(1)`
- No thread holds Qore's environment lock while it waits for the GIL (see "The Process Environment"), since
  `os.fork()` holds the GIL while the environment lock is acquired.

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
