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

#include "PythonQoreClass.h"
#include "QoreLoader.h"
#include "QorePythonProgram.h"
#include "QorePythonStackLocationHelper.h"
#include "QorePythonDeclHandle.h"

#include <string.h>
#include <memory>
#include <set>
#include <map>

#include <frameobject.h>
#if PY_VERSION_HEX < 0x030C0000
// the member types and flags of Python 3.11 (Py_T_PYSSIZET and Py_READONLY since Python 3.12)
#include <structmember.h>
#define Py_T_PYSSIZET T_PYSSIZET
#define Py_READONLY READONLY
#endif

#include <pthread.h>

static constexpr const char* QCLASS_KEY = "__$QCLS__";

// Python callables can survive their Qore program during Python shutdown.
// Their capsules own the method definition and its strings until the callable dies.
struct method_capsule_t {
    const QoreMethod& method;
    //! the handle of the method's class; the method is used only while the handle is valid
    std::shared_ptr<QorePythonDeclHandle> handle;
    QorePythonProgram* program;
    const std::string name;
    const std::string doc;
    PyMethodDef definition;

    method_capsule_t(const QoreMethod& method, QorePythonProgram* program, const PyMethodDef& original)
            : method(method),
              handle(QorePythonDeclHandle::create(method.getClass(), method.getClass()->getProgram(),
                  QorePythonDeclHandle::CLASS, method.getClass()->getName())),
              program(program), name(original.ml_name),
              doc(original.ml_doc ? original.ml_doc : ""),
              definition{name.c_str(), original.ml_meth, original.ml_flags,
                  original.ml_doc ? doc.c_str() : nullptr} {
        program->weakRef();
    }

    ~method_capsule_t() {
        program->weakDeref();
    }
};

static void method_capsule_destructor(PyObject* capsule) {
    delete static_cast<method_capsule_t*>(PyCapsule_GetPointer(capsule, nullptr));
}

static PyObject* new_method_function(const QoreMethod& method, QorePythonProgram* program,
        const PyMethodDef& definition) {
    std::unique_ptr<method_capsule_t> data(new method_capsule_t(method, program, definition));
    QorePythonReferenceHolder capsule(PyCapsule_New(data.get(), nullptr, method_capsule_destructor));
    if (!capsule) {
        return nullptr;
    }
    method_capsule_t* owned = data.release();
    return PyCFunction_New(&owned->definition, *capsule);
}

// returns the data of the method's capsule, or nullptr with a Python exception set if the method cannot be called
static method_capsule_t* get_capsule_method(PyObject* capsule) {
    method_capsule_t* data = static_cast<method_capsule_t*>(PyCapsule_GetPointer(capsule, nullptr));
    if (!data) {
        return nullptr;
    }
    if (!data->program->isValid() || data->program->isDestroyed()) {
        PyErr_SetString(PyExc_RuntimeError, "the owning Qore program has been deleted");
        return nullptr;
    }
    return data;
}

void PythonQoreClass::py_free(PyQoreObject* self) {
    //printd(5, "PythonQoreClass::py_free() self: %p '%s'\n", self, Py_TYPE(self)->tp_name);
    PyObject_GC_Del(self);
}

bool PyQoreObject_Check(PyObject* obj) {
    return obj && PyObject_TypeCheck(obj, &PythonQoreObjectBase_Type);
}

bool PyQoreObjectType_Check(PyTypeObject* type) {
    assert(type);
    // FIXME: use PyDict_Contains() instead
    return type->tp_dict && PyDict_GetItemString(type->tp_dict, QCLASS_KEY);
}

PythonQoreClass::PythonQoreClass(QorePythonProgram* pypgm, PyTypeObject* type, const QoreClass& qcls) {
    Py_INCREF((PyObject*)type);
    py_type = type;
    pypgm->insertClass(&qcls, this);
    // do not save the qore class to the python class, as the python class may be a builtin class and the Qore class
    // can be deleted afterwards
}

static PyMethodDef qore_dir_def = {"__dir__", PythonQoreClass::py_dir, METH_O,
    "returns the Python attributes and the public members of the Qore object"};

PythonQoreClass::PythonQoreClass(QorePythonProgram* pypgm, const char* module_name, const QoreClass& qcls)
        : handle(QorePythonDeclHandle::create(&qcls, qcls.getProgram(), QorePythonDeclHandle::CLASS,
            qcls.getName())) {
    //printd(5, "PythonQoreClass::PythonQoreClass() %s.%s py_type: %p\n", module_name, qcls.getName(), &py_type);

    name.sprintf("%s.%s", module_name, qcls.getName());
    const char* namestr = pypgm->saveString(name.c_str());

    doc.sprintf("Python wrapper class for Qore class %s", qcls.getName());
    const char* docstr = pypgm->saveString(doc.c_str());

    static PyMemberDef members[] = {
        // the objects support weak references
        {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(PyQoreObject, weakreflist), Py_READONLY, nullptr},
        {nullptr, 0, 0, 0, nullptr},
    };

    PyType_Slot slots[] = {
        {Py_tp_doc, (void*)docstr},
        {Py_tp_dealloc, (void*)PythonQoreClass::py_dealloc},
        {Py_tp_traverse, (void*)PythonQoreClass::py_traverse},
        {Py_tp_clear, (void*)PythonQoreClass::py_clear},
        {Py_tp_repr, (void*)PythonQoreClass::py_repr},
        {Py_tp_getattro, (void*)PythonQoreClass::py_getattro},
        {Py_tp_setattro, (void*)PythonQoreClass::py_setattro},
        {Py_tp_members, (void*)members},
        {Py_tp_base, (void*)&PythonQoreObjectBase_Type},
        {Py_tp_alloc, (void*)PyType_GenericAlloc},
        {Py_tp_init, (void*)PythonQoreClass::py_init},
        {Py_tp_new, (void*)PythonQoreClass::py_new},
        {Py_tp_free, (void*)PythonQoreClass::py_free},
        {0, nullptr},
    };

    PyType_Spec spec = {
        .name = namestr,
        .basicsize = sizeof(PyQoreObject),
        .itemsize = 0,
        .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,
        .slots = slots,
    };

    clsset_t cls_set;

    // get single base class - Python and Qore's multiple inheritance models are not compatible
    // we can only set a single class for the Python base class, so if there are multiple
    // base classes, then we populate methods instead directly
    QorePythonReferenceHolder bases;
    {
        QoreParentClassIterator ci(qcls);
        while (ci.next()) {
            if (ci.getAccess() > Private) {
                continue;
            }

            PythonQoreClass* base_cls = pypgm->findCreatePythonClass(ci.getParentClass(), module_name);
            if (!base_cls) {
                // the Python exception is set
                return;
            }
            PyTypeObject* base_type = base_cls->getPythonType();
            // the type of a Python class (the parent class is a Qore class of a Python class) does not have the
            // layout of the types of Qore classes, so it cannot be the base class; the methods of the parent class
            // are added instead
            if (!PyType_IsSubtype(base_type, &PythonQoreObjectBase_Type)) {
                break;
            }
            bases = PyTuple_New(1);
            if (!bases) {
                return;
            }
            cls_set.insert(&ci.getParentClass());
            Py_INCREF(reinterpret_cast<PyObject*>(base_type));
            PyTuple_SET_ITEM(*bases, 0, reinterpret_cast<PyObject*>(base_type));
            break;
        }
    }

    py_type = bases
        ? reinterpret_cast<PyTypeObject*>(PyType_FromSpecWithBases(&spec, *bases))
        : reinterpret_cast<PyTypeObject*>(PyType_FromSpec(&spec));
    //printd(5, "PythonQoreClass::PythonQoreClass() %s py_type: %p\n", qcls.getName(), py_type);
    if (!py_type) {
        // the Python exception is set; findCreatePythonClass() returns nullptr
        return;
    }
    assert(py_type->tp_dict);

    // set of normal method names
    cstrset_t meth_set;

    // no hint: the creation of the base classes above can change the map (see findCreatePythonClass())
    pypgm->insertClass(&qcls, this);

    // setup class; the first parent class is not populated if it is the base class
    populateClass(pypgm, qcls, cls_set, meth_set, (bool)bases);

    // dir() lists the public members of the Qore object
    {
        QorePythonReferenceHolder func(PyCFunction_NewEx(&qore_dir_def, nullptr, nullptr));
        QorePythonReferenceHolder meth(func ? PyInstanceMethod_New(*func) : nullptr);
        if (!meth || PyDict_SetItemString(py_type->tp_dict, "__dir__", *meth) < 0) {
            PyErr_Clear();
        }
    }

    // add normal methods
    for (size_t i = 0; i < py_normal_meth_vec.size(); ++i) {
        PyMethodDef& md = py_normal_meth_vec[i];
        QorePythonReferenceHolder method_capsule(py_normal_meth_obj_vec[i].release());
        const QoreMethod* m = static_cast<const QoreMethod*>(PyCapsule_GetPointer(*method_capsule, nullptr));
        QorePythonReferenceHolder func(new_method_function(*m, pypgm, md));
        assert(func);
        QorePythonReferenceHolder meth(PyInstanceMethod_New(*func));
        assert(meth);
        PyDict_SetItemString(py_type->tp_dict, m->getName(), *meth);
        meth_names.emplace_back(m->getName());
    }
    py_normal_meth_obj_vec.clear();

    // add static methods
    for (size_t i = 0; i < py_static_meth_vec.size(); ++i) {
        PyMethodDef& md = py_static_meth_vec[i];
        QorePythonReferenceHolder method_capsule(py_static_meth_obj_vec[i].release());
        const QoreMethod* m = static_cast<const QoreMethod*>(PyCapsule_GetPointer(*method_capsule, nullptr));
        QorePythonReferenceHolder func(new_method_function(*m, pypgm, md));
        assert(func);
        QorePythonReferenceHolder meth(PyStaticMethod_New(*func));
        assert(meth);
        PyDict_SetItemString(py_type->tp_dict, m->getName(), *meth);
        meth_names.emplace_back(m->getName());
    }
    py_static_meth_obj_vec.clear();
    // the method definitions refer to the names of the Qore methods, which are freed with the Qore class; the
    // Python functions have their own copies (see method_capsule_t)
    py_normal_meth_vec.clear();
    py_static_meth_vec.clear();

    // add the handle of the Qore class to type dictionary
    QorePythonReferenceHolder qore_class(QorePythonDeclHandle::newCapsule(handle));
    if (!qore_class || PyDict_SetItemString(py_type->tp_dict, QCLASS_KEY, *qore_class) < 0) {
        PyErr_Clear();
    }
}

PythonQoreClass::~PythonQoreClass() {
    printd(5, "PythonQoreClass::~PythonQoreClass() this: %p '%s'\n", this, name.c_str());
    // py_type should have been released via release() before interpreter deletion
    // Only decref if it's still set (shouldn't happen in normal cleanup)
    if (py_type) {
        Py_DECREF(py_type);
    }
}

void PythonQoreClass::release() {
    // Release Python reference before interpreter is cleared
    // This must be called while the interpreter is still valid
    if (py_type) {
        Py_DECREF(py_type);
        py_type = nullptr;
    }
}

void PythonQoreClass::clearMethods() {
    // Remove bridge entry points while the type's interpreter is still valid.
    // Any bound functions that survive retain their own capsule-owned definitions.
    if (py_type && py_type->tp_dict) {
        // Remove all method objects we added; the Qore class may already have been freed with its Program, so its
        // method names are not used
        for (const std::string& meth_name : meth_names) {
            PyDict_DelItemString(py_type->tp_dict, meth_name.c_str());
            PyErr_Clear();  // Ignore errors if key doesn't exist
        }
        // Also remove the Qore class capsule
        PyDict_DelItemString(py_type->tp_dict, QCLASS_KEY);
        PyErr_Clear();
    }
}

// a static member of a Qore class held by the get and set functions of its descriptor
struct static_member_capsule_t {
    const QoreExternalStaticMember& mem;
    //! the handle of the member's class; the member is used only while the handle is valid
    std::shared_ptr<QorePythonDeclHandle> handle;

    static_member_capsule_t(const QoreExternalStaticMember& mem, const std::shared_ptr<QorePythonDeclHandle>& handle)
            : mem(mem), handle(handle) {
    }
};

static void static_member_capsule_destructor(PyObject* capsule) {
    delete static_cast<static_member_capsule_t*>(PyCapsule_GetPointer(capsule, nullptr));
}

// returns the member of a static member capsule, or nullptr with a Python exception set if its class has been deleted
static const QoreExternalStaticMember* get_static_member(PyObject* capsule, QorePythonDeclUse& use) {
    static_member_capsule_t* data = static_cast<static_member_capsule_t*>(PyCapsule_GetPointer(capsule, nullptr));
    assert(data);
    if (!use) {
        data->handle->raiseDeleted();
        return nullptr;
    }
    return &data->mem;
}

// returns the handle of a static member capsule
static QorePythonDeclHandle* get_static_member_handle(PyObject* capsule) {
    static_member_capsule_t* data = static_cast<static_member_capsule_t*>(PyCapsule_GetPointer(capsule, nullptr));
    assert(data);
    return data->handle.get();
}

// gives the value of a static member of a Qore class; self is a capsule holding the member
static PyObject* qore_static_member_get(PyObject* capsule, PyObject* /* unused */) {
    // the member's class is not freed while it is used
    QorePythonDeclUse use(get_static_member_handle(capsule));
    const QoreExternalStaticMember* mem = get_static_member(capsule, use);
    if (!mem) {
        return nullptr;
    }
    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getExecutionContext();
    ExceptionSink xsink;
    QoreExternalProgramContextHelper pch(&xsink, qore_python_pgm->getQoreProgram());
    if (!xsink) {
        ValueHolder v(&xsink);
        {
            QorePythonReleaseGilHelper prgh;
            v = mem->getValue();
        }
        // the member is not used anymore: converting and releasing the value can run Qore code
        use.end();
        QorePythonReferenceHolder rv(qore_python_pgm->getPythonValue(*v, &xsink));
        // the value is released without the GIL if this can run Qore code
        qore_python_release(v);
        if (!xsink) {
            return rv.release();
        }
    }
    qore_python_pgm->raisePythonException(xsink);
    return nullptr;
}

// sets the value of a static member of a Qore class; self is a capsule holding the member
static PyObject* qore_static_member_set(PyObject* capsule, PyObject* value) {
    // the member's class is not freed while it is used
    QorePythonDeclUse use(get_static_member_handle(capsule));
    const QoreExternalStaticMember* mem = get_static_member(capsule, use);
    if (!mem) {
        return nullptr;
    }
    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getExecutionContext();
    ExceptionSink xsink;
    QoreExternalProgramContextHelper pch(&xsink, qore_python_pgm->getQoreProgram());
    if (!xsink) {
        ValueHolder v(qore_python_pgm->getQoreValue(&xsink, value), &xsink);
        if (!xsink) {
            // assigning the member releases the old value, which can run Qore code
            QorePythonReleaseGilHelper prgh;
            mem->setValue(*v, &xsink);
        }
        // the member is not used anymore: releasing the value can run Qore code
        use.end();
        qore_python_release(v);
        if (!xsink) {
            Py_RETURN_NONE;
        }
    }
    qore_python_pgm->raisePythonException(xsink);
    return nullptr;
}

static PyMethodDef qore_static_member_get_def = {"get", qore_static_member_get, METH_NOARGS,
    "returns the value of the static member of the Qore class"};
static PyMethodDef qore_static_member_set_def = {"set", qore_static_member_set, METH_O,
    "sets the value of the static member of the Qore class"};

void PythonQoreClass::populateClass(QorePythonProgram* pypgm, const QoreClass& qcls, clsset_t& cls_set,
        cstrset_t& meth_set, bool skip_first) {
    //printd(5, "PythonQoreClass::populateClass() cls: %s cs: %d ms: %d\n", qcls.getName(), (int)cls_set.size(),
    //  (int)meth_set.size());

    {
        QoreMethodIterator i(qcls);
        while (i.next()) {
            const QoreMethod* m = i.getMethod();
            // the constructor, destructor and copy method are not callable as methods
            if (m->getAccess() > Private || m->getMethodType() != MT_Normal) {
                continue;
            }

            //printd(5, "PythonQoreClass::populateClass() adding %s -> %s::%s()\n", name.c_str(), qcls.getName(),
            //  m->getName());
            cstrset_t::iterator mi = meth_set.lower_bound(m->getName());
            if (mi == meth_set.end() || strcmp(*mi, m->getName())) {
                meth_set.insert(mi, m->getName());
                QoreStringMaker mdoc("Python wrapper for Qore class method %s::%s()", qcls.getName(), m->getName());
                const char* mdocstr = pypgm->saveString(mdoc.c_str());
                py_normal_meth_vec.push_back({m->getName(), (PyCFunction)exec_qore_method, METH_VARARGS, mdocstr});
                py_normal_meth_obj_vec.push_back(PyCapsule_New((void*)m, nullptr, nullptr));
            }
        }
    }

    {
        QoreStaticMethodIterator i(qcls);
        while (i.next()) {
            const QoreMethod* m = i.getMethod();
            if (m->getAccess() > Private) {
                continue;
            }
            // issue #4397: check if there is an accessible normal method with the same name, if so, skip it
            {
                ClassAccess access;
                const QoreMethod* nm = qcls.findMethod(m->getName(), access);
                if (nm && access < Internal) {
                    continue;
                }
            }

            //printd(5, "PythonQoreClass::populateClass() adding %s -> static %s::%s()\n", name.c_str(),
            //  qcls.getName(), m->getName());
            cstrset_t::iterator mi = meth_set.lower_bound(m->getName());
            if (mi == meth_set.end() || strcmp(*mi, m->getName())) {
                meth_set.insert(mi, m->getName());
                QoreStringMaker mdoc("Python wrapper for Qore static class method %s::%s()", qcls.getName(),
                    m->getName());
                const char* mdocstr = pypgm->saveString(mdoc.c_str());
                py_static_meth_vec.push_back({m->getName(), (PyCFunction)exec_qore_static_method, METH_VARARGS,
                    mdocstr});
                py_static_meth_obj_vec.push_back(PyCapsule_New((void*)m, nullptr, nullptr));
            }
        }
    }

    ExceptionSink xsink;
    {
        QoreClassConstantIterator i(qcls);
        while (i.next()) {
            const QoreExternalConstant& c = i.get();
            if (c.getAccess() > Private) {
                continue;
            }
            cstrset_t::iterator mi = meth_set.lower_bound(c.getName());
            if (mi == meth_set.end() || strcmp(*mi, c.getName())) {
                meth_set.insert(mi, c.getName());
                ValueHolder qoreval(c.getReferencedValue(), &xsink);
                if (xsink) {
                    // a constant that cannot be read must not abort the import of the entire class; skip it
                    xsink.clear();
                    continue;
                }
                QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
                QorePythonReferenceHolder val(qore_python_pgm->getPythonValue(*qoreval, &xsink));
                if (xsink) {
                    // a constant that has no Python representation (ex: java.time.Instant::MIN, whose year lies
                    // outside the range supported by datetime.datetime) must not abort the import of the entire
                    // class; skip the constant instead
                    xsink.clear();
                    PyErr_Clear();
                    continue;
                }
                assert(val);
                PyDict_SetItemString(py_type->tp_dict, c.getName(), *val);
            }
        }
    }

    // the public static members are descriptors giving and setting their current values
    {
        QorePythonReferenceHolder descr_type;
        std::shared_ptr<QorePythonDeclHandle> cls_handle;
        QoreClassStaticMemberIterator i(qcls);
        while (i.next()) {
            const QoreExternalStaticMember& mem = i.getMember();
            if (mem.getAccess() != Public) {
                continue;
            }
            const char* mname = i.getName();
            cstrset_t::iterator mi = meth_set.lower_bound(mname);
            if (mi != meth_set.end() && !strcmp(*mi, mname)) {
                continue;
            }
            if (!descr_type) {
                descr_type = qore_python_get_qore_static_member_type();
                if (!descr_type) {
                    PyErr_Clear();
                    break;
                }
            }
            if (!cls_handle) {
                cls_handle = QorePythonDeclHandle::create(&qcls, qcls.getProgram(), QorePythonDeclHandle::CLASS,
                    qcls.getName());
            }
            std::unique_ptr<static_member_capsule_t> data(new static_member_capsule_t(mem, cls_handle));
            QorePythonReferenceHolder capsule(PyCapsule_New(data.get(), nullptr, static_member_capsule_destructor));
            if (capsule) {
                data.release();
            }
            QorePythonReferenceHolder get(capsule ? PyCFunction_NewEx(&qore_static_member_get_def, *capsule,
                nullptr) : nullptr);
            QorePythonReferenceHolder set(capsule ? PyCFunction_NewEx(&qore_static_member_set_def, *capsule,
                nullptr) : nullptr);
            QorePythonReferenceHolder descr(get && set
                ? PyObject_CallFunction(*descr_type, "sOO", mname, *get, *set) : nullptr);
            if (!descr || PyDict_SetItemString(py_type->tp_dict, mname, *descr) < 0) {
                PyErr_Clear();
                continue;
            }
            meth_set.insert(mi, mname);
        }
    }

    bool first = false;
    QoreParentClassIterator ci(qcls);
    while (ci.next()) {
        if (ci.getAccess() > Private) {
            continue;
        }

        // skip the first class, as it's already been added as a base class
        if (skip_first && !first) {
            first = true;
            continue;
        }

        const QoreClass& parent_cls = ci.getParentClass();

        clsset_t::iterator i = cls_set.lower_bound(&parent_cls);
        if (i != cls_set.end() && ((*i) == &parent_cls)) {
            //printd(5, "PythonQoreClass::populateClass() %s skipping parent %s\n", qcls.getName(), parent_cls.getName());
            continue;
        }
        //printd(5, "PythonQoreClass::populateClass() %s parent <- %s\n", qcls.getName(), parent_cls.getName());
        cls_set.insert(i, &parent_cls);
        populateClass(pypgm, parent_cls, cls_set, meth_set, false);
    }
}

// the Python objects of Qore objects: (interpreter, Qore object) -> Python object; the Python objects are borrowed:
// they are removed from the map before they release their Qore objects; guarded by qpy_proxy_lock
typedef std::map<std::pair<PyInterpreterState*, QoreObject*>, PyQoreObject*> proxy_map_t;
static proxy_map_t proxy_map;

// free-threaded Python 3.13 has no API to take a reference to an object whose reference count can reach zero in
// another thread, so the Qore objects have no unique Python objects there
#if defined(Py_GIL_DISABLED) && PY_VERSION_HEX < 0x030E0000
#define QORE_PYTHON_NO_PROXY_MAP 1
#endif

// returns true if the object is a special name (ex: __fspath__), which is a Python protocol, not a Qore member
static bool qore_python_is_special_name(const char* name) {
    size_t len = strlen(name);
    return len > 4 && name[0] == '_' && name[1] == '_' && name[len - 2] == '_' && name[len - 1] == '_';
}

PyObject* PythonQoreClass::findProxy(QoreObject* obj) {
#ifdef QORE_PYTHON_NO_PROXY_MAP
    return nullptr;
#else
    AutoLocker al(qpy_proxy_lock);
    proxy_map_t::iterator i = proxy_map.find(proxy_map_t::key_type(
        qore_python_get_interpreter(PyThreadState_Get()), obj));
    if (i == proxy_map.end()) {
        return nullptr;
    }
    PyObject* rv = reinterpret_cast<PyObject*>(i->second);
#ifdef Py_GIL_DISABLED
    // the object can be being deallocated by another thread, which removes it from the map
    if (!PyUnstable_TryIncRef(rv)) {
        return nullptr;
    }
#else
    // with the GIL, an object being deallocated has been removed from the map before the GIL can be released
    Py_INCREF(rv);
#endif
    return rv;
#endif
}

PyObject* PythonQoreClass::registerProxy(PyQoreObject* self) {
#ifndef QORE_PYTHON_NO_PROXY_MAP
    assert(self->qobj && self->strong && !self->registered);
#ifdef Py_GIL_DISABLED
    PyUnstable_EnableTryIncRef(reinterpret_cast<PyObject*>(self));
#endif
    proxy_map_t::key_type key(qore_python_get_interpreter(PyThreadState_Get()), self->qobj);
    while (true) {
        PyObject* other;
        {
            AutoLocker al(qpy_proxy_lock);
            std::pair<proxy_map_t::iterator, bool> rv = proxy_map.insert(proxy_map_t::value_type(key, self));
            if (rv.second) {
                self->registered = true;
                break;
            }
            // another thread registered the Python object of the Qore object meanwhile
            other = reinterpret_cast<PyObject*>(rv.first->second);
#ifdef Py_GIL_DISABLED
            if (!PyUnstable_TryIncRef(other)) {
                // the other object is being deallocated and is about to be removed from the map
                continue;
            }
#else
            Py_INCREF(other);
#endif
        }
        // releasing the object can run Qore code: it must be released without the lock
        Py_DECREF(reinterpret_cast<PyObject*>(self));
        return other;
    }
#endif
    return reinterpret_cast<PyObject*>(self);
}

void PythonQoreClass::deregisterProxy(PyQoreObject* self) {
    if (!self->registered) {
        return;
    }
    self->registered = false;
    AutoLocker al(qpy_proxy_lock);
    proxy_map_t::iterator i = proxy_map.find(proxy_map_t::key_type(
        qore_python_get_interpreter(PyThreadState_Get()), self->qobj));
    if (i != proxy_map.end() && i->second == self) {
        proxy_map.erase(i);
    }
}

PyObject* PythonQoreClass::wrap(QoreObject* obj) {
    PyQoreObject* self = (PyQoreObject*)py_type->tp_alloc(py_type, 0);
    if (!self) {
        return nullptr;
    }
    // the Python object holds a strong reference: the Qore object exists as long as Qore or Python references it, and
    // its destructor runs when the last reference is released
    obj->ref();
    self->qobj = obj;
    self->strong = true;
    //printd(5, "PythonQoreClass::wrap() obj: %p (%s)\n", obj, obj->getClassName());
    return registerProxy(self);
}

PyObject* PythonQoreClass::exec_qore_method(PyObject* method_capsule, PyObject* args) {
    // Save the thread state Python had when calling us - must restore before returning
    PyThreadState* entry_tstate = qore_python_attached_thread_state();

    // get method
    method_capsule_t* data = get_capsule_method(method_capsule);
    if (!data) {
        return nullptr;
    }
    // the method's class is not freed while it is called
    QorePythonDeclUse use(data->handle.get());
    if (!use) {
        data->handle->raiseDeleted();
        return nullptr;
    }
    const QoreMethod* m = &data->method;
    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    assert(PyTuple_Check(args));
    QoreObject* obj;
    // check if this could be a static method call
    if (!PyTuple_Size(args)) {
        obj = nullptr;
    } else {
        // returns a borrowed reference
        PyObject* py_obj = PyTuple_GetItem(args, 0);
        if (!PyQoreObject_Check(py_obj)) {
            obj = nullptr;
        } else {
            obj = reinterpret_cast<PyQoreObject*>(py_obj)->qobj;
            // if the class is not accessible, do not make the call
            if (obj && obj->getClassAccess(*m->getClass()) > Private) {
                obj = nullptr;
            }
        }
    }

#if 0
    {
        QorePythonReferenceHolder argstr(PyObject_Repr(args));
        printd(5, "PythonQoreClass::exec_qore_method() %s::%s() obj: %p (%s) args: %s\n", m->getClassName(),
            m->getName(), obj, obj ? obj->getClassName() : "n/a", PyUnicode_AsUTF8(*argstr));
    }
#endif
    if (!obj) {
        // see if a static method with the same name is available
        ClassAccess access;
        const QoreMethod* static_meth = m->getClass()->findStaticMethod(m->getName(), access);
        if (!static_meth || access > Private) {
            QoreStringMaker desc("cannot call normal method '%s::%s()' without a 'self' object argument that " \
                "inherits '%s'", m->getClassName(), m->getName(), m->getClassName());
            PyErr_SetString(PyExc_ValueError, desc.c_str());
            return nullptr;
        }

        //printd(5, "about to call exec_qore_static_method()\n");
        return exec_qore_static_method(*static_meth, args, 0);
    }

    ExceptionSink xsink;
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    {
        // issue #4330: get Qore program from method if possible
        QoreProgram* pgm = m->getClass()->getProgram();
        if (!pgm) {
            pgm = qore_python_pgm->getQoreProgram();
        }

        QorePythonHelper qph(qore_python_pgm, &xsink);
        if (!xsink) {
            // the arguments and the result of the call are released after the context of the method's Program is
            // left: releasing them can run Qore code that deletes the Program
            ReferenceHolder<QoreListNode> qargs(&xsink);
            ValueHolder rv(&xsink);
            QorePythonReferenceHolder py_rv;
            {
                QoreExternalProgramContextHelper pch(&xsink, pgm);
                if (!xsink) {
                    qargs = qore_python_pgm->getQoreListFromTuple(&xsink, args, 1);
                    if (!xsink) {
                        {
                            QorePythonReleaseGilHelper prgh;

                            QorePythonStackLocationHelper slh(qph.getOldProgram());

                            rv = obj->evalMethod(*m, *qargs, &xsink);
                        }
                        // the method is not used anymore: converting and releasing the result can run Qore code that
                        // deletes the method's Program
                        use.end();
                        if (!xsink) {
                            py_rv = qore_python_pgm->getPythonValue(*rv, &xsink);
                        }
                    }
                }
            }
            // released without the GIL if this can run Qore code
            qore_python_release(rv);
            qore_python_release(qargs);
            if (!xsink) {
                assert(py_rv);
                return py_rv.release();
            }
        }
    }

    // issue #4329: exception must be thrown in the calling context
    // IMPORTANT: Restore the entry thread state before setting the exception.
    // QorePythonHelper may have changed thread states, but Python expects the
    // exception to be on the thread state it passed when calling us.
    PyThreadState* exit_tstate = qore_python_attached_thread_state();
    if (exit_tstate != entry_tstate) {
        PyThreadState_Swap(entry_tstate);
    }

    if (xsink) {
        qore_python_pgm->raisePythonException(xsink);
        return nullptr;
    }
    // Fallback - should not reach here normally
    return nullptr;
}

PyObject* PythonQoreClass::exec_qore_static_method(PyObject* method_capsule, PyObject* args) {
    printd(5, "exec_qore_static_method() args: %p\n", args);
    // get method
    method_capsule_t* data = get_capsule_method(method_capsule);
    if (!data) {
        return nullptr;
    }
    // the method's class is not freed while it is called
    QorePythonDeclUse use(data->handle.get());
    if (!use) {
        data->handle->raiseDeleted();
        return nullptr;
    }
    const QoreMethod* m = &data->method;
    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    assert(PyTuple_Check(args));
#if 0
    {
        QorePythonReferenceHolder argstr(PyObject_Repr(args));
        assert(PyUnicode_Check(*argstr));
        printd(1, "PythonQoreClass::exec_qore_static_method() %s::%s() args: %s\n", m->getClassName(), m->getName(),
            PyUnicode_AsUTF8(*argstr));
    }
#endif

    return exec_qore_static_method(*m, args);
}

PyObject* PythonQoreClass::exec_qore_static_method(const QoreMethod& m, PyObject* args, size_t offset) {
    // Save the thread state Python had when calling us - must restore before returning
    PyThreadState* entry_tstate = qore_python_attached_thread_state();

    ExceptionSink xsink;
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getContext();
    {
        // issue #4330: get Qore program from method if possible
        QoreProgram* pgm = m.getClass()->getProgram();
        if (!pgm) {
            pgm = qore_python_pgm->getQoreProgram();
        }

        QorePythonHelper qph(qore_python_pgm, &xsink);
        if (!xsink) {
            // the arguments and the result of the call are released after the context of the method's Program is
            // left: releasing them can run Qore code that deletes the Program
            ReferenceHolder<QoreListNode> qargs(&xsink);
            ValueHolder rv(&xsink);
            QorePythonReferenceHolder py_rv;
            {
                QoreExternalProgramContextHelper pch(&xsink, pgm);
                if (!xsink) {
                    qargs = qore_python_pgm->getQoreListFromTuple(&xsink, args, offset);
                    if (!xsink) {
                        {
                            QorePythonReleaseGilHelper prgh;

                            QorePythonStackLocationHelper slh(qph.getOldProgram());

                            rv = QoreObject::evalStaticMethod(m, m.getClass(), *qargs, &xsink);
                        }
                        if (!xsink) {
                            py_rv = qore_python_pgm->getPythonValue(*rv, &xsink);
                        }
                    }
                }
            }
            // released without the GIL if this can run Qore code
            qore_python_release(rv);
            qore_python_release(qargs);
            if (!xsink) {
                assert(py_rv);
                return py_rv.release();
            }
        }
    }

    // issue #4329: exception must be thrown in the calling context
    // IMPORTANT: Restore the entry thread state before setting the exception.
    // QorePythonHelper may have changed thread states, but Python expects the
    // exception to be on the thread state it passed when calling us.
    PyThreadState* exit_tstate = qore_python_attached_thread_state();
    if (exit_tstate != entry_tstate) {
        PyThreadState_Swap(entry_tstate);
    }

    if (xsink) {
        qore_python_pgm->raisePythonException(xsink);
        return nullptr;
    }
    // Fallback - should not reach here normally
    return nullptr;
}

int PythonQoreClass::py_init(PyObject* self, PyObject* args, PyObject* kwds) {
    assert(PyQoreObject_Check(self));
    assert(PyTuple_Check(args));
    //QorePythonReferenceHolder argstr(PyObject_Repr(args));
    //printd(5, "PythonQoreClass::py_init() self: %p '%s' args: %p (%d: %s) kwds: %p\n", self, Py_TYPE(self)->tp_name,
    //  args, (int)PyTuple_Size(args), PyUnicode_AsUTF8(*argstr), kwds);

    // Qore constructors take positional arguments; keyword arguments are not ignored
    if (kwds && qore_python_check_no_kwargs(kwds, QoreStringMaker("%s()", Py_TYPE(self)->tp_name).c_str())) {
        return -1;
    }

    PyQoreObject* pyself = reinterpret_cast<PyQoreObject*>(self);
    // the Qore object of a Python object is created once: calling __init__() again would create another one
    if (pyself->qobj) {
        PyErr_Format(PyExc_RuntimeError, "the '%s' object has already been initialized", Py_TYPE(self)->tp_name);
        return -1;
    }

    // the Python object of a Qore object whose class inherits a Python subclass of this Qore class (an object of the
    // generated subclass, see QorePythonProgram::getQoreSubclassType()): it is the Qore object, whose Qore base class
    // Qore has already constructed with the object; no other Qore object is created
    {
        QoreObject* sub_obj = QorePythonProgram::getQoreSubclassObject(self);
        if (sub_obj) {
            if (PyTuple_GET_SIZE(args)) {
                PyErr_Format(PyExc_TypeError, "the Qore base class of '%s' is constructed by Qore with the object of "
                    "the Qore subclass: no arguments can be given to its __init__() (got %d)", Py_TYPE(self)->tp_name,
                    (int)PyTuple_GET_SIZE(args));
                return -1;
            }
            sub_obj->tRef();
            pyself->qobj = sub_obj;
            return 0;
        }
    }

    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return -1;
    }
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getExecutionContext();

    ExceptionSink xsink;

    QoreExternalProgramContextHelper pch(&xsink, qore_python_pgm->getQoreProgram());
    if (xsink) {
        qore_python_pgm->raisePythonException(xsink);
        return -1;
    }

    // the current Qore class
    const QoreClass* qcls;
    // the Qore class to be constructed
    const QoreClass* constructor_cls;

    PyTypeObject* type = Py_TYPE(self);
    // the Qore class is not freed while the object is constructed
    QorePythonDeclHandle* cls_handle = PyQoreObjectType_Check(type) ? getQoreClass(type) : findQoreClass(self);
    if (!cls_handle) {
        // a Python exception is set
        return -1;
    }
    QorePythonDeclUse use(cls_handle);
    if (!use) {
        cls_handle->raiseDeleted();
        return -1;
    }
    if (!PyQoreObjectType_Check(type)) {
        constructor_cls = static_cast<const QoreClass*>(cls_handle->getDecl());
        assert(type->tp_base);
        // create Qore type for Python type
        qcls = qore_python_pgm->getCreateQorePythonClass(&xsink, type);
        if (!qcls) {
            qore_python_pgm->raisePythonException(xsink);
            return -1;
        }
        printd(5, "PythonQoreClass::py_init() self: %p type: %s got context pypgm: %p\n", self, type->tp_name,
            qore_python_pgm);
    } else {
        constructor_cls = qcls = static_cast<const QoreClass*>(cls_handle->getDecl());
    }

    // returns a borrowed reference
    QoreObject* qobj = QorePythonImplicitQoreArgHelper::getQoreObject();
    //printd(5, "PythonQoreClass::py_init() self: %p py_cls: '%s' qcls: '%s' (%d) cq: '%s' (%d) qobj: %p args: %p\n",
    //  self, type->tp_name, qcls->getName(), qcls->getID(), constructor_cls->getName(), constructor_cls->getID(),
    //  qobj, args);
    if (qobj && qobj->getClass() == qcls) {
        // the Qore object was created by Qore, which manages its lifetime; this object is its Python part
        qobj->tRef();
        pyself->qobj = qobj;
        return 0;
    }

    // issue #4044: do not allow abstract classes to be constructed
    if (!qcls->runtimeCheckInstantiateClass(&xsink)) {
        ReferenceHolder<QoreListNode> qargs(qore_python_pgm->getQoreListFromTuple(&xsink, args, 0, true), &xsink);
        if (!xsink) {
            QoreExternalProgramContextHelper pch(&xsink, qore_python_pgm->getQoreProgram());
            if (!xsink) {
                QoreObject* created_obj = nullptr;
                {
                    QorePythonReleaseGilHelper prgh;

                    QorePythonStackLocationHelper slh(qore_python_pgm);

                    ReferenceHolder<QoreObject> qobj(constructor_cls->execConstructor(*qcls, *qargs, true, &xsink),
                        &xsink);
                    if (!xsink) {
                        printd(5, "PythonQoreClass::py_init() self: %p created Qore %s object (args: %p %d): %p (%s)\n",
                            self, qcls->getName(), *qargs, qargs ? (int)qargs->size() : 0, *qobj, qobj->getClassName());
                        created_obj = qobj.release();
                    }
                    // the arguments are released without the GIL, as this can run Qore code
                    qargs = nullptr;
                }
                // GIL is now held again, safe to call newQoreObject which calls Py_INCREF
                if (created_obj) {
                    return newQoreObject(xsink, pyself, created_obj, qcls == constructor_cls ? nullptr : qcls,
                        qore_python_pgm);
                }
            }
        }
    }

    qore_python_pgm->raisePythonException(xsink);
    assert(PyErr_Occurred());
    return -1;
}

int PythonQoreClass::newQoreObject(ExceptionSink& xsink, PyQoreObject* pyself, QoreObject* qobj,
        const QoreClass* qcls, QorePythonProgram* qore_python_pgm) {
    // another thread initialized the object while the constructor ran without the GIL: the new Qore object is
    // released (without the GIL, as this can run its destructor)
    if (pyself->qobj) {
        {
            QorePythonReleaseGilHelper prgh;
            qobj->deref(&xsink);
        }
        if (xsink) {
            qore_python_pgm->raisePythonException(xsink);
        } else {
            PyErr_Format(PyExc_RuntimeError, "the '%s' object has already been initialized", Py_TYPE(pyself)->tp_name);
        }
        return -1;
    }
    // the Python object takes over the reference returned by the constructor: the Qore object exists as long as Qore
    // or Python references it
    pyself->qobj = qobj;
    pyself->strong = true;

    if (!qcls) {
        // the Python object of the new Qore object; there can be no other one
        PyObject* rv = registerProxy(pyself);
        assert(rv == reinterpret_cast<PyObject*>(pyself));
        (void)rv;
    } else {
        // add private data for python class; the reference cycle between the two objects is visible to Python's cycle
        // collector while the Python object holds the only reference to the Qore object (see py_traverse())
        Py_INCREF(pyself);
        qobj->setPrivate(qcls->getID(), new QorePythonPrivateData(qore_python_pgm, (PyObject*)pyself));
        pyself->qobj_refs_self = true;
    }
    // call the save object callback, if any
    qore_python_pgm->saveQoreObjectFromPython(qobj, xsink);
    if (!xsink) {
        return 0;
    }

    qore_python_pgm->raisePythonException(xsink);
    assert(PyErr_Occurred());
    return -1;
}

// raises AttributeError for the Qore exception for a member that cannot be accessed (INVALID-MEMBER,
// PRIVATE-MEMBER), and returns true, or returns false for other exceptions
static bool qore_python_member_attribute_error(ExceptionSink& xsink) {
    QoreValue err = xsink.getExceptionErr();
    if (err.getType() != NT_STRING) {
        return false;
    }
    const char* errstr = err.get<const QoreStringNode>()->c_str();
    if (strcmp(errstr, "INVALID-MEMBER") && strcmp(errstr, "PRIVATE-MEMBER")) {
        return false;
    }
    QoreStringValueHelper desc(xsink.getExceptionDesc());
    PyErr_SetString(PyExc_AttributeError, desc->c_str());
    xsink.clear();
    return true;
}

// returns true if the name is a data descriptor of the Python type (ex: a property of a Python subclass), which
// handles setting and deleting the attribute
static bool qore_python_has_data_descriptor(PyTypeObject* type, PyObject* attr) {
    PyObject* mro = type->tp_mro;
    Py_ssize_t size = mro && PyTuple_Check(mro) ? PyTuple_GET_SIZE(mro) : 0;
    for (Py_ssize_t i = 0; i < size; ++i) {
        PyTypeObject* t = reinterpret_cast<PyTypeObject*>(PyTuple_GET_ITEM(mro, i));
        // returns a borrowed reference
        PyObject* v = t->tp_dict ? PyDict_GetItem(t->tp_dict, attr) : nullptr;
        if (v) {
            return Py_TYPE(v)->tp_descr_set != nullptr;
        }
    }
    return false;
}

// returns true if the Qore object has the member with a value, as Qore's exists operator: a member whose value was
// removed (QoreObject::removeMember() leaves NOTHING) does not exist; QoreObject::hasMember() raises INVALID-MEMBER
// for a member that is not declared in a class with declared members, which the object does not have; called without
// the GIL, as releasing the value can run Qore code
static bool qore_python_has_member(QoreObject* obj, const char* member, ExceptionSink& xsink) {
    bool rv = obj->hasMember(member, &xsink);
    if (xsink) {
        QoreValue err = xsink.getExceptionErr();
        if (err.getType() == NT_STRING && !strcmp(err.get<const QoreStringNode>()->c_str(), "INVALID-MEMBER")) {
            xsink.clear();
        }
        return false;
    }
    if (!rv) {
        return false;
    }
    ValueHolder v(obj->evalMember(member, &xsink), &xsink);
    return !xsink && !v->isNothing();
}

// raises AttributeError for a missing attribute as Python does: "'<type name>' object has no attribute '<name>'"
static void qore_python_no_attribute(PyObject* self, const char* member, const char* extra = "") {
    QorePythonReferenceHolder name(PyType_GetName(Py_TYPE(self)));
    if (!name) {
        return;
    }
    PyErr_Format(PyExc_AttributeError, "'%U' object has no attribute '%s'%s", *name, member, extra);
}

PyObject* PythonQoreClass::py_getattro(PyObject* self, PyObject* attr) {
    // first try to get python attribute
    PyObject* pyrv = PyObject_GenericGetAttr(self, attr);
    if (pyrv || !PyErr_ExceptionMatches(PyExc_AttributeError)) {
        return pyrv;
    }

    assert(PyQoreObject_Check(self));
    assert(PyUnicode_Check(attr));
    const char* member = PyUnicode_AsUTF8(attr);
    if (!member) {
        return nullptr;
    }
    // special names (ex: __fspath__, __array__) are Python protocols, not Qore members: the AttributeError is raised
    if (qore_python_is_special_name(member)) {
        return nullptr;
    }
    QoreObject* obj = reinterpret_cast<PyQoreObject*>(self)->qobj;
    if (!obj) {
        // ex: a Python subclass whose __init__() does not call the Qore constructor
        qore_python_no_attribute(self, member, " (the object has no Qore object, as the constructor of the Qore "
            "class was not called)");
        return nullptr;
    }
    PyErr_Clear();

    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    // the Qore class is not freed while the member is read
    QorePythonDeclHandle* cls_handle = findQoreClass(self);
    if (!cls_handle) {
        return nullptr;
    }
    QorePythonDeclUse use(cls_handle);
    if (!use) {
        cls_handle->raiseDeleted();
        return nullptr;
    }

    ExceptionSink xsink;
    //printd(5, "PythonQoreClass::py_getattro() obj %p %s.%s\n", obj, cls_handle->getName().c_str(), member);
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getExecutionContext();
    QoreExternalProgramContextHelper pch(&xsink, qore_python_pgm->getQoreProgram());
    if (!xsink) {
        ValueHolder v(&xsink);
        bool exists = true;
        {
            QorePythonReleaseGilHelper prgh;

            QorePythonStackLocationHelper slh(qore_python_pgm);

            // a member that the object does not have is an AttributeError, unless its class has a memberGate()
            // method, which gives the values of all members
            const QoreClass* ocls = obj->getClass();
            bool priv;
            if (!ocls->hasMemberGate() && !ocls->isPublicOrPrivateMember(member, priv)
                && !qore_python_has_member(obj, member, xsink)) {
                exists = false;
            } else if (!xsink) {
                v = obj->evalMember(member, &xsink);
            }
        }
        // the class is not used anymore: converting and releasing the value can run Qore code
        use.end();
        if (!exists && !xsink) {
            qore_python_no_attribute(self, member);
            return nullptr;
        }
        QorePythonReferenceHolder rv;
        if (!xsink) {
            rv = qore_python_pgm->getPythonValue(*v, &xsink);
        }
        // the value is released without the GIL if this can run Qore code
        qore_python_release(v);
        if (!xsink) {
            return rv.release();
        }
    }
    if (qore_python_member_attribute_error(xsink)) {
        return nullptr;
    }
    qore_python_pgm->raisePythonException(xsink);
    assert(PyErr_Occurred());
    return nullptr;
}

int PythonQoreClass::py_setattro(PyObject* self, PyObject* attr, PyObject* value) {
    assert(PyQoreObject_Check(self));
    if (!PyUnicode_Check(attr)) {
        return PyObject_GenericSetAttr(self, attr, value);
    }
    const char* member = PyUnicode_AsUTF8(attr);
    if (!member) {
        return -1;
    }
    QoreObject* obj = reinterpret_cast<PyQoreObject*>(self)->qobj;
    // Python attributes: special names, attributes of objects without a Qore object, and data descriptors of the
    // Python type (ex: a property of a Python subclass)
    if (!obj || qore_python_is_special_name(member) || qore_python_has_data_descriptor(Py_TYPE(self), attr)) {
        return PyObject_GenericSetAttr(self, attr, value);
    }

    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return -1;
    }
    // the Qore class is not freed while the member is written or removed; a class whose Program has been deleted
    // cannot be used, as for reads
    QorePythonDeclHandle* cls_handle = findQoreClass(self);
    if (!cls_handle) {
        return -1;
    }
    QorePythonDeclUse use(cls_handle);
    if (!use) {
        cls_handle->raiseDeleted();
        return -1;
    }

    ExceptionSink xsink;
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getExecutionContext();
    QoreExternalProgramContextHelper pch(&xsink, qore_python_pgm->getQoreProgram());
    if (!xsink) {
        // the attribute is a Qore member if the class declares it or the object has it; otherwise an object of a
        // Python subclass (which has a dict) gets a Python attribute
        const QoreClass* ocls = obj->getClass();
        bool priv;
        bool declared = ocls->isPublicOrPrivateMember(member, priv);
        bool has_member;
        {
            QorePythonReleaseGilHelper prgh;
            has_member = declared || qore_python_has_member(obj, member, xsink);
        }
        if (xsink) {
            // falls through to raise the exception
        } else if (!has_member && Py_TYPE(self)->tp_dictoffset) {
            return PyObject_GenericSetAttr(self, attr, value);
        } else if (!value && !has_member) {
            qore_python_no_attribute(self, member);
            return -1;
        } else if (declared && priv) {
            PyErr_Format(PyExc_AttributeError, "'%s' is a private member of Qore class '%s'", member,
                ocls->getName());
            return -1;
        } else if (!has_member && ocls->hasPublicMembersInHierarchy() && !ocls->hasMemberWriteGate()) {
            // a class with declared members has no other members, unless a member write gate handles the writes (ex:
            // the Qore class of a Python class, whose writes are Python attribute writes)
            PyErr_Format(PyExc_AttributeError, "'%s' is not a registered member of Qore class '%s'", member,
                ocls->getName());
            return -1;
        } else {
            ValueHolder v(&xsink);
            if (value) {
                v = qore_python_pgm->getQoreValue(&xsink, value);
            }
            if (!xsink) {
                // assigning the member or removing it releases the old value, which can run Qore code
                QorePythonReleaseGilHelper prgh;
                QorePythonStackLocationHelper slh(qore_python_pgm);
                if (value) {
                    // assigned in the context of the object's class (its private members are refused above): the
                    // value is checked and converted for the type of the member, as by an assignment in Qore code
                    obj->setMemberValue(member, ocls, v.release(), &xsink);
                } else {
                    obj->removeMember(member, &xsink);
                }
            } else {
                qore_python_release(v);
            }
            use.end();
            if (!xsink) {
                return 0;
            }
        }
    }
    if (qore_python_member_attribute_error(xsink)) {
        return -1;
    }
    qore_python_pgm->raisePythonException(xsink);
    assert(PyErr_Occurred());
    return -1;
}

PyObject* PythonQoreClass::py_dir(PyObject* /* unused */, PyObject* self) {
    // the Python attributes
    QorePythonReferenceHolder base_dir(PyObject_CallMethod(reinterpret_cast<PyObject*>(&PyBaseObject_Type),
        "__dir__", "O", self));
    if (!base_dir) {
        return nullptr;
    }
    QorePythonReferenceHolder rv(PySequence_List(*base_dir));
    if (!rv) {
        return nullptr;
    }
    QoreObject* obj = PyQoreObject_Check(self) ? reinterpret_cast<PyQoreObject*>(self)->qobj : nullptr;
    if (!obj) {
        return rv.release();
    }

    QorePythonQoreEntryHelper qeh;
    if (!qeh.check()) {
        return nullptr;
    }
    // the public members of the Qore object
    ExceptionSink xsink;
    ReferenceHolder<QoreListNode> members(&xsink);
    {
        QorePythonReleaseGilHelper prgh;
        members = obj->getMemberList(&xsink);
    }
    QorePythonProgram* qore_python_pgm = QorePythonProgram::getExecutionContext();
    if (xsink) {
        qore_python_pgm->raisePythonException(xsink);
        return nullptr;
    }
    const QoreClass* ocls = obj->getClass();
    ConstListIterator i(*members);
    while (i.next()) {
        QoreStringValueHelper name(i.getValue(), QCS_UTF8, &xsink);
        bool priv;
        if (xsink || (ocls->isPublicOrPrivateMember(name->c_str(), priv) && priv)) {
            xsink.clear();
            continue;
        }
        QorePythonReferenceHolder str(PyUnicode_FromStringAndSize(name->c_str(), name->size()));
        if (!str || PyList_Append(*rv, *str) < 0) {
            return nullptr;
        }
    }
    return rv.release();
}

QorePythonDeclHandle* PythonQoreClass::findQoreClass(PyObject* self) {
    PyTypeObject* type = Py_TYPE(self);
    // get base Qore class
    while (!PyQoreObjectType_Check(type)) {
        if (!type->tp_base) {
            ExceptionSink xsink;
            xsink.raiseException("QORE-ERROR", "cannot initialize Python class '%s' as a derived class of a Qore " \
                "base class; no Qore base class found", Py_TYPE(self)->tp_name);
            qore_python_pgm->raisePythonException(xsink);
            return nullptr;
        }
        type = type->tp_base;
    }
    return getQoreClass(type);
}

QorePythonDeclHandle* PythonQoreClass::getQoreClass(PyTypeObject* type) {
    assert(type->tp_dict);
    // returns a borrowed reference
    PyObject* obj = PyDict_GetItemString(type->tp_dict, QCLASS_KEY);
    QorePythonDeclHandle* handle = QorePythonDeclHandle::getCapsuleHandle(obj);
    if (!handle) {
        PyErr_Format(PyExc_RuntimeError, "the Python type '%s' has no Qore class", type->tp_name);
    }
    return handle;
}

PyObject* PythonQoreClass::py_new(PyTypeObject* type, PyObject* args, PyObject* kw) {
    return type->tp_alloc(type, 0);
}

int PythonQoreClass::py_traverse(PyObject* self, visitproc visit, void* arg) {
    // Heap instances own their type, including cycles through class constants.
    Py_VISIT(Py_TYPE(self));
    // an instance of a Python subclass of a Qore class is referenced by the private data of its Qore object; while it
    // holds the only reference to the Qore object, that reference is part of a cycle that only this object keeps
    // alive, which py_clear() breaks; while Qore code also references the Qore object, it keeps this object alive.
    // Once the Qore object has been deleted (ex: by Qore's delete), its private data no longer references this
    // object, which must then not be visited: the collector would count a reference that does not exist and clear a
    // reachable object
    PyQoreObject* pyself = reinterpret_cast<PyQoreObject*>(self);
    if (pyself->qobj_refs_self && pyself->strong && pyself->qobj && pyself->qobj->isValid()
        && pyself->qobj->reference_count() == 1) {
        Py_VISIT(self);
    }
    return 0;
}

int PythonQoreClass::py_clear(PyObject* self) {
    releaseQoreObject(reinterpret_cast<PyQoreObject*>(self));
    return 0;
}

void PythonQoreClass::releaseQoreObject(PyQoreObject* self) {
    QoreObject* obj = self->qobj;
    if (!obj) {
        return;
    }
    // removed from the map of Python objects before the GIL can be released (see findProxy())
    deregisterProxy(self);
    self->qobj = nullptr;
    // after the Qore library has been shut down, the Qore object is left to the exit of the process: releasing it
    // could run its destructor, which is Qore code (see qore_python_register_thread())
    if (q_libqore_shutdown()) {
        return;
    }
    if (!self->strong) {
        obj->tDeref();
        return;
    }
    // releasing the reference can run the object's destructor, which is Qore code: it needs a thread registered with
    // Qore, and runs without the GIL, as other Qore code called from Python, so that it cannot deadlock with a thread
    // that holds a Qore lock and waits for the GIL; if the thread cannot be registered, the reference is not released
    QorePythonQoreEntryHelper qeh;
    if (!qeh.ok()) {
        return;
    }
    ExceptionSink xsink;
    {
        QorePythonReleaseGilHelper prgh;
        obj->deref(&xsink);
    }
}

void PythonQoreClass::py_dealloc(PyQoreObject* self) {
    PyObject_GC_UnTrack(self);
    PyTypeObject* type = Py_TYPE(self);
    // removed from the map of Python objects first: the callbacks of weak references are Python code, which can
    // release the GIL and convert the Qore object to Python again, which must not find this object
    deregisterProxy(self);
    if (self->weakreflist) {
        PyObject_ClearWeakRefs(reinterpret_cast<PyObject*>(self));
    }
    releaseQoreObject(self);
    type->tp_free(self);
    // PyType_GenericAlloc gives every instance a reference to its heap type.
    // The custom deallocator must release that reference after freeing the instance.
    Py_DECREF(type);
}

PyObject* PythonQoreClass::py_repr(PyObject* obj) {
    QoreStringMaker str("Qore %s object %p", Py_TYPE(obj)->tp_name, obj);
    return PyUnicode_FromStringAndSize(str.c_str(), str.size());
}
