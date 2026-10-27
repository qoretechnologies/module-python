# Python Module Design Notes

This document captures important design decisions, debugging findings, and technical notes
for the Qore Python module to facilitate future debugging and development.

## Python Version Support

The minimum Python version is 3.11 (`MIN_PY_MINOR_VER` in `CMakeLists.txt`): the thread state tracking of the module
(`_qore_tss_tstate`, `_qore_gil_held`, `_qore_check_python_created_thread_gil()`) exists only in the internals headers
of Python 3.11 and later, and the headers of Python 3.6 to 3.10 were removed.

The module supports multiple Python versions with different internal behaviors:
- **Python 3.11**: Uses `python311_internals.h`
- **Python 3.12**: Uses `python312_internals.h` for GIL tracking
- **Python 3.13**: Uses `python313_internals.h` with updated thread state management
- **Python 3.14+**: Uses `python314_internals.h` with free-threading support (`Py_GIL_DISABLED`); Python 3.15 and
  3.16 use it as well (tested with 3.15.1 and 3.16.0a1, GIL and free-threaded builds)

CI builds and tests the module with the Python of each `qore-test-base` image and, in the `test-ubuntu-python*` jobs,
with CPython 3.11 (the minimum), 3.15, 3.15 free-threaded and 3.16, built from the python.org source releases pinned
(version and SHA-256) in `.gitlab-ci.yml` (`test/docker_test/install-cpython.sh`).  A new Python version that changes
the internals the module uses gives a CMake warning and a build without the affected feature (see the fork GIL check
and `QORE_PYTHON_HAVE_TSTATE_BOUND_GILSTATE`); these jobs fail on any CMake warning.

Each CI job reports its result to GitHub in its own status context (`module-python/<job>`); the `module-python`
context, which the branch protection of `develop` requires, is set once for the pipeline: pending in the `.pre` stage
and success or failure by the `on_success` and `on_failure` jobs of the `report` stage, so that the job that ends last
cannot overwrite a failure.

Python 3.16 deprecates `PyStatus::exitcode`; since the API passes `PyStatus` by value (`PyStatus_Exception()`), the
implicit copy constructor of the structure uses the deprecated member, which clang reports at the definition of the
structure in `cpython/initconfig.h`, where a pragma around the call has no effect.  `python-module.h` therefore
disables `-Wdeprecated-declarations` around the inclusion of the Python headers only (Python 3.16 and later): a
deprecated API that the module uses is still reported where the module uses it.  Reported upstream as
[CPython issue 160436](https://github.com/python/cpython/issues/160436); the suppression can be limited to the Python
versions without the fix once it is released.

## Restrictions (Functional Domains)

Python code is not subject to the restrictions or the sandbox of Qore Programs, so every way to reach Python from Qore
is an uncontrolled API (`QDOM_UNCONTROLLED_API`, blocked by `PO_NO_UNCONTROLLED_APIS`, which `PO_NO_EXTERNAL_ACCESS`,
`PO_NO_IO` and `PO_LOCKDOWN` include), as with the jni module:
- the Python classes, methods and functions imported into Qore (`QorePythonClass`, `QorePythonProgram::importModule()`
  and the class import code) are created in `QDOM_UNCONTROLLED_API`
- `PythonProgram` is in `EMBEDDED_LOGIC,UNCONTROLLED_API` (`src/QC_PythonProgram.qpp`): the constructor, the static
  methods and `create_object()` are rejected by Qore in Programs with either restriction
- `python_module_parse_cmd()` rejects every module command with `PO_NO_UNCONTROLLED_APIS`, and `parse` also with
  `PO_NO_EMBEDDED_LOGIC`.  This check is the module's own: a child Program inherits the module's feature from its
  parent, so Qore's `PO_NO_MODULES` check in `QoreModuleManager::loadModuleIntern()` does not apply to the commands of
  a module the Program already has, and Qore cannot know what a module command does

Qore APIs called from Python are checked against the restrictions and the sandbox of the Program whose Python code calls
them (see "Threads Created by Python").  Test: `restrictedProgramTest()` in `test/python.qtest`.

## GIL (Global Interpreter Lock) Management

### Thread State Tracking

Whether and with which state the thread holds the GIL:

- **Python 3.13+** (GIL-enabled and free-threaded builds): the attached state, `PyThreadState_GetUnchecked()`
  (public API).  The module keeps no tracking of its own: the functions of `python313_internals.h` and
  `python314_internals.h` (`_qore_PyCeval_GetThreadState()`, `_qore_PyCeval_GetGilLockedStatus()`, ...) read the
  attached state, and the ones that set the tracking (`_qore_PyGILState_SetThisThreadState()` in GIL builds,
  `_QORE_SET_GIL_HELD()`) do nothing.  `PyGILState_Check()` and `PyGILState_GetThisThreadState()` report the state
  bound to the thread for the GIL state API, which with sub-interpreters is not necessarily the attached state.
  A thread that holds the GIL without a context of the module (ex: a thread created by Python) is detected by its
  attached state.  The module's own tracking could differ from the attached state (ex: after JNI released the GIL);
  `QorePythonGilHelper`'s destructor restored only the tracking, not the state it had replaced.
- **Python 3.11 and 3.12**: the module's thread-local tracking (`_qore_tss_tstate`, `_qore_tss_initialized`,
  `_qore_gil_held` in `python311_internals.h` and `python312_internals.h`): 3.11 has no public way to read the
  attached state (`_PyThreadState_UncheckedGet()` is private), and in 3.12 `PyEval_ReleaseThread()` does not clear
  the binding of the GIL state API, so `PyGILState_Check()` reports the GIL after it was released.  Never set the
  tracking before the GIL is acquired: `_qore_PyCeval_GetGilLockedStatus()` would then skip
  `PyEval_RestoreThread()`.

### CPython Internals Used

Each remaining use of CPython-specific members or internal structures, and why no public API can replace it:

- `PyThreadState::_status.bound_gilstate` (3.12+, detected by CMake: `QORE_PYTHON_HAVE_TSTATE_BOUND_GILSTATE`):
  cleared before the module deletes an idle state of another thread when an interpreter is deleted
  (`deleteIntern()`), and in 3.12's `setContext()` when the flag of an attached state is stale; deleting a state
  whose flag is set clears the binding of the *deleting* thread (`unbind_gilstate_tstate()`), and no public API
  changes the binding.
- `PyThreadState::c_recursion_remaining` (3.11-3.13, `QORE_PYTHON_TSTATE_C_RECURSION_COUNTER`): reduced to what the
  stack of a Qore thread can hold (see "Stack Usage and Recursion"); 3.14 checks the stack itself.
- Python 3.11's `_PyRuntime.gilstate` (`autoTSSkey`, `tstate_current`, `check_enabled`) and `_PyRuntime.ceval.gil`
  (`last_holder`), with the copied layouts of `python311_internals.h`: the GIL state API of 3.11 does not support
  sub-interpreters, and `PyThreadState_Swap()` does not update its binding; the module binds the state of the
  interpreter that it attaches, so that extension code using `PyGILState_Ensure()` in a sub-interpreter gets its
  state.  3.11 is the minimum version; the layouts are fixed for it.
- `src/python_fork_gil.c` (compiled with the internal headers, `internal/pycore_*.h`): recreates the GIL in a child
  of `fork()` when another thread held it (see "fork()"); CPython only does this for `os.fork()`.
- Reads of public `PyThreadState` members (`interp`, `thread_id`) that have accessor functions only in some versions.

### gilstate_counter

The module does not change `PyThreadState::gilstate_counter`.  CPython creates every thread state with a counter of 1
(`PyGILState_Release()` must not delete it), except a state that `PyGILState_Ensure()` creates for itself, which its
matching `PyGILState_Release()` deletes; `PyGILState_Ensure()`/`PyGILState_Release()` change it in balanced pairs.
The module incremented it when it attached a state and decremented it when it released the state, which only
mirrored those pairs, and had to be rebalanced when `QorePythonGilHelper::set()` switched states (an imbalance left a
counter at 0, and the state was deleted by the next `PyGILState_Release()`).

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

### Nested Helpers and Call Overhead

A `QorePythonHelper` for the program of the thread's current helper, while the thread state that the current helper
attached is still attached (`PyThreadState_GetUnchecked()`, `_PyThreadState_UncheckedGet()` before 3.13), is nested:
it calls neither `setContext()` nor `releaseContext()` and does not swap the thread-local program (ex: the helper of
`callInternal()` called with the context of `QorePythonClass::callPythonMethod()`).  A helper that releases the GIL
in between (`QorePythonReleaseGilHelper`) detaches the state, so a helper inside it takes the full path.  A nested
helper is not counted as a use of the program (`isCounted()`), as it did not increment `pgm_thr_cnt`; it still
checks for an interrupt and registers the cancel callback if no outer helper of the program did.  `callFunction()`
and `callMethod()` call `callWithContext()`, which sets no contexts.  `QorePythonReferenceHolder::purge()` (3.13+)
releases the reference directly when the thread's bound state is attached.  The arguments tuple of a call from
Python is not added to the cycle-detection path (`getQoreListFromTuple()` without a path): a cycle through it also
goes through one of its elements.  `test/benchmark-calls.qr` and `test/benchmark-calls.py` report the per-call costs
(not run in CI).

What remains of a call from Qore is mostly the GIL hand-off: attaching the thread state (`take_gil`) and, before
releasing it, swapping in the thread's idle state of the main interpreter (`qore_python_release_context()`), which
detaches and attaches again.  Most of the cost of a call from Python to Qore is Qore's
`CodeEvaluationHelper`; variant resolution is under 1% (not cached).

### Key Invariants

1. Every `setContext()` must have a matching `releaseContext()` (except for nested helpers, which call neither)
2. The module does not change `gilstate_counter` (CPython keeps it >= 1 for the states it does not delete)
3. Thread states must only be used with their owning interpreter
4. When an interpreter is deleted, all its thread states become invalid

## Stack Usage and Recursion

CPython 3.11 - 3.13 bound the recursion that uses the C stack with a counter in each thread state that is sized for the
stack of the main thread: `recursion_remaining` in 3.11 (which also counts Python calls) and `c_recursion_remaining`
in 3.12 and 3.13 (checked by CMake: `QORE_PYTHON_TSTATE_C_RECURSION_COUNTER` names the member).  `setContext()`
reduces the counter to `q_thread_stack_remaining() / PYTHON_C_STACK_FACTOR` (1 KB per level) when that is lower, and
`releaseContext()` adds the reduction back (`QorePythonThreadInfo::c_recursion_reduction`); nested contexts on the same
thread reduce it further and restore their own reductions.  Only the counter can be changed: when it reaches zero,
`_Py_CheckRecursiveCall()` compares the depth (the thread state's limit minus the counter) with the interpreter's
limit and resets the counter if the depth is lower, so the module's former reduction of the thread state's limit
(`recursion_limit`, `py_recursion_limit`) let the recursion continue.  CPython 3.14 checks the stack of the thread
itself (`c_stack_soft_limit`).  The 3.11 - 3.13 parser has its own nesting limit (`MAXSTACK`), which the module cannot
bound: parsing deeply nested code can overflow stacks below 512 KB.  Test: `smallStackRecursionTest()`.

## Lifetime of Qore Objects Used from Python

A `PyQoreObject` holds a strong reference to its `QoreObject` (`strong`), taken in `PythonQoreClass::wrap()` (Qore
objects passed to or returned to Python) and in `newQoreObject()` (objects created from Python: the reference returned
by the constructor is taken over).  Only when Qore creates the object of a Qore class that inherits the Python class is
the reference weak (`py_init()` with the implicit Qore object), as Qore then manages the object.  `releaseQoreObject()`
releases a strong reference in `tp_dealloc` and `tp_clear` with a thread registered with Qore
(`QorePythonQoreEntryHelper`) and without the GIL, as it can run the object's destructor.  Objects are not saved in
thread-local data (`_python_save` was removed); the save object callback is still called for objects created from
Python, without the GIL.

An instance of a Python subclass of a Qore class is referenced by its Qore object's private data
(`QorePythonPrivateData`, `qobj_refs_self`), a cycle across both runtimes: `py_traverse()` reports that reference to
Python's cycle collector only while the Python object holds the only reference to the Qore object
(`reference_count() == 1`), so the collector releases both when nothing else references them, and Qore code that
references the Qore object keeps the Python object alive.  Test: `qoreObjectLifetimeTest()`.

Qore code must not run with the GIL held: a Qore thread holding a Qore lock and waiting for the GIL would deadlock with
it, and Qore's deadlock detection cannot see the GIL.  Besides calls from Python to Qore, which release the GIL, Qore
code runs when Python releases Qore values (destructors): `qore_python_release()` (`python-module.cpp`) releases the
arguments and results of calls from Python to Qore, member values read from Python and the callable of a
`PyQoreCallable` without the GIL when `qore_python_release_runs_qore_code()` finds that the release can run Qore code
(an object, closure or call reference whose last reference it is, possibly in a list or hash whose last reference it
is); other values are released with the GIL, which avoids releasing and acquiring the GIL in every call.
`PythonQoreClass::releaseQoreObject()` and the save object callback always release the GIL.  Test: `gilReleaseTest()`.

## Exceptions Between Qore and Python

Each interpreter has its own `qoreloader.QoreException` class (a subclass of `RuntimeError` created with
`PyErr_NewExceptionWithDoc()` in `slot_qoreloader_exec()`, which runs once for each interpreter), held in the state of
the `qoreloader` module; `qore_python_get_qore_exception_type()` returns the class of the current interpreter.  A
static type would be shared by all interpreters, which isolated interpreters (PEP 684, PEP 687) must not share;
previously the static type was only used in the main interpreter, sub-interpreters raised a `RuntimeError` with the
Qore exception in its arguments, and a Qore exception was then guessed from a `RuntimeError`'s arguments.

- Qore -> Python (`raisePythonException()`): the Qore exception is consumed as an `ExceptionInfo` hash, whose values
  become the attributes of a `QoreException` (`next` is the `__cause__`); the hash is kept in a capsule attribute
  (`_qore_exception`), released without the GIL (it can hold objects whose destructors are Qore code).  If the
  exception's `arg` wraps a Python exception of the same interpreter and `err` is its class name, the original Python
  exception object is raised instead (so `SystemExit` and `KeyboardInterrupt` keep their meaning).  The conversion
  runs in the program of the current interpreter (`getContext()`), as callers pass the global program
- Python -> Qore (`checkPythonException()` -> `raiseQoreException()`): a `QoreException` with the capsule is raised
  again from the hash; the new exceptions get the current Qore call stack as their outer frames, so the end of the
  saved call stack that equals the current call stack is removed (the type `User` is lost: the API only raises system
  exceptions).  Other exceptions get `err` = `module.qualname`, `desc` = `str()`, `arg` = the converted exception
  object, and their location and call stack from the traceback (`tb_lineno` for the frames of the traceback, the
  current lines for the frames calling them).  The cause or context is raised after the exception, which makes it
  `next`; each exception is raised in its own sink, as the call stack given to `raiseExceptionArg()` is added to all
  exceptions of a sink.  Chains are limited to 20 exceptions (as Qore's `ExceptionInfo`) and end at an exception that
  was already raised

`QorePythonStackLocationHelper` adds the Python frames to the Qore call stack of Qore code called from Python (Qore
methods, callables and functions), starting with the innermost frame (`sys._getframe(0)`).

## Interrupts, Deletion and Signals

`QorePythonHelper` (with an `ExceptionSink`, the entry of Qore calls into Python) registers a cancel callback with the
sandbox manager of the current Program, if any, for the first helper of the program in the thread; the callback takes
the state's mutex, attaches to the interpreter (a `QorePythonHelper` on the requesting thread) and, if the helper is
still active, raises `KeyboardInterrupt` in the thread with `PyThreadState_SetAsyncExc()`.  The helper's destructor
(`deactivateInterrupt()`) takes the mutex with the thread detached (a running callback holds it while it waits for
the GIL), attaches again with the mutex held (a callback takes the mutex before the GIL, so it cannot hold the GIL
then), and clears `active` and an undelivered async exception; a later callback does nothing, so the callback is
removed without waiting.  The helper can still hold the GIL when it is destroyed (a helper created with the GIL held,
ex: on a thread created by Python, or a nested one): waiting for the mutex then deadlocked with a callback that waited
for the GIL.  The mutex also serializes the callback with the deactivation in free-threaded builds.  In a child of
`fork()` the mutex is not taken (a callback of a thread that does not exist can hold it).  `checkPythonException()`
raises `PROGRAM-INTERRUPTED` for `KeyboardInterrupt` when `qore_check_cancel()` reports the interrupt, and
`PYTHON-INTERPRETER-DELETED` when the program is being deleted: `deleteIntern()` records the deleting thread
(`deleting_tid`, under `py_thr_lck`: `getAcquireThreadState()` refuses other threads from then on) and raises
`KeyboardInterrupt` in the live threads of the interpreter whose states the program caches (other than its own) before
waiting for them.

The module registers a Program cleanup callback (`qore_register_program_cleanup_callback()`): the Python program
created for a Program (its external data) is deleted while the Program is still valid, so that the Qore objects that
its Python objects hold (strong references, see "Python Objects of Qore Objects") are released, and their destructors
run, before the Program's data is cleared; deleting the external data later finds the program already deleted.

`slot_qoreloader_exec()` initializes Qore with `QLO_DISABLE_SIGNAL_HANDLING`, as Python owns the signals of a process
that it hosts.

## Python Iterators

The Qore class of a Python type with `__next__` (and without attributes named `next`, `getValue` or `valid`) has
`AbstractIterator` as a builtin virtual base class and the methods `next()` (`PyIter_Next()`), `getValue()` and
`valid()`; the current item is the internal member `__$PYITER__` (a list with the item, or no value), so Qore releases
it with the object.

## Python Objects of Qore Objects

A Qore object has at most one Python object (`PyQoreObject` with a strong reference) in each interpreter: `proxy_map`
in `PythonQoreClass.cpp` maps (interpreter, Qore object) to the Python object, under the leaf lock `qpy_proxy_lock`
(fork handlers).  `wrap()` and `newQoreObject()` (objects of Qore classes created from Python) register the object;
`releaseQoreObject()` removes it first, before the GIL can be released, so with the GIL a registered object always
has references; free-threaded builds take the reference with `PyUnstable_TryIncRef()` (enabled with
`PyUnstable_EnableTryIncRef()`), and free-threaded 3.13, which lacks it, has no map.  Objects of Python subclasses
of Qore classes are not registered: the Qore object's private data gives the Python object.

`py_getattro()` raises `AttributeError` for special names, members that the object does not have (declared members
exist; undeclared ones exist with a value, as with `exists`; classes with `memberGate()` give all members) and
`INVALID-MEMBER`/`PRIVATE-MEMBER`; `py_setattro()` sets Qore members (data descriptors of the Python type and new
attributes of objects with a dict, i.e. of Python subclasses, are Python attributes); `__dir__` (in each type's
dict) adds the public members; `weakreflist` is the `__weaklistoffset__` member.

Static members are `qoreloader.QoreStaticMember` descriptors (a per-interpreter class created from Python source in
`slot_qoreloader_exec()`, held in the module state) whose getter and setter are `PyCFunction`s with the
`QoreExternalStaticMember` and the handle of its class in a capsule.  Enums are `IntEnum`/`StrEnum`/`Enum` classes
and hashdecls `dict` subclasses (`getEnumType()`, `getHashDeclType()`, cached per program under
`qpy_subclass_type_lock`, released in `deleteIntern()`); the classes hold the handle of the Qore declaration in a
capsule attribute (`_qore_enum_`, `_qore_hashdecl_`), which `getQoreValue()` uses to convert members to
`QoreValue::makeEnum()` values and objects to `TypedHashDecl::doRuntimeCast()` hashes; `getPythonValueIntern()`
converts `QoreValue::isEnum()` values and hashes with `getHashDecl()` to them.  The namespace that Qore creates for an
enum's members is not imported (the enum class is).

## Lifetime of Qore Declarations Used from Python

The Python types of Qore classes, hashdecls and enums can outlive the Program that declared them (ex: a type kept in a
Python variable after a child Program is deleted), so they do not hold the declarations themselves but a
`QorePythonDeclHandle` (`QorePythonDeclHandle.h`), following Qore's declaration lifetime protocol
(`design/declaration-lifetime-api.md` in Qore, qoretechnologies/qore#5517):

- a handle holds the declaration and its owner, `getProgram()` of the declaration when the handle is created (the
  class of a method or static member); handles with an owner are registered by owner in a global registry (a leaf
  lock; the registry holds weak pointers, and handles remove themselves when they are destroyed)
- the capsules of the class type (`__$QCLS__`), of method functions (`method_capsule_t`), of static member
  descriptors (`static_member_capsule_t`), of hashdecl types and enum classes hold the handle
- a declaration is only used while a `QorePythonDeclUse` of its handle is held: a use fails once the handle is invalid,
  and the operation raises `RuntimeError` ("the Qore Program that declared it has been deleted"); a use is held while
  the Qore code that uses the declaration runs (a constructor or method call, the default values of a hashdecl)
- the Program cleanup callback (`python_program_cleanup()`), after deleting the Program's own Python program, calls
  `QorePythonDeclHandle::invalidate()`: the Program's handles are removed from the registry, marked invalid, and the
  callback waits for their current uses in other threads to end, without the GIL, as a use can wait for the GIL; this
  happens before Qore frees the Program's declarations
- the caches keyed by declaration (`hashdecl_type_map`, `enum_type_map`, `py_cls_map`) check the handle of an entry:
  an entry whose handle is invalid is replaced, as its declaration was freed and its address can belong to another
  declaration.  A replaced `PythonQoreClass` is kept (`stale_py_cls_vec`) and released with the others, as its Python
  type can still be used
- a declaration without an owner in the static system namespace (`isSystem()`) is never invalidated; one without an
  owner outside it has outlived its Program (it is held by a value) and gets no Python type, which would outlive it:
  `getPythonValueIntern()` converts its hashes to `dict`s and the values of its enums to their values, and raises
  `PYTHON-VALUE-ERROR` for the objects of its classes
- the Qore classes of Python types made for Qore classes, used as base classes of the Qore classes created for Python
  subclasses, are used (`QorePythonDeclUseList`) while the base classes are added

## Qore Classes Inheriting Python Classes

`execPythonConstructor()` runs for the Qore class of the Python class; when the object's class is a Qore class (not a
`QorePythonClass`), `getQoreSubclassType()` returns a generated Python subclass for the pair (Qore class, Python
class), cached in `subclass_type_map` (references released in `deleteIntern()`; guarded by the leaf lock
`qpy_subclass_type_lock`, which has fork handlers), or the Python class itself if the Qore class overrides none of its
methods.  For each method name in the Python class's MRO whose `findMethod()` result is a public normal method of a
class that is not a `QorePythonClass`, the subclass has an instance method (`PyInstanceMethod` of a `PyCFunction`
whose self is the Qore method name) calling `callQoreOverride()`, which runs the Qore method with `evalMethod()`
without the GIL, as other Qore calls from Python.

The subclass has `__slots__ = ('_qore_object_',)`: `newQoreSubclassObject()` creates the object with `tp_new`, sets
the slot to a capsule holding a weak (`tRef()`) reference to the Qore object (the Qore object owns the Python object
through its private data, so a strong reference would be a cycle), sets the private data, and then runs `tp_init`, so
the overrides (and the Python methods they call through the Qore class of the Python class) work from `__init__()`.
A call on a deleted Qore object raises `OBJECT-ALREADY-DELETED`, as with Qore's weak references.  `getQoreValue()`
returns the Qore object for such a Python object (`getQoreSubclassObject()`, which checks for the slot descriptor in
the type's dict first).

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

### Interpreters Created by Python, and the Types of the Module

The types of the `qoreloader` module (the base type of the Python types of Qore classes, `QoreCallable`, the loaders
of Qore and Java modules and their finder in `sys.meta_path`, the modules of Java namespaces and the registrations of
Python threads) are heap types created for each interpreter when the module is initialized in it
(`QorePythonInterpreterTypes`, kept in the state of the module and released in its `m_free`), with the objects of the
loaders.  Python objects, types included, cannot be shared by interpreters: an isolated sub-interpreter (PEP 684) has
its own GIL and its own memory allocator, and with free-threaded Python each interpreter counts the references of its
heap types itself.  The types were static types of the extension shared by all interpreters, which crashed Python
3.16 (the per-type lookup cache of a static type was freed by the allocator of another interpreter) and free-threaded
Python (issue 5536).  `qore_python_get_interpreter_types()` finds the types of the current interpreter in a map keyed
by the interpreter, with a cache in each thread that is checked against a generation counter, incremented when types
are registered or deregistered (the address of a deleted interpreter can be reused).  The types do not reference the
module (`PyType_FromSpec()`, not `PyType_FromModuleAndSpec()`), so that they do not keep it alive.

When Python is the host, a sub-interpreter that imports `qoreloader` gets its own `QorePythonProgram`, with its own
Qore Program, as the main interpreter has the global program (`qore_python_pgm`): created with the attached thread
state of the sub-interpreter in `slot_qoreloader_exec()`, found by `QorePythonProgram::getContext()` by the
interpreter (the map of programs and their thread states), and released in the module's `m_free`, while the
sub-interpreter exists.  Previously the code running in a sub-interpreter used the global program, whose caches of
Python types belong to the main interpreter.  An interpreter that a `QorePythonProgram` creates has that program: its
import of `qoreloader` is marked (`qore_python_program_interpreter_import`).  Test: the sub-interpreter scenario of
`test/standalone-loader.py`, which creates and closes sub-interpreters after the main interpreter imported the module.

## Qore Classes for Python Classes

`getNamespaceForClass()` resolves an automatically imported type's `__module__` before using the import context.
Base classes and re-exported classes therefore get the same defining-module path whichever import reaches them
first; `clmap` still holds one Qore class per Python type. A known module path is not extended by the module prefix
of `tp_name`. Without a usable `__module__`, the legacy dotted `__name__`, import context and relative type path
fallbacks apply. Attribute access and UTF-8 conversion errors propagate; missing or non-string metadata falls back.
The namespace's `python_module` metadata records the resolved dotted module name. Function imports continue to use
`getNamespaceForObject()` so re-exported functions (ex: `os.getcwd`, defined by `posix`) keep their import paths;
constants and explicit `export-class` placement are unchanged. Tests: `classNamespaceTest()` and
`classNamespaceMetadataTest()` (qoretechnologies/qore#5520).
If a base's metadata fails while a subclass is being created, `addClassToNamespaceIntern()` removes the incomplete
subclass from both class caches before freeing it, so a later conversion can retry safely.

`setupQorePythonClass()` creates a `QorePythonClass` for a Python type.  A Python base class is added as a virtual base
class: it shares the Python object, and Qore maps the private data of a virtual base class to the subclass's private
data (`addVirtualPrivateData()`), which the methods of the Python base class use.  A Qore base class (the base is the
Python type of a Qore class, `PyQoreObjectType_Check()`) is not virtual: Qore runs the constructor, destructor and copy
method of a base class only if it is not virtual (the constructor is run explicitly by `PythonQoreClass::py_init()`).
Qore makes the ancestors of a virtual base class virtual (`BCSMList::addBaseClassesToSubclass()`), so a Qore class
reached through Python base classes is added first as a direct non-virtual base class (`BCSMList::add()` keeps the
first entry of a class), and `addPythonTypeAttributes()` adds the methods of the Python base classes before the Qore
class to the subclass in method resolution order, so that they take precedence over the Qore class's methods, which
the direct base class would otherwise put first.  Test: `subclassDestructorTest()`.

Members that the class does not declare are Python attributes: `memberGate()` reads them, and the member write gate
(`QoreClass::setMemberWriteGate()`, Qore 3.0) `memberWriteGate()` sets them with `PyObject_SetAttr()` and removes them
with `PyObject_DelAttr()` (`remove` and `delete` get the previous value, NOTHING if the object has no such
attribute), so properties, `__slots__` and `__setattr__()`/`__delattr__()` apply.  Qore calls the gate without its
locks; a compound or nested assignment (`+=`, `obj.h.key = v`) reads the attribute through `memberGate()`, changes a
copy and writes it back, which is not atomic.  Qore subclasses inherit both gates, with the private data of the
registering class.  Test: `attributeWriteTest()`.

## Thread State Cache

`py_thr_map` caches the thread state of each interpreter for each Qore thread ID (`q_gettid()`), and Qore gives the ID
of a deregistered thread to the next thread.  `pythonThreadCleanup()` (a Qore thread cleanup function) removes the
entries of an ended thread; it skipped the cleanup when `py_thr_lck` was held by another thread (it used `trylock()`,
for issue #4651: static process destruction, which it still detects with `q_libqore_exiting()`), so the next thread
with the ID found the state of the ended thread, possibly the memory of the state of another running thread.
`setContext()` also checks that a cached state belongs to the current native thread (`PyThreadState::thread_id`) and
drops it otherwise, and debug builds assert that the state used belongs to the thread.  Test: `concurrent_rounds()` in
`test/standalone-threads.py` (a stress test: the skipped cleanup depends on timing; it fails reliably with
free-threaded Python without the fix).

## Stale TSS Handling

### The Problem

When an interpreter is deleted:
1. All its thread states are freed by `zapthreads()`
2. But Python's TSS (`autoTSSkey`) may still point to the freed thread state
3. Next Python API call may use the stale/freed thread state

### The Solution (Python 3.13+)

Since the module binds the idle state of the thread to it when it releases the GIL (see "Ownership during interpreter
finalization"), a thread's TSS does not point to a state of another interpreter while the thread does not hold the
GIL.  As a safeguard, before creating a new thread state, `setContext()` checks if TSS points to a stale one:
1. Get TSS thread state via `PyGILState_GetThisThreadState()`
2. Check if its interpreter still exists (iterate `PyInterpreterState_Head()` list)
3. If interpreter is invalid, the thread state is stale (and can be freed): it is not touched, and the thread's
   tracking is reset
4. This allows `PyThreadState_New()` to properly bind the new thread state

### Python 3.12 Differences

Python 3.12 has similar TSS issues but different internal structures:
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

When the module releases the GIL (`qore_python_release_context()`, Python 3.12 and later) with a state of another
interpreter, or with the main thread state, it first attaches the thread's own idle state (`python_idle_tstate`, a
state in the main interpreter that only the thread uses, created on first use), so that CPython moves the thread's
auto-TSS binding (`PyGILState_GetThisThreadState()`) to it, with the GIL held.  A thread that does not hold the GIL
therefore keeps no binding to a state of an interpreter that another thread can destroy (which cannot clear the
thread's binding), nor to a state that other threads use, and attaching a state binds it again (CPython clears the
flag of the state bound before, with the GIL held).  The main interpreter ends only when Python is finalized.  The
idle state is deleted when the thread ends (`qore_python_delete_idle_thread_state()`, from the module's thread
cleanup, which also runs when a thread created by Python is deregistered from Qore while Python clears its own
state: the idle state is then deleted while that state stays attached).  Only public CPython API is used: Python 3.12
to 3.14 keep the binding in `_PyRuntime.autoTSSkey`, which Python 3.15 replaced with a hidden thread-local variable
(`_Py_tss_gilstate` in Python/pystate.c), which no header declares and libpython does not export.

**Invariant:** the `bound_gilstate` flag of a state that other threads use (the main thread state, which the
`QorePythonGilHelper` of interpreter creation and destruction uses on any thread, and the states of other
interpreters) is only changed with the GIL held.  Previously the main thread state was bound to every thread that
released the GIL instead, and `setContext()` and `QorePythonGilHelper` cleared the flags of the states bound to the
thread before acquiring the GIL, so the flag of the main thread state was written by threads that did not hold the
GIL (a data race reported by ThreadSanitizer).  The module reads and writes the flag (a CPython-specific member of
`PyThreadState`) only if CMake finds it (`QORE_PYTHON_HAVE_TSTATE_BOUND_GILSTATE`).  Free-threaded context helpers
instead save and restore the actual attached state, including a detached (`nullptr`) state.

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
  Both modules use multi-phase initialization: their exec functions are slots of module definitions shared by all
  interpreters, and `python_wrap_environment_exec_slots()` (module initialization, in the main interpreter) replaces
  them with functions that hold a `QoreEnvironmentReadLock` while they call the original ones, with garbage collection
  disabled meanwhile; the originals are restored when the module is deleted.
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
  The check is skipped in a fork that Python makes with its fork protocol (`python_fork_protocol_running`, a
  thread-local flag set from the at-fork "before" function that `qore_python_register_at_fork_functions()` registers
  in the main interpreter, and by the protocol handlers below): the forking thread holds the GIL, and
  `PyOS_AfterFork_Child()` recreates it, so a mutex held by a thread waiting for the GIL (which takes it from time to
  time) does not make it lost.  The debug-build test hooks `qoreloader._test_hold_gil_mutex()` /
  `_test_release_gil_mutex()` hold the mutex in a thread for the test of this case.
  That file is C, compiled with CPython's internal headers (C headers, installed with the Python headers), which
  give the layout of the GIL of every version (the main interpreter's `ceval.gil` in 3.12+, `_PyRuntime.ceval.gil`
  before); CMake compiles it only if it compiles with the headers of the Python library, with and without the test
  hooks of debug builds (`try_compile()` of the file itself, `QORE_PYTHON_HAVE_FORK_GIL_CHECK`), so that a Python
  version without the headers, or whose internal structures changed, is built without it and a warning.  If the GIL
  is lost,
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
- In a child whose GIL is usable, the threads of the parent that were using an interpreter (a Qore thread in a
  Qore call from Python, a thread created by Python) do not exist.  The child handler of `py_thr_lck`
  (`forkChildThreadStates()`) counts the uses of each interpreter (`pgm_thr_cnt`) again from the
  `QorePythonHelper` objects of the forking thread, which form a thread-local list, so that deleting an interpreter
  does not wait for the threads of the parent; it also records the forking thread's TID, and the first use of the
  thread-state maps in the child (`checkForkIntern()`, after every acquisition of `py_thr_lck`) removes the entries of
  the other TIDs, which new threads of the child can get (removing them in the handler would free memory there).
  An interpreter created before the fork (`interp_fork_generation` differs from `fork_generation`) that still has
  thread states other than the finalizing one when it is deleted in the child (states of the parent's Python
  threads) is left in memory instead of being ended: the threading module, which is not told of a fork() that Python
  does not make, would wait for those threads forever in `Py_EndInterpreter()`.
- Qore's `fork()` called from Python code of the main interpreter (a Python process hosting Qore) runs Python's fork
  protocol, as `os.fork()` does: `callQoreFunction()` recognizes Qore's `fork()` and sets the thread-local
  `python_fork_protocol_tstate` to the thread state it releases for the call (`QorePythonForkProtocolHelper`), and
  handlers registered with `pthread_atfork()` (`qore_python_register_fork_protocol()`) run the protocol.
  `pthread_atfork()` runs prepare handlers in the reverse order of their registration and parent/child handlers in
  the order of their registration, and the module registers them after the Qore library's: the prepare handler takes
  the GIL again (with the saved thread state, as `~QorePythonReleaseGilHelper()` does) and calls `PyOS_BeforeFork()`
  before Qore's handlers take their locks (the order of `os.fork()`); the parent and child handlers call
  `PyOS_AfterFork_Parent()` / `PyOS_AfterFork_Child()` after Qore's handlers and release the GIL again.  Qore's
  `fork()` stops its signal thread before `fork()`, so the GIL is not held while it waits for that thread.  The handlers
  cannot be removed, so the module marks itself `RTLD_NODELETE`.  In the child, Python runs its at-fork functions,
  and the threading module forgets the threads of the parent, so the child's exit does not wait for them.
- The protocol is run only when the main interpreter is the only interpreter (checked with the GIL held, just
  before `PyOS_BeforeFork()`; the module's interpreters share the main GIL, so none is created meanwhile):
  `PyOS_AfterFork_Child()` deletes every other interpreter, which crashes the child in CPython 3.12 to 3.14 and hangs
  it in 3.10 and 3.11 with `os.fork()` as well, also with no Qore code involved (CPython issue gh-139759).  With
  interpreters of Qore present, Qore's `fork()` is made without the protocol: they can be used in the child, but
  Python's own exit (`Py_FinalizeEx()`, `threading._shutdown()`) waits forever for the parent's non-daemon threads,
  as with any `fork()` that Python does not make (ex: through ctypes, or Qore's `fork()` called from Qore code).
  Follow-up once gh-139759 is fixed upstream: run the protocol with interpreters of Qore present, and make the module
  forget, in the child, the interpreters that `PyOS_AfterFork_Child()` deleted (their programs invalid, their thread
  states and object references dropped without being released); this cannot be reached or tested while CPython
  crashes first.

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
- the interpreters of the module are created with `allow_daemon_threads = 0` (and `allow_exec = 0`,
  `check_multi_interp_extensions = 1`): starting a daemon thread raises `RuntimeError`

## Debugging Tips

### Common Crashes and Their Causes

1. **"thread state already initialized"**: `PyThreadState_New()` tried to reuse
   `interp->_initial_thread` when `threads.head` is NULL but `_status.initialized` is 1.
   Usually caused by improper thread state cleanup.

2. **SIGSEGV in `_PyEvalFramePushAndInit`**: Thread state's data stack is corrupted, ex: the state was deleted by
   `PyGILState_Release()` (its `gilstate_counter` reached 0) while the module still used it.

3. **"non-NULL old thread state" in free-threading**: Tried to attach a thread state
   when one was already attached. Use `PyGILState_GetThisThreadState()` to check first.

### Useful Debug Output

Enable debug level 5 for detailed GIL tracking:
```
qore --debug=5 -l python your_script.q
```

Key debug messages to look for:
- `setContext() ENTRY`: Shows thread state pointers and tracking state
- `got thread context`: Shows `GIL:` (PyGILState_Check), `hG:` (our tracking)
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
- `QorePythonDeclHandle.h/.cpp`: handles of the Qore declarations held by Python types (see "Lifetime of Qore
  Declarations Used from Python")

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
