/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    qore Python module

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

#ifndef _QORE_PYTHON_FORK_GIL_H

#define _QORE_PYTHON_FORK_GIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 if, in the child of fork(), the GIL can never be acquired, because a thread that does not exist in the
   child held it (or one of the mutexes that protect it) when the process was copied; returns 0 if the GIL is free or
   held by the calling thread.

   Must be called in the child by the thread that called fork(), before Python is used in the child; it only reads
   memory and tries the GIL's mutexes, so it can be called from a fork handler.
*/
int qore_python_fork_child_gil_lost(void);

#ifdef __cplusplus
}
#endif

#endif
