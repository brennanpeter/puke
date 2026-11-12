cmake_minimum_required(VERSION 3.20)
project(comp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenImageIO CONFIG REQUIRED)

add_library(comp_core
    src/core/FramePattern.cpp
    src/graph/Node.cpp
    src/graph/Graph.cpp
    src/engine/Engine.cpp
    src/nodes/ReadNode.cpp
    src/nodes/WriteNode.cpp
    src/nodes/GradeNode.cpp
    src/nodes/MergeNode.cpp
    src/nodes/TransformNode.cpp
)
target_include_directories(comp_core PUBLIC include)
target_link_libraries(comp_core PUBLIC OpenImageIO::OpenImageIO)
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
