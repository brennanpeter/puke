cmake_minimum_required(VERSION 3.20)
project(comp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenImageIO CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(comp_core
    src/core/FramePattern.cpp
    src/graph/Node.cpp
    src/graph/Graph.cpp
    src/engine/Engine.cpp
    src/engine/ThreadPool.cpp
    src/engine/TileCache.cpp
    src/nodes/ReadNode.cpp
    src/nodes/WriteNode.cpp
    src/nodes/GradeNode.cpp
    src/nodes/MergeNode.cpp
    src/nodes/TransformNode.cpp
    src/nodes/Registry.cpp
)
target_include_directories(comp_core PUBLIC include)
# The Python module links comp_core into a shared library.
set_target_properties(comp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(comp_core PUBLIC OpenImageIO::OpenImageIO Threads::Threads)
if(MSVC)
    target_compile_options(comp_core PRIVATE /W4)
else()
    target_compile_options(comp_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(comp_render tools/comp_render.cpp)
target_link_libraries(comp_render PRIVATE comp_core)

enable_testing()
foreach(test read_write engine nodes cli golden)
    add_executable(test_${test} tests/test_${test}.cpp)
    target_link_libraries(test_${test} PRIVATE comp_core)
    add_test(NAME ${test} COMMAND test_${test} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()

# The command-line test runs the real comp_render binary.
target_compile_definitions(test_cli PRIVATE COMP_RENDER_PATH="$<TARGET_FILE:comp_render>")
add_dependencies(test_cli comp_render)

# Reference images for the golden-image test live in the source tree.
target_compile_definitions(test_golden PRIVATE COMP_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")

# Python module (import comp). Optional: needs pybind11 and Python's
# development headers. Built into <build>/python; add that to PYTHONPATH.
option(COMP_PYTHON "Build the Python module, if pybind11 is available" ON)
if(COMP_PYTHON)
    find_package(Python 3.8 COMPONENTS Interpreter Development.Module)
    set(PYBIND11_FINDPYTHON ON)
    find_package(pybind11 2.6 CONFIG QUIET)
    if(Python_FOUND AND pybind11_FOUND)
        pybind11_add_module(comp_python python/module.cpp)
        set_target_properties(comp_python PROPERTIES
            OUTPUT_NAME comp
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/python)
        target_link_libraries(comp_python PRIVATE comp_core)
        if(NOT MSVC)
            target_compile_options(comp_python PRIVATE -Wall -Wextra -Wpedantic)
        endif()

        add_test(NAME python
                 COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python.py
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(python PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python")
        message(STATUS "Python module: building for ${Python_EXECUTABLE}")
    else()
        message(STATUS "Python module: skipped (needs pybind11 and Python development headers)")
    endif()
endif()
