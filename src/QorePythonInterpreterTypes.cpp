/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QorePythonInterpreterTypes.cpp

    Qore Programming Language python Module

    Copyright (C) 2026 Qore Technologies, s.r.o.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "QorePythonInterpreterTypes.h"
#include "QoreLoader.h"
#include "JavaLoader.h"
#include "QoreMetaPathFinder.h"
#include "ModuleNamespace.h"
#include "PythonQoreClass.h"
#include "PythonQoreCallable.h"

#include <qore/QoreForkHandler.h>

#include <atomic>
#include <map>
#include <utility>

namespace {
//! the types of the interpreters that initialized the qoreloader module; guarded by types_lock
//! the identifier of the interpreter is kept with the types, which are not used if the identifier differs: they belong
//! to an interpreter that ended at the same address without freeing the module, whose types may be gone
typedef std::map<PyInterpreterState*, std::pair<int64_t, QorePythonInterpreterTypes*>> types_map_t;
types_map_t types_map;
QoreThreadLock types_lock;
QoreForkHandlerRegistration* types_lock_fork_reg = nullptr;

//! incremented when types are registered or deregistered, so that the cache of a thread is not used for an
//! interpreter whose types changed (a new interpreter can have the address of one that was deleted)
std::atomic<unsigned long> types_generation{0};

//! the types last found by a thread
struct TypesCache {
    PyInterpreterState* interp = nullptr;
    int64_t interp_id = -1;
    unsigned long generation = 0;
    const QorePythonInterpreterTypes* types = nullptr;
};
thread_local TypesCache types_cache;

//! sets a new reference to a type, returning -1 with a Python exception on error
int set_type(PyTypeObject*& target, PyObject* type) {
    if (!type) {
        return -1;
    }
    assert(PyType_Check(type));
    target = reinterpret_cast<PyTypeObject*>(type);
    return 0;
}

//! creates an object of a loader type (with no arguments)
int set_loader(PyObject*& target, PyTypeObject* type) {
    QorePythonReferenceHolder args(PyTuple_New(0));
    target = args ? PyObject_CallObject(reinterpret_cast<PyObject*>(type), *args) : nullptr;
    return target ? 0 : -1;
}
}

int qore_python_interpreter_types_init(QorePythonInterpreterTypes& types) {
    assert(!types.interp);
    PyInterpreterState* interp = PyInterpreterState_Get();
    int64_t interp_id = PyInterpreterState_GetID(interp);
    // the module can be initialized again in an interpreter (ex: after it was removed from sys.modules) only once the
    // module initialized before has been freed: the Python types of Qore classes made by its program derive from its
    // types, which would no longer be found; the types of an interpreter that ended without freeing the module (ex:
    // an interpreter left in memory, or deleted in a child of fork()) are replaced, as the address of an interpreter
    // can be the one of an interpreter that ended
    {
        AutoLocker al(types_lock);
        types_map_t::const_iterator i = types_map.find(interp);
        if (i != types_map.end() && i->second.first == interp_id) {
            PyErr_SetString(PyExc_ImportError, "the qoreloader module is already initialized in this interpreter; it "
                "cannot be initialized again before the module initialized before is freed");
            return -1;
        }
    }
    if (set_type(types.qore_object_base, PythonQoreClass::createBaseType())
        || set_type(types.qore_callable, qore_python_create_callable_type())
        || set_type(types.module_namespace, ModuleNamespace_CreateType())
        || set_type(types.qore_loader, QoreLoader::createType())
        || set_type(types.java_loader, JavaLoader::createType())
        || set_type(types.meta_path_finder, QoreMetaPathFinder::createType())
        || set_type(types.thread_sentinel, qore_python_create_thread_sentinel_type())
        || set_loader(types.qore_loader_obj, types.qore_loader)
        || set_loader(types.java_loader_obj, types.java_loader)) {
        qore_python_interpreter_types_clear(types);
        return -1;
    }

    bool registered;
    {
        AutoLocker al(types_lock);
        // memory errors leave the types unregistered
        try {
            types_map[interp] = std::make_pair(interp_id, &types);
            registered = true;
        } catch (...) {
            registered = false;
        }
    }
    if (!registered) {
        qore_python_interpreter_types_clear(types);
        PyErr_NoMemory();
        return -1;
    }
    types.interp = interp;
    types.interp_id = interp_id;
    // a thread can have cached that the interpreter has no types
    ++types_generation;
    return 0;
}

void qore_python_interpreter_types_clear(QorePythonInterpreterTypes& types) {
    if (types.interp) {
        {
            AutoLocker al(types_lock);
            types_map_t::iterator i = types_map.find(types.interp);
            if (i != types_map.end() && i->second.second == &types) {
                types_map.erase(i);
            }
        }
        ++types_generation;
        types.interp = nullptr;
        types.interp_id = -1;
    }
    Py_CLEAR(types.java_loader_obj);
    Py_CLEAR(types.qore_loader_obj);
    Py_CLEAR(types.thread_sentinel);
    Py_CLEAR(types.meta_path_finder);
    Py_CLEAR(types.java_loader);
    Py_CLEAR(types.qore_loader);
    Py_CLEAR(types.module_namespace);
    Py_CLEAR(types.qore_callable);
    Py_CLEAR(types.qore_object_base);
}

int qore_python_interpreter_types_traverse(QorePythonInterpreterTypes& types, visitproc visit, void* arg) {
    Py_VISIT(types.qore_object_base);
    Py_VISIT(types.qore_callable);
    Py_VISIT(types.module_namespace);
    Py_VISIT(types.qore_loader);
    Py_VISIT(types.java_loader);
    Py_VISIT(types.meta_path_finder);
    Py_VISIT(types.thread_sentinel);
    Py_VISIT(types.qore_loader_obj);
    Py_VISIT(types.java_loader_obj);
    return 0;
}

const QorePythonInterpreterTypes* qore_python_get_interpreter_types() {
#if PY_VERSION_HEX >= 0x030D0000
    PyThreadState* tstate = PyThreadState_GetUnchecked();
#else
    PyThreadState* tstate = _PyThreadState_UncheckedGet();
#endif
    if (!tstate) {
        return nullptr;
    }
    PyInterpreterState* interp = qore_python_get_interpreter(tstate);
    int64_t interp_id = PyInterpreterState_GetID(interp);
    unsigned long generation = types_generation.load();
    TypesCache& cache = types_cache;
    if (cache.interp == interp && cache.interp_id == interp_id && cache.generation == generation) {
        return cache.types;
    }
    const QorePythonInterpreterTypes* types = nullptr;
    {
        AutoLocker al(types_lock);
        types_map_t::const_iterator i = types_map.find(interp);
        // not the types of an interpreter that ended at the same address
        if (i != types_map.end() && i->second.first == interp_id) {
            types = i->second.second;
        }
    }
    cache.interp = interp;
    cache.interp_id = interp_id;
    cache.generation = generation;
    cache.types = types;
    return types;
}

int qore_python_interpreter_types_register_fork_handler() {
    assert(!types_lock_fork_reg);
    // the holders of the lock only change or read the map, so it is a leaf (QFR_MODULE_LEAF) acquired before fork()
    types_lock_fork_reg = qore_register_fork_handler(QFR_MODULE_LEAF, "python_interpreter_types",
        qore_fork_prepare_thread_lock, qore_fork_parent_thread_lock, qore_fork_child_thread_lock, &types_lock);
    return types_lock_fork_reg ? 0 : -1;
}

void qore_python_interpreter_types_unregister_fork_handler() {
    qore_unregister_fork_handler(types_lock_fork_reg);
    types_lock_fork_reg = nullptr;
}
