// comp_render: command-line renderer.
//
//   comp_render <project.comp> [--frames A-B] [--threads N] [--write NAME]...
//       Loads a project and runs its Write nodes for each frame: all of
//       them, in project order, or only those named with --write. Without
//       --frames, renders the project's frame range.
//
//   comp_render <input> <output> [--frames A-B] [--threads N]
//               [--datatype half|float|uint8|uint16] [--compression NAME]
//       Quick conversion: wires a single Read into a single Write (frame 1
//       unless --frames is given).
//
// --threads 0 (the default) uses every core; --threads 1 renders on one thread.
//
// Examples:
//   comp_render shot.comp --frames 1001-1100
//   comp_render plates/shot.####.exr out/shot.####.exr --frames 1001-1100 --compression zip

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "comp/core/FramePattern.h"
#include "comp/engine/Engine.h"
#include "comp/graph/Graph.h"
#include "comp/nodes/ReadNode.h"
#include "comp/nodes/WriteNode.h"
#include "comp/project/Project.h"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: comp_render <project.comp> [--frames A-B] [--threads N] [--write NAME]...\n"
                 "       comp_render <input> <output> [--frames A-B] [--threads N]\n"
                 "                   [--datatype half|float|uint8|uint16] [--compression NAME]\n");
}

bool parseRange(const std::string& s, int& first, int& last) {
    // Accepts "N" or "A-B". A leading '-' belongs to the first number.
    auto dash = s.find('-', 1);
    try {
        if (dash == std::string::npos) {
            first = last = std::stoi(s);
        } else {
            first = std::stoi(s.substr(0, dash));
            last = std::stoi(s.substr(dash + 1));
        }
    } catch (...) {
        return false;
    }
    return first <= last;
}

// A whole, non-negative number.
bool parseCount(const std::string& s, int& n) {
    try {
        std::size_t used = 0;
        n = std::stoi(s, &used);
        return used == s.size() && n >= 0;
    } catch (...) {
        return false;
    }
}

// Runs `writes` for every frame, printing each file written. Returns the
// number of failed (frame, Write) pairs; a failure doesn't stop the others.
int renderFrames(const std::vector<const comp::WriteNode*>& writes, int first, int last, int threads,
                 bool showNames) {
    comp::Engine engine(comp::EngineOptions{.threads = threads});
    int failures = 0;
    for (int f = first; f <= last; ++f) {
        for (const comp::WriteNode* write : writes) {
            const std::string label = showNames ? " " + write->name() : "";
            try {
                const std::string written = write->execute(engine, {static_cast<double>(f)});
                std::printf("frame %d%s -> %s\n", f, label.c_str(), written.c_str());
            } catch (const comp::Error& e) {
                std::fprintf(stderr, "frame %d%s failed: %s\n", f, label.c_str(), e.what());
                ++failures;
            }
        }
    }
    return failures;
}

bool checkFrameTokens(const std::vector<const comp::WriteNode*>& writes, int first, int last) {
    if (first == last) return true;
    for (const comp::WriteNode* write : writes) {
        if (!comp::hasFrameToken(write->param<std::string>("file", ""))) {
            std::fprintf(stderr, "%s: output needs a frame token (e.g. ####) to render a range\n",
                         write->name().c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> positional;
    std::vector<std::string> onlyWrites;
    int first = 1, last = 1;
    bool framesGiven = false;
    std::string datatype, compression;
    int threads = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg.rfind("--", 0) != 0) {
            positional.push_back(arg);
        } else if (arg == "--frames" && hasValue) {
            framesGiven = true;
            if (!parseRange(argv[++i], first, last)) {
                std::fprintf(stderr, "invalid frame range: %s\n", argv[i]);
                return 2;
            }
        } else if (arg == "--datatype" && hasValue) {
            datatype = argv[++i];
        } else if (arg == "--compression" && hasValue) {
            compression = argv[++i];
        } else if (arg == "--threads" && hasValue) {
            if (!parseCount(argv[++i], threads)) {
                std::fprintf(stderr, "invalid thread count: %s\n", argv[i]);
                return 2;
            }
        } else if (arg == "--write" && hasValue) {
            onlyWrites.push_back(argv[++i]);
        } else {
            usage();
            return 2;
        }
    }

    // --- project: run its Write nodes ---
    if (positional.size() == 1) {
        if (!datatype.empty() || !compression.empty()) {
            std::fprintf(stderr, "--datatype and --compression only apply to the <input> <output> form; "
                                 "set them on the Write nodes in the project\n");
            return 2;
        }
        comp::Graph graph;
        try {
            graph = comp::loadProject(positional[0]);
        } catch (const comp::Error& e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 2;
        }
        if (!framesGiven) {  // the project's own frame range
            first = graph.settings().firstFrame;
            last = graph.settings().lastFrame;
        }

        std::vector<const comp::WriteNode*> writes;
        if (onlyWrites.empty()) {
            for (const auto& n : graph.nodes())
                if (auto* w = dynamic_cast<const comp::WriteNode*>(n.get())) writes.push_back(w);
            if (writes.empty()) {
                std::fprintf(stderr, "%s: the project has no Write nodes\n", positional[0].c_str());
                return 2;
            }
        } else {
            for (const auto& name : onlyWrites) {
                const comp::Node* n = graph.find(name);
                auto* w = dynamic_cast<const comp::WriteNode*>(n);
                if (!w) {
                    std::fprintf(stderr, "%s: %s\n", positional[0].c_str(),
                                 (n ? name + " is a " + n->typeName() + " node, not a Write"
                                    : "no node named '" + name + "'").c_str());
                    return 2;
                }
                writes.push_back(w);
            }
        }
        if (!checkFrameTokens(writes, first, last)) return 2;
        return renderFrames(writes, first, last, threads, true) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    // --- quick conversion: Read -> Write ---
    if (positional.size() != 2 || !onlyWrites.empty()) {
        usage();
        return 2;
    }
    const std::string& input = positional[0];
    const std::string& output = positional[1];

    comp::Graph graph;
    auto& read = graph.create<comp::ReadNode>("Read1");
    read.setParam("file", input);
    if (comp::hasFrameToken(input)) {
        read.setParam("first", first);
        read.setParam("last", last);
    }

    auto& write = graph.create<comp::WriteNode>("Write1");
    write.setParam("file", output);
    write.setParam("datatype", datatype.empty() ? std::string("half") : datatype);
    if (!compression.empty()) write.setParam("compression", compression);
    graph.connect(read, write, 0);

    if (!checkFrameTokens({&write}, first, last)) return 2;
    return renderFrames({&write}, first, last, threads, false) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
