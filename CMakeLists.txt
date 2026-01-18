cmake_minimum_required(VERSION 3.10)
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

# thread-state management only works with Python 3.7+
set(MIN_PY_MINOR_VER 7)

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
set(CMAKE_REQUIRED_INCLUDES ${Python3_INCLUDE_DIRS})
# Use bundled internal definitions for all Python versions
# Our src/python*_internals.h files provide the necessary thread state management
message(STATUS "Python ${Python3_VERSION} - using bundled internals")

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

qore_wrap_qpp_value(QPP_SOURCES ${QPP_SRC})

foreach (it ${QPP_SOURCES})
    GET_FILENAME_COMPONENT(_outfile ${it} NAME_WE)
    set(QPP_DOX ${QPP_DOX} ${CMAKE_CURRENT_BINARY_DIR}/${_outfile}.dox.h)
endforeach()

set(module_name "python")

set(QORE_DOX_TMPL_SRC
  docs/mainpage.dox.tmpl
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

set(MODULE_DOX_INPUT ${CMAKE_BINARY_DIR})
qore_external_binary_module(${module_name} ${PROJECT_VERSION})

qore_dist(${PROJECT_VERSION})

qore_config_info()

if (DOXYGEN_FOUND)
    qore_wrap_dox(QORE_DOX_SRC ${QORE_DOX_TMPL_SRC})
    add_custom_target(QORE_MOD_DOX_FILES DEPENDS ${QORE_DOX_SRC})
    add_dependencies(docs-module QORE_MOD_DOX_FILES)
endif()
