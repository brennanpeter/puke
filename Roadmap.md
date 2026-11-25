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
- [x] **Command-line renderer** (`comp_render`): runs a project's Write
  nodes over a frame range, or converts one input to one output.
- [x] **Tiles and threading**: 64×64 tiles on a fixed grid, built-in thread
  pool, `--threads` flag; output identical for any thread count or tile
  size.
- [x] **Tile cache**: memory-bounded LRU keyed on node hash and tile; off by
  default; Read invalidates on file changes.
- [x] **Python bindings** (pybind11, `import comp`): graphs, nodes and
  parameters, rendering, Write execution, tile cache; images readable from
  numpy without copying. Optional in the build. Node types are created by
  name through a registry that project files will reuse.
- [x] **Project files** (`.comp`, JSON): versioned, diff-friendly, strict
  loading with errors that name the file and node; atomic saves. Saved and
  loaded from C++, Python and `comp_render`.
- [x] **Color management** (OpenColorIO 2.1+): one linear working space
  (the config's `scene_linear` role); Read and Write convert in and out
  (`colorspace`, default `auto`: float files linear, integer files sRGB);
  ColorSpace node. Config from `$OCIO`, or comp's small built-in ACES config
  with ACES studio-config names.
- [x] **OpenFX plugin hosting** (OFX 1.4, Linux): plugins from
  `$OFX_PLUGIN_PATH` and `/usr/OFX/Plugins` become `ofx:<id>` node types;
  Filter and General contexts, float RGBA/Alpha, tiles, declared thread
  safety, all common parameter types. Tested with comp's own test plugins
  and the official OpenFX examples.
- [x] **Render snapshots**: renders work from a frozen copy of the graph
  (copy-on-write parameters, shared node ownership), so it can be edited
  while renders run. Python releases the GIL during renders.
- [x] **GUI, first milestone** (`comp_gui`, Qt 6 Widgets): node graph editor
  (Tab to create, drag to connect, positions saved in projects), properties
  panel built from each node type's parameter descriptions (OFX included),
  viewer on its own render thread with the tile cache, OCIO display/view,
  exposure, channels, pan/zoom and a pixel readout; open/save projects.
- [x] **GUI, second milestone**: undo/redo for every edit; rename, cut/copy/
  paste/duplicate (the clipboard holds project text), Backdrop nodes;
  project frame range and fps (saved in projects, used by `comp_render`);
  a timeline with playback that never skips frames, backed by a cache of
  the frames the viewer has shown; rendering Write nodes in the background
  with progress, a log and cancel, while editing goes on; the viewer
  renders only what's on screen.
- [x] **Project format**: a project width and height (saved in project
  settings, set in Edit > Project Settings with common presets), carried
  to every node in the render context and part of the cache key.
  Constant, the first generator, makes it (or a size of its own).
- [x] **More nodes**: Crop, Reformat (Nuke's resize modes, sharing
  Transform's filters), Shuffle (with an optional second input),
  Premult/Unpremult and Blur (gaussian or box, separable). Nodes can read
  their inputs' infos while rendering (Node::inputInfo()).

## Next

In planned order. Items further down are less settled.

### 1. Roto and OFX generators
Roto: shapes drawn in the viewer, rendered as mattes (Bezier splines,
feather, animation of the points, so it needs keyframes; the biggest
single node). OFX generators: plugins with no inputs, given the project
format as their frame.

### 2. GUI, third milestone
Left over from the second, each needing groundwork first:
- **Proxy resolution**: rendering at a fraction of full size needs a
  render scale passed through the engine to every node (Transform
  offsets, filter sizes, OFX's render scale), or results won't match full
  resolution. Viewer region of interest covers the "big plates" problem
  meanwhile.
- **OFX overlays and custom parameter interacts**: OFX draws these with
  OpenGL, so the viewer needs an OpenGL canvas and the host needs the
  interact suite (pen and key events, redraw requests).
- Smaller: cancelling a render mid-frame (now it stops between files),
  and caching frames ahead of playback in the background (now playback
  caches as it goes).

### Later
- Animation curves on parameters, sub-frame sampling for motion blur and
  retiming.
- Deep compositing.
- GPU backend behind the existing pixel-math interface.
- Run independent branches of the graph at the same time.
- An OCIO config per project (frame range and format are project
  settings already). Pixel aspect ratio in formats (square pixels only
  now).
- Nodes written in Python (needs GIL handling around compute()).
- Install rules and a pip-installable package for the Python module.
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
- **GUI**: there's no keyboard shortcuts editor, and no animation on any
  parameter yet. The frame cache is emptied by any edit, even one that
  doesn't affect the viewed node.
  ThreadSanitizer reports remain inside Qt's own event posting (Qt isn't
  built with TSan); comp's data is race-free (`gui/`).
- **OFX**: no generators (plugins without inputs), no animated parameters,
  no access to other frames than the one rendering, render scale 1 only,
  Linux bundles only. Plugins' own multithreading runs on one thread
  (comp already renders tiles on every core). Parametric parameters
  (curves) aren't hosted. Plugins are never unloaded (`src/ofx/`).
- **Color**: Read and Write convert on one thread (only integer files, or
  an explicit color space, need converting at all). Color is always treated
  as premultiplied when there's an alpha channel; there's no switch for
  straight-alpha images yet. The built-in config has no camera log spaces
  (LogC, S-Log3, Log3G10...); studios with an ACES config get those via
  `$OCIO` (`ColorConfig.cpp`).
- **Project files**: relative paths in Read and Write resolve against the
  working directory, not the project file's folder, so a project should be
  rendered from the folder its paths assume (`Project.h`).
- **Tile cache**: caches every node's tiles; may need a policy such as
  caching only expensive nodes or the viewer's output. Partial edge tiles
  are recomputed when the requested region shifts.
