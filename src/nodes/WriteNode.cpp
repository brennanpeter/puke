#include "puke/nodes/WriteNode.h"

#include "puke/engine/Snapshot.h"

#include "Specs.h"

#include "puke/color/ColorConfig.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <system_error>

#include <OpenImageIO/imageio.h>

#include "puke/core/FramePattern.h"

namespace puke {
namespace {

namespace OIIO = OIIO_NAMESPACE;
namespace fs = std::filesystem;

OIIO::TypeDesc parseDataType(const std::string& nodeName, const std::string& s) {
    if (s == "float") return OIIO::TypeDesc::FLOAT;
    if (s == "half") return OIIO::TypeDesc::HALF;
    if (s == "uint8") return OIIO::TypeDesc::UINT8;
    if (s == "uint16") return OIIO::TypeDesc::UINT16;
    throw Error(nodeName + ": unknown datatype '" + s + "' (use float, half, uint8 or uint16)");
}

int alphaIndex(const ChannelList& channels) {
    for (int i = 0; i < static_cast<int>(channels.size()); ++i)
        if (channels[i] == "A" || channels[i] == "alpha") return i;
    return -1;
}

}  // namespace

std::string WriteNode::execute(Engine& engine, const RenderContext& ctx) const {
    return execute(engine, Snapshot::of(*this), ctx);  // reports an unconnected input by our name
}

std::string WriteNode::execute(Engine& engine, const Snapshot& snapshot, const RenderContext& ctx) const {
    if (snapshot.output().node != this) throw Error(name() + ": the snapshot is of another node");
    // Our own parameters too, as they were, at this frame.
    const auto& self = snapshot.output();
    const auto params = self.curves ? Node::evaluate(self.params, *self.curves, ctx.frame) : self.params;
    ParamScope scope(*this, *params);

    const int frame = static_cast<int>(std::floor(ctx.frame + 0.5));
    const std::string path = expandFramePattern(param<std::string>("file"), frame);

    const ImageInfo info = engine.info(snapshot, ctx);
    if (info.channels.empty()) throw Error(name() + ": input has no channels");

    // Formats can't store an empty image; write a single black pixel instead.
    Box window = info.dataWindow;
    if (window.empty())
        window = {info.displayWindow.x0, info.displayWindow.y0, info.displayWindow.x0 + 1,
                  info.displayWindow.y0 + 1};

    // Render the input directly; going through this node would only add a copy.
    Image img = engine.render(snapshot.upstream(0), ctx, window);

    OIIO::ImageSpec spec(window.width(), window.height(), img.numChannels(),
                         parseDataType(name(), param<std::string>("datatype", "half")));
    spec.x = window.x0;
    spec.y = window.y0;
    spec.full_x = info.displayWindow.x0;
    spec.full_y = info.displayWindow.y0;
    spec.full_width = info.displayWindow.width();
    spec.full_height = info.displayWindow.height();
    spec.channelnames.assign(img.channels().begin(), img.channels().end());
    spec.alpha_channel = alphaIndex(img.channels());
    if (hasParam("compression")) spec.attribute("compression", param<std::string>("compression"));
    spec.attribute("Software", "puke");

    // Choose the format plugin from the final path's extension, but write
    // to a temporary name and rename once the file is complete.
    const fs::path finalPath(path);
    if (finalPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(finalPath.parent_path(), ec);
        if (ec) throw Error(name() + ": cannot create directory for '" + path + "': " + ec.message());
    }
    const std::string tmpPath = path + ".partial";

    auto out = OIIO::ImageOutput::create(path);
    if (!out) throw Error(name() + ": no writer for '" + path + "': " + OIIO::geterror());

    if (!out->supports("origin") && (window.x0 != 0 || window.y0 != 0)) {
        // TODO: pad or crop to the display window for formats without data windows (PNG, JPEG).
        throw Error(name() + ": format of '" + path + "' cannot store an offset data window");
    }

    bool ok = out->open(tmpPath, spec);
    if (ok) {
        // Out of the working space. "auto" goes by the data type the format
        // actually stores, which may differ from the one asked for.
        std::string colorspace = param<std::string>("colorspace", "auto");
        if (colorspace == "auto")
            colorspace = out->spec().format.is_floating_point() ? "scene_linear" : "matte_paint";
        try {
            ColorConfig::current()->convert(img, "scene_linear", colorspace, info.dataWindow);
        } catch (const Error& e) {
            out->close();
            std::error_code ec;
            fs::remove(tmpPath, ec);
            throw Error(name() + ": " + e.what());
        }
        ok = out->write_image(OIIO::TypeDesc::FLOAT, img.data()) && out->close();
    }
    if (!ok) {
        std::string err = out->geterror();
        std::error_code ec;
        fs::remove(tmpPath, ec);
        throw Error(name() + ": failed writing '" + path + "': " + err);
    }

    std::error_code ec;
    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
        const std::string msg = ec.message();
        fs::remove(tmpPath, ec);  // don't leave the temporary file behind
        throw Error(name() + ": cannot move finished file into place at '" + path + "': " + msg);
    }
    return path;
}

std::vector<ParamSpec> WriteNode::paramSpecs() const {
    using namespace specs;
    ParamSpec compression = optional(choice(
        "compression", {"none", "zip", "zips", "piz", "rle", "pxr24", "b44", "b44a", "dwaa", "dwab"}, "",
        "Passed to the file format (these are EXR's); empty: the format's default"));
    compression.openChoice = true;
    return {
        make("file", Kind::File, std::string(), "Output path; frame tokens: #### %04d @@@"),
        choice("datatype", {"float", "half", "uint8", "uint16"}, "half", "Data type in the file"),
        compression,
        colorSpace("colorspace", "auto", "Color space to write (auto: float files linear, integer files sRGB)", true),
    };
}

}  // namespace puke
