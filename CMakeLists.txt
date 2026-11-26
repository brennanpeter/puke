cmake_minimum_required(VERSION 3.20)
project(puke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenImageIO CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3.2 CONFIG REQUIRED)
# OpenColorIO: use its CMake package where it works (source builds, vcpkg,
# most distros). Ubuntu 24.04's libopencolorio-dev ships a broken one (wrong
# location and include path), so if no package was found, fall back to
# pkg-config. Don't let the broken package be picked up via a stale cache.
find_package(OpenColorIO 2.1 CONFIG QUIET)
if(NOT OpenColorIO_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OCIO REQUIRED IMPORTED_TARGET OpenColorIO>=2.1)
    add_library(OpenColorIO::OpenColorIO ALIAS PkgConfig::OCIO)
endif()

add_library(puke_core
    src/core/Downscale.cpp
    src/core/FramePattern.cpp
    src/core/Path.cpp
    src/graph/Node.cpp
    src/graph/Curve.cpp
    src/graph/Graph.cpp
    src/engine/Engine.cpp
    src/engine/ThreadPool.cpp
    src/engine/TileCache.cpp
    src/engine/Snapshot.cpp
    src/nodes/PremultNode.cpp
    src/nodes/ReadNode.cpp
    src/nodes/ReformatNode.cpp
    src/nodes/RotoNode.cpp
    src/nodes/ShuffleNode.cpp
    src/nodes/WriteNode.cpp
    src/nodes/GradeNode.cpp
    src/nodes/MergeNode.cpp
    src/nodes/TransformNode.cpp
    src/nodes/Registry.cpp
    src/nodes/ColorSpaceNode.cpp
    src/nodes/ConstantNode.cpp
    src/nodes/CropNode.cpp
    src/nodes/BackdropNode.cpp
    src/nodes/BlurNode.cpp
    src/project/Project.cpp
    src/color/ColorConfig.cpp
    src/ofx/OfxHost.cpp
    src/ofx/OfxNode.cpp
    src/ofx/Overlay.cpp
)
target_include_directories(puke_core PUBLIC include)
# The Python module links puke_core into a shared library.
set_target_properties(puke_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(puke_core PUBLIC OpenImageIO::OpenImageIO Threads::Threads)
# OpenFX plugin hosting: the vendored OFX headers stay private; plugins are
# loaded with dlopen.
target_include_directories(puke_core PRIVATE third_party/openfx/include)
target_link_libraries(puke_core PRIVATE ${CMAKE_DL_LIBS})
# JSON is an implementation detail of project files; it stays out of public headers.
target_link_libraries(puke_core PRIVATE nlohmann_json::nlohmann_json OpenColorIO::OpenColorIO)
if(MSVC)
    target_compile_options(puke_core PRIVATE /W4)
else()
    target_compile_options(puke_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(puke_render tools/puke_render.cpp)
target_link_libraries(puke_render PRIVATE puke_core)

enable_testing()
foreach(test read_write engine nodes more_nodes roto animation proxy cli golden project color ofx)
    add_executable(test_${test} tests/test_${test}.cpp)
    target_link_libraries(test_${test} PRIVATE puke_core)
    add_test(NAME ${test} COMMAND test_${test} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    # Tests use puke's built-in color config, whatever $OCIO says.
    set_tests_properties(${test} PROPERTIES ENVIRONMENT "OCIO=")
endforeach()

# The command-line test runs the real puke_render binary.
target_compile_definitions(test_cli PRIVATE PUKE_RENDER_PATH="$<TARGET_FILE:puke_render>")
add_dependencies(test_cli puke_render)

# Reference images for the golden-image test live in the source tree.
target_compile_definitions(test_golden PRIVATE PUKE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")

# OFX test plugins (tests/ofx/TestPlugins.cpp), built as a bundle in
# <build>/ofx_test, next to a deliberately broken one. The OFX and Python
# tests find them through $OFX_PLUGIN_PATH.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(PUKE_OFX_ARCH Linux-x86-64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(PUKE_OFX_ARCH Linux-arm-64)
    else()
        set(PUKE_OFX_ARCH Linux-x86)
    endif()
    set(PUKE_OFX_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/ofx_test)
    add_library(puke_test_ofx_plugins MODULE tests/ofx/TestPlugins.cpp)
    # The Point plugin's overlay draws with OpenGL, when there is one.
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
        target_link_libraries(puke_test_ofx_plugins PRIVATE OpenGL::GL)
        target_compile_definitions(puke_test_ofx_plugins PRIVATE PUKE_TEST_GL=1)
    endif()
    target_include_directories(puke_test_ofx_plugins PRIVATE third_party/openfx/include)
    set_target_properties(puke_test_ofx_plugins PROPERTIES
        OUTPUT_NAME pukeTestPlugins PREFIX "" SUFFIX ".ofx"
        CXX_VISIBILITY_PRESET hidden
        LIBRARY_OUTPUT_DIRECTORY ${PUKE_OFX_TEST_DIR}/pukeTestPlugins.ofx.bundle/Contents/${PUKE_OFX_ARCH})
    file(WRITE ${PUKE_OFX_TEST_DIR}/Broken.ofx.bundle/Contents/${PUKE_OFX_ARCH}/Broken.ofx "not a plugin\n")
    add_dependencies(test_ofx puke_test_ofx_plugins)
    set_tests_properties(ofx PROPERTIES ENVIRONMENT "OCIO=;OFX_PLUGIN_PATH=${PUKE_OFX_TEST_DIR}")
endif()

# The desktop GUI (puke_gui). Optional: needs Qt 6 Widgets.
option(PUKE_GUI "Build the GUI, if Qt 6 is available" ON)
if(PUKE_GUI)
    find_package(Qt6 6.2 COMPONENTS Widgets OpenGL QUIET)
    find_package(OpenGL QUIET)  # OFX overlays draw with it
    if(Qt6_FOUND AND OpenGL_FOUND)
        set(CMAKE_AUTOMOC ON)
        add_library(puke_gui_lib STATIC
            gui/CurveEditor.cpp
            gui/Document.cpp
            gui/MainWindow.cpp
            gui/NodeGraphView.cpp
            gui/OverlayRenderer.cpp
            gui/ProjectSettingsDialog.cpp
            gui/PropertiesPanel.cpp
            gui/RenderDialog.cpp
            gui/RenderJob.cpp
            gui/RotoTool.cpp
            gui/Timeline.cpp
            gui/Viewer.cpp)
        target_link_libraries(puke_gui_lib PUBLIC puke_core Qt6::Widgets Qt6::OpenGL OpenGL::GL)
        target_include_directories(puke_gui_lib PUBLIC gui)
        if(NOT MSVC)
            target_compile_options(puke_gui_lib PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_executable(puke_gui gui/main.cpp)
        target_link_libraries(puke_gui PRIVATE puke_gui_lib)

        add_executable(test_gui tests/test_gui.cpp)
        target_link_libraries(test_gui PRIVATE puke_gui_lib)
        target_include_directories(test_gui PRIVATE tests)
        add_test(NAME gui COMMAND test_gui WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(gui PROPERTIES
            ENVIRONMENT "OCIO=;QT_QPA_PLATFORM=offscreen;OFX_PLUGIN_PATH=${PUKE_OFX_TEST_DIR}")
        # OFX overlays drawn with OpenGL: needs a display, so under xvfb-run
        # where it's installed (exit code 77: skipped, no OpenGL).
        if(TARGET puke_test_ofx_plugins)
            add_executable(test_overlay_gl tests/test_overlay_gl.cpp)
            target_link_libraries(test_overlay_gl PRIVATE puke_gui_lib)
            target_include_directories(test_overlay_gl PRIVATE tests)
            add_dependencies(test_overlay_gl puke_test_ofx_plugins)
            find_program(XVFB_RUN xvfb-run)
            if(XVFB_RUN)
                add_test(NAME overlay_gl COMMAND ${XVFB_RUN} -a ${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=xcb
                         $<TARGET_FILE:test_overlay_gl>)
            else()
                add_test(NAME overlay_gl COMMAND test_overlay_gl)
            endif()
            set_tests_properties(overlay_gl PROPERTIES SKIP_RETURN_CODE 77
                ENVIRONMENT "OCIO=;OFX_PLUGIN_PATH=${PUKE_OFX_TEST_DIR}")
        endif()
        message(STATUS "GUI: building puke_gui with Qt ${Qt6_VERSION}")
    else()
        message(STATUS "GUI: skipped (needs Qt 6 Widgets and OpenGL, and OpenGL headers)")
    endif()
endif()

# Python module (import puke). Optional: needs pybind11 and Python's
# development headers. Built into <build>/python; add that to PYTHONPATH.
option(PUKE_PYTHON "Build the Python module, if pybind11 is available" ON)
if(PUKE_PYTHON)
    find_package(Python 3.8 COMPONENTS Interpreter Development.Module)
    set(PYBIND11_FINDPYTHON ON)
    find_package(pybind11 2.6 CONFIG QUIET)
    if(Python_FOUND AND pybind11_FOUND)
        pybind11_add_module(puke_python python/module.cpp)
        set_target_properties(puke_python PROPERTIES
            OUTPUT_NAME puke
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/python)
        target_link_libraries(puke_python PRIVATE puke_core)
        if(NOT MSVC)
            target_compile_options(puke_python PRIVATE -Wall -Wextra -Wpedantic)
        endif()

        add_test(NAME python
                 COMMAND ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python.py
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(python PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python;OCIO=;OFX_PLUGIN_PATH=${PUKE_OFX_TEST_DIR}")
        if(TARGET puke_test_ofx_plugins)
            add_dependencies(puke_python puke_test_ofx_plugins)
        endif()
        message(STATUS "Python module: building for ${Python_EXECUTABLE}")
    else()
        message(STATUS "Python module: skipped (needs pybind11 and Python development headers)")
    endif()
endif()
