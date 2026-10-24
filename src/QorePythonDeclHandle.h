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

#ifndef _QORE_PYTHON_DECL_HANDLE_H

#define _QORE_PYTHON_DECL_HANDLE_H

#include <Python.h>

#include <qore/Qore.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

//! A declaration of a Qore Program held by Python: a class, a hashdecl or an enum
/** Python types made for Qore declarations can outlive the Program that owns the declaration; the handle is
    invalidated by the Program cleanup callback before the Program's declarations are freed (see
    QorePythonDeclHandle::invalidate()), and a declaration is only used while a QorePythonDeclUse of its handle is held.

    A declaration without an owning Program (one in the static system namespace) is never invalidated.

    The registry of handles and the uses of all handles are guarded by one leaf lock, which has fork handlers (see
    registerForkHandler())
*/
class QorePythonDeclHandle {
    friend class QorePythonDeclUse;
public:
    //! the kind of declaration, for error messages
    enum Kind {
        CLASS,
        HASHDECL,
        ENUM,
    };

    //! creates a handle of a declaration
    /** @param decl the declaration, which must be alive
        @param owner the Program that owns the declaration (its getProgram()), or nullptr if it has none
        @param kind the kind of declaration
        @param name the name of the declaration
    */
    DLLLOCAL static std::shared_ptr<QorePythonDeclHandle> create(const void* decl, QoreProgram* owner, Kind kind,
            const char* name);

    DLLLOCAL ~QorePythonDeclHandle();

    //! invalidates the handles of the declarations of the Program
    /** Called from the Program cleanup callback, before the Program's declarations are freed; waits for the uses of
        the declarations in other threads to end
    */
    DLLLOCAL static void invalidate(QoreProgram* pgm);

    //! returns true if the handle has not been invalidated
    /** A cached Python type whose handle is invalid is not used: the declaration has been freed and its address can
        belong to another declaration.  Takes no lock, so it can be called with other locks held
    */
    DLLLOCAL bool isValid() const {
        return valid.load(std::memory_order_acquire);
    }

    //! returns the declaration
    DLLLOCAL const void* getDecl() const {
        return decl;
    }

    //! returns the name of the declaration
    DLLLOCAL const std::string& getName() const {
        return name;
    }

    //! raises a Python RuntimeError for a declaration whose Program has been deleted
    DLLLOCAL void raiseDeleted() const;

    //! returns a new capsule holding the handle; returns nullptr with a Python exception set on error
    DLLLOCAL static PyObject* newCapsule(const std::shared_ptr<QorePythonDeclHandle>& handle);

    //! returns the handle held by a capsule made by newCapsule(), or nullptr if the object is not such a capsule
    /** The handle is valid as long as the capsule is
    */
    DLLLOCAL static QorePythonDeclHandle* getCapsuleHandle(PyObject* capsule);

    //! registers the fork handlers of the lock of the handles; returns -1 on error
    DLLLOCAL static int registerForkHandler();

    //! removes the fork handlers registered with registerForkHandler()
    DLLLOCAL static void unregisterForkHandler();

private:
    //! the declaration
    const void* decl;
    //! the owning Program; nullptr if the declaration has none
    QoreProgram* owner;
    //! the kind of declaration
    Kind kind;
    //! the name of the declaration
    std::string name;

    //! the number of current uses; guarded by the lock of the handles
    unsigned users = 0;
    //! false once the owning Program has deleted the declaration; written with the lock of the handles held
    std::atomic<bool> valid{true};

    DLLLOCAL QorePythonDeclHandle(const void* decl, QoreProgram* owner, Kind kind, const char* name)
            : decl(decl), owner(owner), kind(kind), name(name) {
    }

    //! starts a use; returns false if the handle is invalid
    DLLLOCAL bool startUse();

    //! ends a use started with startUse()
    DLLLOCAL void endUse();
};

//! A use of a declaration: while it is held, the declaration is not freed
/** If the handle has been invalidated, the use fails and the declaration must not be used
*/
class QorePythonDeclUse {
public:
    DLLLOCAL QorePythonDeclUse(QorePythonDeclHandle* handle) {
        if (handle && handle->startUse()) {
            this->handle = handle;
        }
    }

    DLLLOCAL ~QorePythonDeclUse() {
        if (handle) {
            handle->endUse();
        }
    }

    QorePythonDeclUse(const QorePythonDeclUse&) = delete;
    QorePythonDeclUse& operator=(const QorePythonDeclUse&) = delete;

    //! returns true if the declaration can be used
    DLLLOCAL explicit operator bool() const {
        return handle != nullptr;
    }

private:
    QorePythonDeclHandle* handle = nullptr;
};

//! the uses of the declarations needed by an operation, held until it ends
typedef std::vector<std::unique_ptr<QorePythonDeclUse>> QorePythonDeclUseList;

#endif
