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

#ifndef _QORE_PYTHON_QORETHREADATTACHHELPER_H

#define _QORE_PYTHON_QORETHREADATTACHHELPER_H

#include "python-module.h"

constexpr int LogLevel = 10;

class QoreThreadAttacher {
public:
    DLLLOCAL QoreThreadAttacher() : attached(false) {
    }

    DLLLOCAL ~QoreThreadAttacher() {
        if (attached) {
            detach();
        }
    }

    //! attaches the thread to Qore if it is not registered
    /** @return QFT_OK if the thread was attached, QFT_REGISTERED if it was already attached or registered, or
        QFT_ERROR if it could not be registered (ex: the maximum number of threads is reached)
    */
    DLLLOCAL int attach() {
        if (attached) {
            return QFT_REGISTERED;
        }
        return attachIntern();
    }

    DLLLOCAL void detach() {
        if (attached) {
            detachIntern();
        }
    }

    DLLLOCAL operator bool() const {
        return attached;
    }

private:
    bool attached;

    DLLLOCAL int attachIntern() {
        assert(!attached);
        int rc = q_register_foreign_thread();
        if (rc == QFT_OK) {
            attached = true;
            printd(LogLevel, "Thread %ld attached to Qore\n", pthread_self());
        }
        return rc;
    }

    DLLLOCAL void detachIntern() {
        assert(attached);
        printd(LogLevel, "Detaching thread %ld from Qore\n", pthread_self());
        q_deregister_foreign_thread();
        attached = false;
    }
};

// class that serves to attach a thread to Qore if not already attached
// if attached in the constructor, then it will detach in the destructor
class QoreThreadAttachHelper {
public:
    //! attaches the thread to Qore if it is not registered
    /** @return 0 for OK, -1 if the thread could not be registered
    */
    DLLLOCAL int attach() {
        int rc = attacher.attach();
        attached = rc == QFT_OK;
        return rc == QFT_ERROR ? -1 : 0;
    }

    DLLLOCAL ~QoreThreadAttachHelper() {
        if (attached) {
            attacher.detach();
        }
    }

private:
    QoreThreadAttacher attacher;
    bool attached = false;
};

#endif