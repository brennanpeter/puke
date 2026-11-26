# comp

comp is an open source, node-based VFX compositor modeled on Nuke, written
in C++20. It is a headless rendering library at heart. Three front ends sit
on top of the same public API: a desktop GUI, a command-line renderer and a
Python module.

It's early, but it already works end to end. You can read EXR sequences,
build a node graph, animate it, look at it through OpenColorIO, and render
Write nodes. That works from the GUI, from the command line and from
Python. See [Roadmap.md](Roadmap.md) for what's done and what's next.

## Features

- **Engine**: pull-based and region-driven, like OFX's regions of
  definition and interest. It renders in 64-pixel tiles on a thread pool,
  with a hash-keyed tile cache, so an edit only recomputes what's
  downstream of it. Images are float32 with any named channels, and keep
  EXR-style data and display windows.
- **Nodes**: Read, Write, Constant, Grade, Merge, Transform, Crop,
  Reformat, Shuffle, Premult/Unpremult, Blur, ColorSpace, Roto, Backdrop.
- **OpenFX plugins**: filters, generators and transitions, with plugin
  overlays drawn in the viewer.
- **Color**: OpenColorIO 2. comp uses `$OCIO` when it's set, and
  otherwise a small built-in config.
- **Animation**: keyframes on every parameter (smooth, linear or
  constant, with optional key tangents), and a curve editor.
- **Projects**: text project files (`.comp`, JSON), meant to be easy to
  diff. They hold the frame range, fps and project format.
- **GUI** (Qt 6):
  - Node graph, viewer, properties panel and timeline.
  - Undo and redo for every edit.
  - Copy and paste, and backdrops.
  - Roto shapes drawn and edited in the viewer.
  - Proxy resolution.
  - Playback with a frame cache.
  - Rendering of Write nodes.
- **Tests**: unit, golden-image, CLI, Python and GUI tests, run with
  `ctest`.

## Building

You need CMake 3.20+, a C++20 compiler, and these libraries:

| Library | Needed for |
| --- | --- |
| OpenImageIO | everything (image files) |
| OpenColorIO 2.1+ | everything (color) |
| nlohmann/json 3.2+ | everything (project files) |
| Qt 6.2+ (Widgets, OpenGL) and OpenGL | the GUI (optional) |
| pybind11 2.6+ and Python 3.8+ headers | the Python module (optional) |
| xvfb | the OpenGL overlay test (optional) |

On Ubuntu 24.04:

```sh
sudo apt install build-essential cmake ninja-build \
    libopenimageio-dev libopencolorio-dev nlohmann-json3-dev \
    qt6-base-dev libgl-dev pybind11-dev python3-dev xvfb
```

Then:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The GUI and the Python module are built when their libraries are found.
To skip them, pass `-DCOMP_GUI=OFF` or `-DCOMP_PYTHON=OFF`.

So far comp is built and tested on Linux. The OFX test plugins are built
on Linux only.

## Using it

### The GUI

```sh
build/comp_gui
```

| Key | Does |
| --- | --- |
| Tab | create a node (in the node graph) |
| 1 | view the selected node |
| F | fit the node graph |
| F2 | rename the selected node |
| S | project settings |
| Space | play / stop |
| Left / Right | previous / next frame |
| F7 | render Write nodes |
| Ctrl+Z / Ctrl+Shift+Z | undo / redo |
| Ctrl+D | duplicate |

### The command line

```sh
# Render a project's Write nodes over its frame range, or part of it:
build/comp_render shot.comp
build/comp_render shot.comp --frames 1001-1100 --write Write1

# Convert an image or sequence (#### is the frame number):
build/comp_render plates/shot.####.exr out/shot.####.exr --frames 1001-1100 --compression zip
```

`--threads N` limits the threads used (the default, 0, uses every core).

### Python

The module is built into `build/python`:

```sh
PYTHONPATH=build/python python3
```

```python
import comp

g = comp.Graph()
read = g.create("Read", file="plates/shot.####.exr")
grade = g.create("Grade", multiply=(1.2, 1.1, 1.0, 1.0))
write = g.create("Write", file="out/shot.####.exr")
g.connect(read, grade)
g.connect(grade, write)
g.save("shot.comp")

engine = comp.Engine()
for frame in range(1001, 1011):
    write.execute(engine, frame)
```

### OpenFX plugins

comp loads OFX plugins from the directories in `$OFX_PLUGIN_PATH` and from
`/usr/OFX/Plugins`. Their node types are named `ofx:<plugin identifier>`.

## Layout

| Path | What's there |
| --- | --- |
| `include/comp/`, `src/` | the library: core types, graph, engine, nodes, color, OFX host, projects |
| `gui/` | the Qt GUI |
| `tools/comp_render.cpp` | the command-line renderer |
| `python/module.cpp` | the Python bindings |
| `tests/` | tests, golden images and OFX test plugins |
| `third_party/openfx/` | the OpenFX headers |

## License

comp is released under the [MIT License](LICENSE). The OpenFX headers in
`third_party/openfx/` keep their own license
([third_party/openfx/LICENSE.txt](third_party/openfx/LICENSE.txt)).
