cmake_minimum_required(VERSION 3.12...3.31)
# Copyright (C) 2026 Qore Technologies, s.r.o.
option(QORE_PYTHON_STRICT_DOCS "Fail Python module documentation on warnings" OFF)
project(qore-python-module)

set (VERSION_MAJOR 1)
set (VERSION_MINOR 3)
set (VERSION_PATCH 0)

set(PROJECT_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

if (${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.12.0")
    cmake_policy(SET CMP0074 NEW)

    if (${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.18.0")
        cmake_policy(SET CMP0053 NEW)
    endif()
endif()

cmake_policy(SET CMP0042 NEW)

if (UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif()

# the minimum Python version: the thread state management of the module supports Python 3.11 and later
set(MIN_PY_MINOR_VER 11)

find_package(Qore 1.0 REQUIRED)

if (DEFINED ENV{Python3_ROOT})
    set(Python3_ROOT $ENV{Python3_ROOT})
    set(Python3_ROOT_DIR $ENV{Python3_ROOT})
endif()
if (DEFINED ENV{Python3_LIBRARIES})
    set(Python3_LIBRARIES $ENV{Python3_LIBRARIES})
endif()
if (DEFINED ENV{Python3_INCLUDE_DIRS})
    set(Python3_INCLUDE_DIRS $ENV{Python3_INCLUDE_DIRS})
endif()
if (DEFINED ENV{Python3_EXECUTABLE})
    set(Python3_EXECUTABLE $ENV{Python3_EXECUTABLE})
endif()

find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
if(${Python3_VERSION_MINOR} VERSION_LESS ${MIN_PY_MINOR_VER})
    message(FATAL_ERROR "Python version ${Python3_VERSION} is unsupported; the minimum Python version is 3.${MIN_PY_MINOR_VER}")
endif()
message(STATUS "Found Python3 libs: ${Python3_LIBRARIES}")
message(STATUS "Found Python3 includes: ${Python3_INCLUDE_DIRS}")

# Check for Python internal includes
# Python 3.13+ requires internal includes for proper thread state management
# Python 3.12 and earlier use bundled internals
include(CheckCXXSourceCompiles)
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${Python3_INCLUDE_DIRS})
# Use bundled internal definitions for all Python versions
# Our src/python*_internals.h files provide the necessary thread state management
message(STATUS "Python ${Python3_VERSION} - using bundled internals")

# The state of the GIL in the child of fork() is read by a C file compiled with CPython's internal headers (C headers,
# installed with the Python headers), which give the layout of the GIL of the Python library.  The file itself is
# compiled here, with and without the test hooks of debug builds, so that a Python version without the headers, or
# whose internal structures changed, is built without it instead of failing: a child created while another thread
# held the GIL then waits forever on its first use of Python instead of raising PYTHON-FORK-ERROR
set(_qore_python_fork_gil_check_dir ${CMAKE_CURRENT_BINARY_DIR}/fork_gil_check)
set(_qore_python_fork_gil_try_type ${CMAKE_TRY_COMPILE_TARGET_TYPE})
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
try_compile(QORE_PYTHON_HAVE_INTERNAL_C_HEADERS ${_qore_python_fork_gil_check_dir}/release
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/python_fork_gil.c
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${Python3_INCLUDE_DIRS};${CMAKE_CURRENT_SOURCE_DIR}/src"
    OUTPUT_VARIABLE _qore_python_fork_gil_output)
if (QORE_PYTHON_HAVE_INTERNAL_C_HEADERS)
    try_compile(QORE_PYTHON_HAVE_INTERNAL_C_HEADERS ${_qore_python_fork_gil_check_dir}/debug
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/python_fork_gil.c
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${Python3_INCLUDE_DIRS};${CMAKE_CURRENT_SOURCE_DIR}/src"
        COMPILE_DEFINITIONS -DDEBUG
        OUTPUT_VARIABLE _qore_python_fork_gil_output)
endif()
set(CMAKE_TRY_COMPILE_TARGET_TYPE ${_qore_python_fork_gil_try_type})
if (QORE_PYTHON_HAVE_INTERNAL_C_HEADERS)
    message(STATUS "Python internal headers found: a fork() child that cannot use Python raises PYTHON-FORK-ERROR")
else()
    message(WARNING "src/python_fork_gil.c does not compile with the internal headers (internal/pycore_*.h) of "
        "Python ${Python3_VERSION} (missing, or with other internal structures): a child process created with fork() "
        "while another thread held the GIL waits forever on its first use of Python")
endif()

# Python 3.12 and later keep in each thread state whether it is bound to its thread for the GIL state API
# (PyThreadState::_status.bound_gilstate, a CPython-specific member); the module clears it before it deletes a state
# of another thread, so that CPython does not clear the binding of the deleting thread instead
if (${Python3_VERSION_MINOR} GREATER_EQUAL 12)
    set(CMAKE_REQUIRED_INCLUDES ${Python3_INCLUDE_DIRS})
    check_c_source_compiles("
#include <Python.h>
int main(void) {
    PyThreadState tstate;
    tstate._status.bound_gilstate = 0;
    return tstate._status.bound_gilstate;
}" QORE_PYTHON_HAVE_TSTATE_BOUND_GILSTATE)
    if (QORE_PYTHON_HAVE_TSTATE_BOUND_GILSTATE)
        add_definitions(-DQORE_PYTHON_HAVE_TSTATE_BOUND_GILSTATE=1)
    else()
        message(WARNING "Python ${Python3_VERSION} has no PyThreadState::_status.bound_gilstate member: the binding of "
            "a thread to its GIL state can be cleared by another thread when it deletes the thread's state")
    endif()
endif()

# Python 3.11 - 3.13 limit the recursion that uses the C stack with a counter in each thread state (a CPython-specific
# member: recursion_remaining in 3.11, which also counts Python calls, and c_recursion_remaining in 3.12 and 3.13) that
# is sized for the stack of the main thread; the module reduces it for the stack of the Qore thread (Python 3.14
# checks the stack of the thread itself and has no such member)
if (${Python3_VERSION_MINOR} EQUAL 11)
    set(_qore_python_c_recursion_field recursion_remaining)
elseif (${Python3_VERSION_MINOR} EQUAL 12 OR ${Python3_VERSION_MINOR} EQUAL 13)
    set(_qore_python_c_recursion_field c_recursion_remaining)
endif()
if (DEFINED _qore_python_c_recursion_field)
    set(CMAKE_REQUIRED_INCLUDES ${Python3_INCLUDE_DIRS})
    check_c_source_compiles("
#include <Python.h>
int main(void) {
    PyThreadState tstate;
    tstate.${_qore_python_c_recursion_field} = 0;
    return tstate.${_qore_python_c_recursion_field};
}" QORE_PYTHON_HAVE_TSTATE_C_RECURSION_COUNTER)
    if (QORE_PYTHON_HAVE_TSTATE_C_RECURSION_COUNTER)
        add_definitions(-DQORE_PYTHON_TSTATE_C_RECURSION_COUNTER=${_qore_python_c_recursion_field})
    else()
        message(WARNING "Python ${Python3_VERSION} has no PyThreadState::${_qore_python_c_recursion_field} member: "
            "deep recursion can overflow the stack of Qore threads with small stacks")
    endif()
endif()

# Check for C++17 (required for Python 3.13+ and inline thread_local).
include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-std=c++17" COMPILER_SUPPORTS_CXX17)
if(COMPILER_SUPPORTS_CXX17)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
else()
    message(FATAL_ERROR "The compiler ${CMAKE_CXX_COMPILER} has no C++17 support. Please use a different C++ compiler.")
endif()

set(QPP_SRC
    src/QC_PythonProgram.qpp
    src/ql_python.qpp
)

set(CPP_SRC
    src/python-module.cpp
    src/QorePythonProgram.cpp
    src/QorePythonClass.cpp
    src/PythonCallableCallReferenceNode.cpp
    src/qoreloadermodule.cpp
    src/QoreMetaPathFinder.cpp
    src/QoreLoader.cpp
    src/JavaLoader.cpp
    src/PythonQoreClass.cpp
    src/PythonQoreCallable.cpp
    src/ModuleNamespace.cpp
    src/QorePythonStackLocationHelper.cpp
)

if (QORE_PYTHON_HAVE_INTERNAL_C_HEADERS)
    set(CPP_SRC ${CPP_SRC} src/python_fork_gil.c)
    add_definitions(-DQORE_PYTHON_HAVE_FORK_GIL_CHECK=1)
endif()

qore_wrap_qpp_value(QPP_SOURCES METALIST QPP_META_SRC ${QPP_SRC})

foreach (it ${QPP_SOURCES})
    GET_FILENAME_COMPONENT(_outfile ${it} NAME_WE)
    set(QPP_DOX ${QPP_DOX} ${CMAKE_CURRENT_BINARY_DIR}/${_outfile}.dox.h)
endforeach()

set(module_name "python")

set(QORE_DOX_TMPL_SRC
  docs/mainpage.dox.tmpl
    docs/guide-python_examples.dox.tmpl
    docs/guide-python_exceptions.dox.tmpl
    docs/guide-python_import.dox.tmpl
    docs/guide-python_qore_import.dox.tmpl
    docs/guide-python_qore_object_lifecycle_management.dox.tmpl
    docs/guide-python_qoreloader_module.dox.tmpl
    docs/guide-python_threads_and_stack.dox.tmpl
    docs/guide-python_types.dox.tmpl
    docs/guide-pythonreleasenotes.dox.tmpl
)

add_library(${module_name} MODULE ${CPP_SRC} ${QPP_SOURCES})

include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${Python3_INCLUDE_DIRS})
target_include_directories(${module_name} PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)

add_custom_target(QORE_INC_FILES DEPENDS ${QORE_INC_SRC})
add_dependencies(${module_name} QORE_INC_FILES)

target_link_libraries(${module_name} ${Python3_LIBRARIES} ${QORE_LIBRARY})

set(MODULE_DOX_INPUT ${CMAKE_CURRENT_BINARY_DIR}/mainpage.dox ${JAVA_JAR_SRC_STR} ${QPP_DOX})
string(REPLACE ";" " " MODULE_DOX_INPUT "${MODULE_DOX_INPUT}")
#message(STATUS mdi: ${MODULE_DOX_INPUT})

if (DEFINED ENV{DOXYGEN_EXECUTABLE})
    set(DOXYGEN_EXECUTABLE $ENV{DOXYGEN_EXECUTABLE})
endif()

list(APPEND MODULE_DOX_INPUT
    ${CMAKE_CURRENT_BINARY_DIR}/guide-python_examples.dox
    ${CMAKE_CURRENT_BINARY_DIR}/guide-python_exceptions.dox
    ${CMAKE_CURRENT_BINARY_DIR}/guide-python_import.dox
    ${CMAKE_CURRENT_BINARY_DIR}/guide-python_qore_import.dox
    ${CMAKE_CURRENT_BINARY_DIR}/guide-python_qore_object_lifecycle_management.dox
    ${CMAKE_CURRENT_BINARY_DIR}/guide-python_qoreloader_module.dox
    ${CMAKE_CURRENT_BINARY_DIR}/guide-python_threads_and_stack.dox
    ${CMAKE_CURRENT_BINARY_DIR}/guide-python_types.dox
    ${CMAKE_CURRENT_BINARY_DIR}/guide-pythonreleasenotes.dox
)
qore_external_binary_module(${module_name} ${PROJECT_VERSION})

qore_dist(${PROJECT_VERSION})

# install qpp metadata files
if (COMMAND qore_install_qpp_metadata)
    qore_install_qpp_metadata(${module_name} ${QPP_META_SRC})
else()
    install(FILES ${QPP_META_SRC}
            DESTINATION ${CMAKE_INSTALL_DATADIR}/qore/metadata)
endif()

qore_config_info()

if (DOXYGEN_FOUND)
    qore_wrap_dox(QORE_DOX_SRC ${QORE_DOX_TMPL_SRC})
    add_custom_target(QORE_MOD_DOX_FILES DEPENDS ${QORE_DOX_SRC})
    add_dependencies(docs-module QORE_MOD_DOX_FILES)
    if (QORE_PYTHON_STRICT_DOCS)
        file(APPEND "${CMAKE_BINARY_DIR}/Doxyfile" "\nWARN_AS_ERROR = FAIL_ON_WARNINGS\n")
    endif()
endif()
