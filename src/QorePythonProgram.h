/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
  QorePythonProgram.h

  Qore Programming Language

  Copyright (C) 2020 - 2022 Qore Technologies, s.r.o.

  Permission is hereby granted, free of charge, to any person obtaining a
  copy of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom the
  Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
  DEALINGS IN THE SOFTWARE.

  Note that the Qore library is released under a choice of three open-source
  licenses: MIT (as above), LGPL 2+, or GPL 2+; see README-LICENSE for more
  information.
*/

#ifndef _QORE_QOREPYTHONPROGRAM

#define _QORE_QOREPYTHONPROGRAM

#include "python-module.h"

#include "QorePythonClass.h"
#include "QorePythonPrivateData.h"

#include <pythonrun.h>

#include <set>
#include <map>
#include <memory>

// forward reference
class QorePythonProgram;
class PythonQoreClass;

#define IF_CLASS (1 << 0)
#define IF_OTHER (1 << 1)
#define IF_ALL   (IF_CLASS | IF_OTHER)

//! best guess at the ratio of stack size / x = python recursion limit to avoid crashes
constexpr int PYTHON_LARGE_STACK_FACTOR = 10 * 1024;
constexpr int PYTHON_SMALL_STACK_FACTOR = 6 * 1024;

struct QorePythonThreadStateInfo {
    PyThreadState* state;
    bool owns_state;
};

class QorePythonProgram : public AbstractQoreProgramExternalData {
    friend class PythonModuleContextHelper;
public:
    typedef std::set<std::string> strset_t;

    //! Python context using the main interpreter
    DLLLOCAL QorePythonProgram();

    //! Default Qore Python context; does not own the QoreProgram reference
    DLLLOCAL QorePythonProgram(QoreProgram* qpgm, QoreNamespace* pyns);

    //! New Qore Python context; does not own the QoreProgram reference
    DLLLOCAL QorePythonProgram(const QorePythonProgram& old, QoreProgram* qpgm)
            : QorePythonProgram(qpgm, qpgm->findNamespace(QORE_PYTHON_NS_NAME)) {
        if (!pyns) {
            pyns = PNS->copy();
            qpgm->getRootNS()->addNamespace(pyns);
        }
    }

    DLLLOCAL QorePythonProgram(const QoreString& source_code, const QoreString& source_label, int start,
        ExceptionSink* xsink);

    DLLLOCAL virtual AbstractQoreProgramExternalData* copy(QoreProgram* pgm) const {
        return new QorePythonProgram(*this, pgm);
    }

    DLLLOCAL virtual void doDeref() {
        printd(5, "QorePythonProgram::doDeref() this: %p\n", this);
        ExceptionSink xsink;
        deleteIntern(&xsink);
        weakDeref();
        if (xsink) {
            throw QoreXSinkException(xsink);
        }
    }

    DLLLOCAL void destructor(ExceptionSink* xsink) {
        deleteIntern(xsink);
    }

    DLLLOCAL void py_destructor(ExceptionSink* xsink) {
        needs_deregistration = false;
        deleteIntern(xsink);
    }

    DLLLOCAL QoreValue run(ExceptionSink* xsink) {
        assert(python_code);
        QorePythonHelper qph(this, xsink);
        if (qph.wasInterrupted() || checkValid(xsink)) {
            return QoreValue();
        }
        assert(module_dict);
        QorePythonReferenceHolder return_value(PyEval_EvalCode(*python_code, module_dict, module_dict));

        // check for Python exceptions
        if (checkPythonException(xsink)) {
            return QoreValue();
        }

        return getQoreValue(xsink, return_value);
    }

    //! Evaluates the statement and returns any result
    DLLLOCAL QoreValue eval(ExceptionSink* xsink, const QoreString& source_code, const QoreString& source_label,
            int input, bool encapsulate);

    //! Call the function and return the result
    DLLLOCAL QoreValue callFunction(ExceptionSink* xsink, const QoreString& func_name, const QoreListNode* args,
        size_t arg_offset = 0);

    //! Call a method and return the result
    /** converts the string arguments to UTF-8 and makes the call
    */
    DLLLOCAL QoreValue callMethod(ExceptionSink* xsink, const QoreString& class_name, const QoreString& method_name,
        const QoreListNode* args, size_t arg_offset = 0);

    //! Call a method and return the result
    /** string args are assumed to be in UTF-8 encoding
    */
    DLLLOCAL QoreValue callMethod(ExceptionSink* xsink, const char* cname, const char* mname,
        const QoreListNode* args, size_t arg_offset = 0, PyObject* first = nullptr);

    //! Call a callable and and return the result
    DLLLOCAL QoreValue callInternal(ExceptionSink* xsink, PyObject* callable, const QoreListNode* args,
        size_t arg_offset = 0, PyObject* first = nullptr);

    //! Call a callable and and return the result as a Python value
    DLLLOCAL PyObject* callPythonInternal(ExceptionSink* xsink, PyObject* callable, const QoreListNode* args,
        size_t arg_offset = 0, PyObject* first = nullptr, PyObject* kwargs = nullptr);

    //! Call a PyFunctionObject and and return the result
    DLLLOCAL QoreValue callFunctionObject(ExceptionSink* xsink, PyObject* func, const QoreListNode* args,
        size_t arg_offset = 0, PyObject* first = nullptr);

    //! Sets the "save object callback" for %Qore objects created in Python code
    DLLLOCAL void setSaveObjectCallback(const ResolvedCallReferenceNode* save_object_callback) {
        //printd(5, "QorePythonProgram::setSaveObjectCallback() this: %p old: %p new: %p\n", this,
        //  *this->save_object_callback, save_object_callback);
        this->save_object_callback = save_object_callback ? save_object_callback->refRefSelf() : nullptr;
    }

    //! Returns the "save object callback" for %Qore objects created in Python code
    DLLLOCAL ResolvedCallReferenceNode* getSaveObjectCallback() const {
        return *save_object_callback;
    }

    //! Checks for a Python exception and creates a Qore exception from it
    DLLLOCAL int checkPythonException(ExceptionSink* xsink);

    //! Clears any Python
    DLLLOCAL void clearPythonException();

    //! Calls a Python method and returns the result as a %Qore value
    DLLLOCAL QoreValue callPythonMethod(ExceptionSink* xsink, PyObject* attr, PyObject* obj, const QoreListNode* args,
            size_t arg_offset = 0);

    //! Import Python code into the Qore program object
    DLLLOCAL int import(ExceptionSink* xsink, const char* module, const char* symbol = nullptr);

    //! Returns the attribute of the given object as a Qore value
    /** must already have the Python thread context set
    */
    DLLLOCAL QoreValue getQoreAttr(PyObject* obj, const char* attr, ExceptionSink* xsink);

    //! Returns a Qore value for the given Python value; does not dereference val
    /** must already have the Python thread context set
    */
    DLLLOCAL QoreValue getQoreValue(ExceptionSink* xsink, PyObject* val);

    //! Returns a Qore value for the given Python value
    /** must already have the Python thread context set
    */
    DLLLOCAL QoreValue getQoreValue(ExceptionSink* xsink, QorePythonReferenceHolder& val);

    //! Returns a Qore list from a Python list
    /** must already have the Python thread context set
    */
    DLLLOCAL QoreListNode* getQoreListFromList(ExceptionSink* xsink, PyObject* val);

    //! Returns a Qore list from a Python tuple
    /** must already have the Python thread context set
    */
    DLLLOCAL QoreListNode* getQoreListFromTuple(ExceptionSink* xsink, PyObject* val, size_t offset = 0,
            bool for_args = false);

    //! Returns a Qore hash from a Python dict
    /** must already have the Python thread context set
    */
    DLLLOCAL QoreHashNode* getQoreHashFromDict(ExceptionSink* xsink, PyObject* val);

    //! Set Python thread context
    /** @param check_interrupt if true, check for interrupt before acquiring the GIL
    */
    DLLLOCAL QorePythonThreadInfo setContext(bool check_interrupt = false) const;

    //! Release Python thread context
    DLLLOCAL void releaseContext(const QorePythonThreadInfo& oldstate) const;

    DLLLOCAL void addObj(PyObject* obj) {
        obj_sink.push_back(obj);
    }

    //! Checks if the program is valid
    DLLLOCAL int checkValid(ExceptionSink* xsink) const {
        // the GIL must be held when this function is called
        if (!valid) {
            xsink->raiseException("PYTHON-ERROR", "the given PythonProgram object is invalid or has already been " \
                "deleted");
            return -1;
        }
        assert(PyGILState_Check());
        return 0;
    }

    //! Returns true if the program is valid (does not require GIL)
    DLLLOCAL bool isValid() const {
        return valid;
    }

    //! Returns the Qore program
    DLLLOCAL QoreProgram* getQoreProgram() const {
        return qpgm;
    }

    //! Saves a unique string
    DLLLOCAL const char* saveString(const char* str) {
        std::string sstr = str;
        strset_t::iterator i = strset.lower_bound(sstr);
        if (i != strset.end() && *i == sstr) {
            return (*i).c_str();
        }
        i = strset.insert(i, sstr);
        return (*i).c_str();
    }

    //! Saves Qore objects in thread-local data or using a callback
    DLLLOCAL int saveQoreObjectFromPython(const QoreValue& rv, ExceptionSink& xsink);

    //! Imports the given Qore namespace to Python under the given module path
    DLLLOCAL void importQoreNamespaceToPython(const QoreNamespace& ns, const QoreString& py_mod_path,
            ExceptionSink* xsink);

    //! returns a registered PythonQoreClass for the given Qore class
    DLLLOCAL PythonQoreClass* findCreatePythonClass(const QoreClass& cls, const char* mod_name);

    //! Imports a Qore namespace into a Python module
    DLLLOCAL void importQoreToPython(PyObject* mod, const QoreNamespace& ns, const char* mod_name);

    //! Imports a Qore constant into a Python module
    DLLLOCAL int importQoreConstantToPython(PyObject* mod, const QoreExternalConstant& constant);

    //! imports a Qore function into a Python module
    DLLLOCAL int importQoreFunctionToPython(PyObject* mod, const QoreExternalFunction& func);

    //! Imports a Qore class into a Python module
    DLLLOCAL int importQoreClassToPython(PyObject* mod, const QoreClass& cls, const char* mod_name);

    //! Creates an alias for an existing definition
    DLLLOCAL void aliasDefinition(const QoreString& source_path, const QoreString& target_path);

    //! export a Python class and create a QoreClass for it
    DLLLOCAL void exportClass(ExceptionSink* xsink, QoreString& arg);

    //! export a Python function and create a Qore function for it
    DLLLOCAL void exportFunction(ExceptionSink* xsink, QoreString& arg);

    //! add a path to the Python module search path
    DLLLOCAL void addModulePath(ExceptionSink* xsink, QoreString& arg);

    //! Raise a Python exception from a Qore exception; consumes the Qore exception
    DLLLOCAL void raisePythonException(ExceptionSink& xsink);

    //! Returns a Python list for the given Qore list
    DLLLOCAL PyObject* getPythonList(ExceptionSink* xsink, const QoreListNode* l);

    //! Returns a Python tuple for the given Qore list
    DLLLOCAL PyObject* getPythonTupleValue(ExceptionSink* xsink, const QoreListNode* l, size_t arg_offset = 0,
            PyObject* first = nullptr);

    //! Returns a Python dict for the given Qore hash
    DLLLOCAL PyObject* getPythonDict(ExceptionSink* xsink, const QoreHashNode* h);

    //! Populates the QoreClass based on the Python class
    DLLLOCAL QorePythonClass* setupQorePythonClass(ExceptionSink* xsink, QoreNamespace* ns, PyTypeObject* type,
            std::unique_ptr<QorePythonClass>& cls, strset_t& nsset, int flags = 0);

    //! Creates ot retrieves a QoreClass for the given Python type
    DLLLOCAL QoreClass* getCreateQorePythonClass(ExceptionSink* xsink, PyTypeObject* type, int flags = 0);

    //! Increment the weak ref count
    DLLLOCAL void weakRef() {
        weak_refs.ROreference();
    }

    //! Decrement the weak ref count
    DLLLOCAL void weakDeref() {
        if (weak_refs.ROdereference()) {
            delete this;
        }
    }

    //! Sets the Python recursion limit according to the current default thread stack size
    DLLLOCAL int setRecursionLimit(ExceptionSink* xsink);

    //! Returns the estimated recursion limit based on the current stack pointer in the current thread's stack
    DLLLOCAL static int getRecursionLimit();

    //! Returns a Qore binary from a Python Bytes object
    DLLLOCAL static BinaryNode* getQoreBinaryFromBytes(PyObject* val);

    //! Returns a Qore binary from a Python ByteArray object
    DLLLOCAL static BinaryNode* getQoreBinaryFromByteArray(PyObject* val);

    //! Returns a Qore relative date time value from a Python Delta object
    DLLLOCAL static DateTimeNode* getQoreDateTimeFromDelta(PyObject* val);

    //! Returns a Qore absolute date time value from a Python DateTime object
    DLLLOCAL static DateTimeNode* getQoreDateTimeFromDateTime(PyObject* val);

    //! Returns a Qore absolute date time value from a Python Date object
    DLLLOCAL static DateTimeNode* getQoreDateTimeFromDate(PyObject* val);

    //! Returns a Qore absolute date time value from a Python Time object
    DLLLOCAL static DateTimeNode* getQoreDateTimeFromTime(PyObject* val);

    //! Returns a Python string for the given Qore string
    DLLLOCAL static PyObject* getPythonString(ExceptionSink* xsink, const QoreString* str);

    //! Returns a Python string for the given Qore string
    DLLLOCAL static PyObject* getPythonByteArray(ExceptionSink* xsink, const BinaryNode* b);

    //! Returns a Python delta for the given Qore relative date/time value
    DLLLOCAL static PyObject* getPythonDelta(ExceptionSink* xsink, const DateTime* dt);

    //! Returns a Python string for the given Qore absolute date/time value
    DLLLOCAL static PyObject* getPythonDateTime(ExceptionSink* xsink, const DateTime* dt);

    //! Returns a Python callable object for the given Qore closure / call reference
    DLLLOCAL static PyObject* getPythonCallable(ExceptionSink* xsink, const ResolvedCallReferenceNode* call);

    //! Returns a new reference
    DLLLOCAL PyObject* getPythonValue(QoreValue val, ExceptionSink* xsink);

    //! Inserts a new class in the map
    DLLLOCAL void insertClass(py_cls_map_t::iterator i, const QoreClass* qcls, PythonQoreClass* pycls) {
        py_cls_map.insert(i, py_cls_map_t::value_type(qcls, pycls));
    }

    DLLLOCAL void insertClass(const QoreClass* qcls, PythonQoreClass* pycls) {
        py_cls_map.insert(py_cls_map_t::value_type(qcls, pycls));
    }

    //! Creates a new module or package
    DLLLOCAL PyObject* newModule(const char* name, const QoreNamespace* ns_pkg = nullptr);

    //! Returns a c string for the given python unicode value
    DLLLOCAL static const char* getCString(PyObject* obj) {
        assert(PyUnicode_Check(obj));
        return PyUnicode_AsUTF8(obj);
    }

    DLLLOCAL static QorePythonProgram* getPythonProgramFromMethod(const QoreMethod& meth, ExceptionSink* xsink) {
        const QoreClass* cls = meth.getClass();
        assert(dynamic_cast<const QorePythonClass*>(cls));
        return static_cast<const QorePythonClass*>(cls)->getPythonProgram();
    }

    DLLLOCAL static QorePythonProgram* getExecutionContext();

    DLLLOCAL static QorePythonProgram* getContext();

    //! Static initialization
    DLLLOCAL static int staticInit();

    //! Delete thread local data when a thread terminates
    DLLLOCAL static void pythonThreadCleanup(void*);

    //! Does this thread hold the GIL?
    DLLLOCAL static bool haveGil();

    //! Does this thread hold the GIL with the given thread state?
    DLLLOCAL static bool haveGilUnlocked(PyThreadState* tstate);

    //! Returns the program count
    DLLLOCAL static int getProgramCount() {
        AutoLocker al(py_thr_lck);
        return pgm_count;
    }

protected:
    PyInterpreterState* interpreter;
    QorePythonReferenceHolder module;
    QorePythonReferenceHolder python_code;
    PyObject* module_dict = nullptr;
    PyObject* builtin_dict = nullptr;
    //! each Python program object must have a corresponding Qore program object for Qore class generation
    QoreProgram* qpgm = nullptr;
    //! Python namespace ptr
    QoreNamespace* pyns = nullptr;
    //! module context when importing Python modules to Qore
    const char* module_context = nullptr;

    // list of objects to dereference when classes are deleted
    typedef std::vector<PyObject*> obj_sink_t;
    obj_sink_t obj_sink;

    //! set to true if this object owns the Qore program reference
    bool owns_qore_program_ref = false;
    //! true if the object is valid
    bool valid = true;
    //! needs deregistration
    bool needs_deregistration = false;
    //! has this object been destroyed
    bool destroyed = false;

    //! if we should destroy the interpreter state
    bool owns_interpreter = false;

    //! maps types to classes
    typedef std::map<PyTypeObject*, QorePythonClass*> clmap_t;
    clmap_t clmap;

    //! maps python functions to Qore functions
    typedef std::map<PyObject*, QoreExternalFunction*> flmap_t;
    flmap_t flmap;

    //! ensures modulea are only imported once
    typedef std::set<PyObject*> pyobj_set_t;
    pyobj_set_t mod_set;

    //! set of unique strings
    strset_t strset;

    //! mutex for thread state map
    static QoreThreadLock py_thr_lck;
    //! map of TIDs to the thread state
    typedef std::map<int, QorePythonThreadStateInfo> py_tid_map_t;
    //! map of QorePythonProgram objects to thread states for the current thread
    typedef std::map<const QorePythonProgram*, py_tid_map_t> py_thr_map_t;
    DLLLOCAL static py_thr_map_t py_thr_map;
    //! for lookups from TID to thread state
    typedef std::set<PyThreadState*> py_thr_set_t;
    typedef std::map<int, py_thr_set_t> py_global_tid_map_t;
    DLLLOCAL static py_global_tid_map_t py_global_tid_map;

    //! number of program objects; writable only in the py_thr_lck lock
    DLLLOCAL static unsigned pgm_count;

    // for local program thread management
    mutable int pgm_thr_cnt = 0;
    mutable int pgm_thr_waiting = 0;
    mutable QoreCondition pgm_thr_cond;

    // call reference for saving object references
    mutable ReferenceHolder<ResolvedCallReferenceNode> save_object_callback;

    //! Map of Qore classes to Python classes
    py_cls_map_t py_cls_map;

    typedef std::vector<PyMethodDef*> meth_vec_t;
    meth_vec_t meth_vec;

    //! for weak refs
    QoreReferenceCounter weak_refs;

    //! Saves Qore objects in thread-local data
    DLLLOCAL int saveQoreObjectFromPythonDefault(const QoreValue& rv, ExceptionSink& xsink);

    DLLLOCAL int importQoreNamespaceToPython(PyObject* mod, const QoreNamespace& ns);

    DLLLOCAL QoreNamespace* getNamespaceForObject(PyObject* type);

    DLLLOCAL QoreClass* getCreateQorePythonClassIntern(ExceptionSink* xsink, PyTypeObject* type, strset_t& nsset,
            const char* cls_name = nullptr, int flags = 0);

    DLLLOCAL QorePythonClass* addClassToNamespaceIntern(ExceptionSink* xsink, QoreNamespace* ns, PyTypeObject* type,
            const char* cname, clmap_t::iterator i, strset_t& nsset, int flags = 0);

    //! Call a method and and return the result
    DLLLOCAL QoreValue callCFunctionMethod(ExceptionSink* xsink, PyObject* func, const QoreListNode* args,
            size_t arg_offset = 0);

    //! Call a wrapper descriptor method and return the result
    DLLLOCAL QoreValue callWrapperDescriptorMethod(ExceptionSink* xsink, PyObject* self, PyObject* obj,
            const QoreListNode* args, size_t arg_offset = 0);
    //! Call a method descriptor method and return the result
    DLLLOCAL QoreValue callMethodDescriptorMethod(ExceptionSink* xsink, PyObject* self, PyObject* obj,
            const QoreListNode* args, size_t arg_offset = 0);
    //! Call a classmethod descriptor method and return the result
    DLLLOCAL QoreValue callClassMethodDescriptorMethod(ExceptionSink* xsink, PyObject* self, PyObject* obj,
            const QoreListNode* args, size_t arg_offset = 0);

    //! Retrieve and import the given symbol
    DLLLOCAL int checkImportSymbol(ExceptionSink* xsink, const char* module, PyObject* mod, bool is_package,
            const char* symbol, int filter, bool ignore_missing);

    //! Import the given symbol into the Qore program object
    DLLLOCAL int importSymbol(ExceptionSink* xsink, PyObject* value, const char* module, const char* symbol,
            int filter);

    //! Imports the given module
    DLLLOCAL int importModule(ExceptionSink* xsink, PyObject* mod, const char* module, int filter);

    //! Saves the module in sys.modules
    DLLLOCAL int saveModule(const char* name, PyObject* mod);

    //! Creates a new module package with an explicit path name
    DLLLOCAL PyObject* newModule(const char* name, const char* path);

    DLLLOCAL int findCreateQoreFunction(PyObject* value, const char* symbol, q_external_func_t func);

    //! Returns a Qore value for the given Python value; does not dereference val
    DLLLOCAL QoreValue getQoreValue(ExceptionSink* xsink, PyObject* val, pyobj_set_t& rset);
    //! Returns a Qore value for the given Python value
    DLLLOCAL QoreValue getQoreValue(ExceptionSink* xsink, QorePythonReferenceHolder& val, pyobj_set_t& rset);
    //! Returns a Qore hash from a Python dict
    DLLLOCAL QoreHashNode* getQoreHashFromDict(ExceptionSink* xsink, PyObject* val, pyobj_set_t& rset);
    //! Returns a Qore list from a Python list
    DLLLOCAL QoreListNode* getQoreListFromList(ExceptionSink* xsink, PyObject* val, pyobj_set_t& rset);
    //! Returns a Qore list from a Python tuple
    DLLLOCAL QoreListNode* getQoreListFromTuple(ExceptionSink* xsink, PyObject* val, pyobj_set_t& rset,
            size_t offset = 0, bool for_args = false);
    //! Returns a Qore call reference from a Python function
    DLLLOCAL ResolvedCallReferenceNode* getQoreCallRefFromFunc(ExceptionSink* xsink, PyObject* val);
    //! Returns a Qore call reference from a Python method
    DLLLOCAL ResolvedCallReferenceNode* getQoreCallRefFromMethod(ExceptionSink* xsink, PyObject* val);

    //! Sets and replaces the global dictionoary
    DLLLOCAL int setGlobalDictionary(PyObject* mod);

    //! Waits for all threads to complete; must be called in the py_thr_lck lock
    DLLLOCAL void waitForThreadsIntern();

    DLLLOCAL static void execPythonConstructor(const QoreMethod& meth, PyObject* pycls, QoreObject* self,
            const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink);
    DLLLOCAL static void execPythonDestructor(const QorePythonClass& thisclass, PyObject* pycls, QoreObject* self,
            QorePythonPrivateData* pd, ExceptionSink* xsink);

    DLLLOCAL static QoreValue execPythonStaticMethod(const QoreMethod& meth, PyObject* m, const QoreListNode* args,
            q_rt_flags_t rtflags, ExceptionSink* xsink);
    DLLLOCAL static QoreValue execPythonNormalMethod(const QoreMethod& meth, PyObject* m, QoreObject* self,
            QorePythonPrivateData* pd, const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink);

    DLLLOCAL static QoreValue execPythonNormalWrapperDescriptorMethod(const QoreMethod& meth, PyObject* m,
            QoreObject* self, QorePythonPrivateData* pd, const QoreListNode* args, q_rt_flags_t rtflags,
            ExceptionSink* xsink);
    DLLLOCAL static QoreValue execPythonNormalMethodDescriptorMethod(const QoreMethod& meth, PyObject* m,
            QoreObject* self, QorePythonPrivateData* pd, const QoreListNode* args, q_rt_flags_t rtflags,
            ExceptionSink* xsink);
    DLLLOCAL static QoreValue execPythonNormalClassMethodDescriptorMethod(const QoreMethod& meth, PyObject* m,
            QoreObject* self, QorePythonPrivateData* pd, const QoreListNode* args, q_rt_flags_t rtflags,
            ExceptionSink* xsink);

    DLLLOCAL static QoreValue execPythonStaticCFunctionMethod(const QoreMethod& meth, PyObject* func,
            const QoreListNode* args, q_rt_flags_t rtflags, ExceptionSink* xsink);

    DLLLOCAL static QoreValue execPythonCFunction(PyObject* func, const QoreListNode* args, q_rt_flags_t rtflags,
            ExceptionSink* xsink);
    DLLLOCAL static QoreValue execPythonFunction(PyObject* func, const QoreListNode* args, q_rt_flags_t rtflags,
            ExceptionSink* xsink);

    //! Python integration
    DLLLOCAL static PyObject* callQoreFunction(PyObject* self, PyObject* args);

    DLLLOCAL virtual ~QorePythonProgram() {
        //printd(5, "QorePythonProgram::~QorePythonProgram() this: %p\n", this);
        assert(!qpgm);
    }

    DLLLOCAL void deleteIntern(ExceptionSink* xsink);

    //! the GIL must be held when this function is called
    DLLLOCAL int createInterpreter(QorePythonGilHelper& qpgh, ExceptionSink* xsink);

    //! Returns the Python thread state for this interpreter
    DLLLOCAL PyThreadState* getAcquireThreadState() const {
        AutoLocker al(py_thr_lck);
        ++pgm_thr_cnt;
        return getThreadStateIntern();
    }

    //! Returns the Python thread state for this interpreter; releases the thread context
    DLLLOCAL PyThreadState* getReleaseThreadState() const {
        AutoLocker al(py_thr_lck);
        PyThreadState* python = getThreadStateIntern();
#if 0
        // XXX DEBUG
        if (!python) {
            py_thr_map_t::iterator i = py_thr_map.find(this);
            if (i == py_thr_map.end()) {
                printd(0, "QorePythonProgram::releaseContext() ERROR missing pgm: %p\n", this);
            } else {
                for (auto& i : i->second) {
                    printd(0, "QorePythonProgram::releaseContext() this: %p ERROR missing TID: {TID %d, {%p, "
                        "own: %d}}\n", this, i.first, i.second.state, i.second.owns_state);
                }
            }
        }
        // XXX DEBUG
#endif
        assert(python);

        if (!--pgm_thr_cnt && pgm_thr_waiting) {
            pgm_thr_cond.signal();
        }
        return python;
    }

    //! Returns the Python thread state for this interpreter; py_thr_lck must be held
    DLLLOCAL PyThreadState* getThreadStateIntern() const {
        py_thr_map_t::iterator i = py_thr_map.find(this);
        if (i == py_thr_map.end()) {
            return nullptr;
        }
        int tid = q_gettid();
        py_tid_map_t::iterator ti = i->second.find(tid);
        //printd(5, "QorePythonProgram::getThreadState() this: %p found TID %d: %p\n", this, q_gettid(),
        //  ti == i->second.end() ? nullptr : ti->second);
        return ti == i->second.end() ? nullptr : ti->second.state;
    }

    //! Creates a QoreProgram object owned by this object
    DLLLOCAL void createQoreProgram();
};

class QorePythonProgramData : public AbstractPrivateData, public QorePythonProgram {
public:
   DLLLOCAL QorePythonProgramData(const QoreString& source_code, const QoreString& source_label, int start,
        ExceptionSink* xsink) : QorePythonProgram(source_code, source_label, start, xsink) {
        //printd(5, "QorePythonProgramData::QorePythonProgramData() this: %p\n", this);
    }

    using AbstractPrivateData::deref;
    DLLLOCAL virtual void deref(ExceptionSink* xsink) {
        if (ROdereference()) {
            deleteIntern(xsink);
            weakDeref();
        }
    }

private:
    DLLLOCAL ~QorePythonProgramData() {
    }
};

class QorePythonProgramWeakReferenceHolder {
public:
    DLLLOCAL QorePythonProgramWeakReferenceHolder(QorePythonProgram* py_pgm) : py_pgm(py_pgm) {
    }

    DLLLOCAL ~QorePythonProgramWeakReferenceHolder() {
        py_pgm->weakDeref();
    }

    DLLLOCAL QorePythonProgram* operator->() {
        return py_pgm;
    }

    DLLLOCAL QorePythonProgram* operator*() {
        return py_pgm;
    }

private:
    QorePythonProgram* py_pgm;
};

class PythonModuleContextHelper {
public:
    DLLLOCAL PythonModuleContextHelper(QorePythonProgram* pypgm, const char* mod) : pypgm(pypgm),
        old_module(pypgm->module_context) {
        pypgm->module_context = mod;
    }

    DLLLOCAL ~PythonModuleContextHelper() {
        pypgm->module_context = old_module;
    }

private:
    QorePythonProgram* pypgm;
    const char* old_module;
};

#endif