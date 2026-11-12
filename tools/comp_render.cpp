// comp_render: minimal command-line renderer.
//
//   comp_render <input> <output> [--frames A-B] [--datatype half|float|uint8|uint16]
//               [--compression NAME]
//
// Example:
//   comp_render plates/shot.####.exr out/shot.####.exr --frames 1001-1100 --compression zip
//
// For now this wires a single Read -> Write. Once there is a project file
// format, it will load a graph and execute its Write nodes instead.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "comp/core/FramePattern.h"
#include "comp/engine/Engine.h"
#include "comp/graph/Graph.h"
#include "comp/nodes/ReadNode.h"
#include "comp/nodes/WriteNode.h"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: comp_render <input> <output> [--frames A-B] "
                 "[--datatype half|float|uint8|uint16] [--compression NAME]\n");
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }

    const std::string input = argv[1];
    const std::string output = argv[2];
    int first = 1, last = 1;
    std::string datatype = "half";
    std::string compression;

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--frames" && hasValue) {
            if (!parseRange(argv[++i], first, last)) {
                std::fprintf(stderr, "invalid frame range: %s\n", argv[i]);
                return 2;
            }
        } else if (arg == "--datatype" && hasValue) {
            datatype = argv[++i];
        } else if (arg == "--compression" && hasValue) {
            compression = argv[++i];
        } else {
            usage();
            return 2;
        }
    }

    if (first != last && !comp::hasFrameToken(output)) {
        std::fprintf(stderr, "output needs a frame token (e.g. ####) to render a range\n");
        return 2;
    }

    comp::Graph graph;
    auto& read = graph.create<comp::ReadNode>("Read1");
    read.setParam("file", input);
    if (comp::hasFrameToken(input)) {
        read.setParam("first", first);
        read.setParam("last", last);
    }

    auto& write = graph.create<comp::WriteNode>("Write1");
    write.setParam("file", output);
    write.setParam("datatype", datatype);
    if (!compression.empty()) write.setParam("compression", compression);
    graph.connect(read, write, 0);

    comp::Engine engine;
    int failures = 0;
    for (int f = first; f <= last; ++f) {
        try {
            const std::string written = write.execute(engine, {static_cast<double>(f)});
            std::printf("frame %d -> %s\n", f, written.c_str());
        } catch (const comp::Error& e) {
            std::fprintf(stderr, "frame %d failed: %s\n", f, e.what());
            ++failures;
        }
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
