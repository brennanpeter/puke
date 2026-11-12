# comp roadmap

comp is an open source, node-based VFX compositor modeled on Nuke. It is
being built as a headless C++ library first; a GUI comes later and sits on
top of the same public API as every other front end.

Guiding preference: when there's a choice, pick the simpler, more stable
option, and keep it behind an interface so it can be swapped later.

## Architecture

Layers, from the bottom up:

1. **Core types**: image buffers (float32, named channels), regions, frame
   and time (a double), formats with EXR-style data and display windows.
2. **Graph**: nodes, typed parameters, connections, and later animation
   curves and save/load. Holds no pixels.
3. **Evaluation engine**: turns "render this region at this frame" into
   work. Pull-based and region-driven; handles tiling, threading, caching.
4. **Services**: image I/O (OpenImageIO / OpenEXR), color (OpenColorIO),
   plugin hosting (OpenFX plus a native node API).
5. **Front ends**: command-line renderer, Python bindings, GUI. All use
   only the public API.

Key decisions:

- Pull-based, region-driven rendering, modeled on OFX's region of
  definition and region of interest.
- Nodes only compute a region they're given; they never manage threads.
- Hash-based caching, so an edit only recomputes what's downstream of it.
- Arbitrary named channels (depth, motion vectors, AOVs), float32 for
  processing, half or 8/16-bit only at file boundaries.
- CPU first, with pixel math kept behind a small interface so a GPU
  backend can be added later.
- Text project files, versioned and diff-friendly.
- Golden-image tests from the start, so the engine can be refactored safely.

## Done

- [x] **Core types**: `Box`, `Image`, `ImageInfo`, frame patterns
  (`####`, `%04d`, `@@@`).
- [x] **Read and Write nodes** (OpenImageIO): overscan, out-of-range frame
  modes, float/half/uint8/uint16 output, compression, atomic writes
  via `.partial` files.
- [x] **Evaluation engine**: plan, info, region and compute passes; shared
  inputs computed once; images freed as soon as they're no longer needed.
- [x] **Grade, Merge, Transform nodes**, with golden-image tests.
- [x] **Command-line renderer** (`comp_render`): one Read into one Write over
  a frame range.
- [x] **Tiles and threading**: 64×64 tiles on a fixed grid, built-in thread
  pool, `--threads` flag; output identical for any thread count or tile
  size.
- [x] **Tile cache**: memory-bounded LRU keyed on node hash and tile; off by
  default; Read invalidates on file changes.

## Next

In planned order. Items further down are less settled.

### 1. Render snapshots
Freeze the graph state when a render starts, so a GUI can edit parameters
while renders run. Separate a node's *definition* (type, parameter schema),
its *instance* (user values) and the render-time *op* (a frozen, evaluable
copy).

### 2. Project files
A versioned, diff-friendly text format (JSON or a simple custom format)
for saving and loading graphs. `comp_render` then loads a project and runs
its Write nodes instead of wiring a single Read into a Write.

### 3. Python bindings
pybind11 or nanobind, covering graph building, parameters and rendering.

### 4. Color management (OpenColorIO)
Input and output color spaces on Read and Write, plus a color-space
conversion node.

### 5. OpenFX plugin hosting
Run existing OFX effects as nodes.

### 6. GUI
Qt node graph, viewer and properties panels, built on the public API.
Turns on the tile cache for interactive use.

### Later
- Animation curves on parameters, sub-frame sampling for motion blur and
  retiming.
- Deep compositing.
- GPU backend behind the existing pixel-math interface.
- Run independent branches of the graph at the same time.
- Look at Natron (open source, OFX-based, modeled on Nuke) for ideas and
  pitfalls.

## Known gaps

Smaller items already marked in the code:

- **Read**: reopens the file on every request; tiled files are read whole
  then cropped. Needs an open-file/tile cache, which would also let Read
  use engine tiles (`ReadNode.h`, `ReadNode.cpp`).
- **Read**: frames are sampled at the nearest whole frame (`ReadNode.cpp`).
- **Read**: deep images aren't supported yet (`ReadNode.cpp`).
- **Write**: an offset data window can't be written to PNG or JPEG; should
  pad or crop to the display window (`WriteNode.cpp`).
- **Transform**: downscaling by more than about 2× aliases, because the
  filter doesn't widen (`TransformNode.h`).
- **Merge**: no row-wise fast paths; every read is bounds-checked
  (`MergeNode.cpp`).
- **Tile cache**: caches every node's tiles; may need a policy such as
  caching only expensive nodes or the viewer's output. Partial edge tiles
  are recomputed when the requested region shifts.
