/* indent-tabs-mode: nil -*- */
/*
    qore Python module

    Copyright (C) 2020 - 2026 Qore Technologies, s.r.o.

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

#include "QorePythonDeclHandle.h"
#include "python-module.h"

#include <qore/QoreForkHandler.h>

#include <map>
#include <optional>

namespace {
// the handles of each Program's declarations; handles remove themselves when they are destroyed
typedef std::map<QorePythonDeclHandle*, std::weak_ptr<QorePythonDeclHandle>> handle_map_t;
typedef std::map<QoreProgram*, handle_map_t> program_handle_map_t;

struct DeclHandleRegistry {
    //! guards map and the uses of all handles; a leaf lock: its holders take no other lock (see the fork handlers)
    QoreThreadLock lck;
    //! signaled when the last use of a handle ends
    QoreCondition cond;
    //! the number of threads waiting on cond
    unsigned waiting = 0;
    program_handle_map_t map;
};

// not destroyed, so that handles of capsules freed while the process exits can still deregister
DeclHandleRegistry& get_registry() {
    static DeclHandleRegistry* registry = new DeclHandleRegistry;
    return *registry;
}

QoreForkHandlerRegistration* decl_handle_fork_reg = nullptr;

// the capsule name of declaration handles
constexpr const char* QORE_PYTHON_DECL_CAPSULE_NAME = "qore.decl";

void decl_capsule_destructor(PyObject* capsule) {
    delete static_cast<std::shared_ptr<QorePythonDeclHandle>*>(PyCapsule_GetPointer(capsule,
        QORE_PYTHON_DECL_CAPSULE_NAME));
}
}

thread_local QorePythonDeclUse* QorePythonDeclUse::current = nullptr;

int QorePythonDeclHandle::registerForkHandler() {
    assert(!decl_handle_fork_reg);
    decl_handle_fork_reg = qore_register_fork_handler(QFR_MODULE_LEAF, "python_qore_declaration_handles",
        qore_fork_prepare_thread_lock, qore_fork_parent_thread_lock, qore_fork_child_thread_lock,
        &get_registry().lck);
    return decl_handle_fork_reg ? 0 : -1;
}

void QorePythonDeclHandle::unregisterForkHandler() {
    qore_unregister_fork_handler(decl_handle_fork_reg);
    decl_handle_fork_reg = nullptr;
}

std::shared_ptr<QorePythonDeclHandle> QorePythonDeclHandle::create(const void* decl, QoreProgram* owner, Kind kind,
        const char* name) {
    std::shared_ptr<QorePythonDeclHandle> rv(new QorePythonDeclHandle(decl, owner, kind, name));
    if (owner) {
        DeclHandleRegistry& registry = get_registry();
        AutoLocker al(registry.lck);
        registry.map[owner].insert(handle_map_t::value_type(rv.get(), rv));
    }
    return rv;
}

QorePythonDeclHandle::~QorePythonDeclHandle() {
    assert(!users);
    if (owner) {
        DeclHandleRegistry& registry = get_registry();
        AutoLocker al(registry.lck);
        // the entry has been removed if the Program has invalidated its handles
        program_handle_map_t::iterator i = registry.map.find(owner);
        if (i != registry.map.end()) {
            i->second.erase(this);
            if (i->second.empty()) {
                registry.map.erase(i);
            }
        }
    }
}

bool QorePythonDeclHandle::startUse() {
    AutoLocker al(get_registry().lck);
    if (!valid.load(std::memory_order_relaxed)) {
        return false;
    }
    ++users;
    return true;
}

void QorePythonDeclHandle::endUse() {
    DeclHandleRegistry& registry = get_registry();
    AutoLocker al(registry.lck);
    assert(users);
    if (!--users && registry.waiting) {
        registry.cond.broadcast();
    }
}

void QorePythonDeclHandle::invalidate(QoreProgram* pgm) {
    DeclHandleRegistry& registry = get_registry();
    // the handles are referenced so that they are not destroyed while their uses end; the last references to the
    // handles whose capsules are freed meanwhile are released after the lock, as their destructors take it
    std::vector<std::shared_ptr<QorePythonDeclHandle>> handles;
    bool used = false;
    {
        AutoLocker al(registry.lck);
        program_handle_map_t::iterator i = registry.map.find(pgm);
        if (i == registry.map.end()) {
            return;
        }
        handles.reserve(i->second.size());
        for (auto& hi : i->second) {
            // a handle whose destructor is waiting for the lock is not used anymore
            std::shared_ptr<QorePythonDeclHandle> handle = hi.second.lock();
            if (handle) {
                handle->valid.store(false, std::memory_order_release);
                used = used || handle->users > QorePythonDeclUse::countThreadUses(handle.get());
                handles.push_back(std::move(handle));
            }
        }
        registry.map.erase(i);
    }
    if (!used) {
        return;
    }

    // wait for the uses in other threads that started before the handles were invalidated (the uses of this thread
    // are not waited for, see QorePythonDeclUse::countThreadUses()); a use in another thread can wait for the GIL, so
    // the GIL is not held meanwhile
    {
        std::optional<QorePythonReleaseGilHelper> prgh;
        if (qore_python_attached_thread_state()) {
            prgh.emplace();
        }
        AutoLocker al(registry.lck);
        for (auto& handle : handles) {
            unsigned own = QorePythonDeclUse::countThreadUses(handle.get());
            while (handle->users > own) {
                ++registry.waiting;
                registry.cond.wait(registry.lck);
                --registry.waiting;
            }
        }
    }
}

void QorePythonDeclHandle::raiseDeleted() const {
    const char* kind_str = "declaration";
    switch (kind) {
        case CLASS: kind_str = "class"; break;
        case HASHDECL: kind_str = "hashdecl"; break;
        case ENUM: kind_str = "enum"; break;
    }
    PyErr_Format(PyExc_RuntimeError, "Qore %s '%s' cannot be used: the Qore Program that declared it has been deleted",
        kind_str, name.c_str());
}

PyObject* QorePythonDeclHandle::newCapsule(const std::shared_ptr<QorePythonDeclHandle>& handle) {
    std::unique_ptr<std::shared_ptr<QorePythonDeclHandle>> data(new std::shared_ptr<QorePythonDeclHandle>(handle));
    PyObject* capsule = PyCapsule_New(data.get(), QORE_PYTHON_DECL_CAPSULE_NAME, decl_capsule_destructor);
    if (capsule) {
        data.release();
    }
    return capsule;
}

QorePythonDeclHandle* QorePythonDeclHandle::getCapsuleHandle(PyObject* capsule) {
    if (!capsule || !PyCapsule_IsValid(capsule, QORE_PYTHON_DECL_CAPSULE_NAME)) {
        return nullptr;
    }
    return static_cast<std::shared_ptr<QorePythonDeclHandle>*>(PyCapsule_GetPointer(capsule,
        QORE_PYTHON_DECL_CAPSULE_NAME))->get();
}
