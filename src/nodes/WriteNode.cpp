#include "comp/nodes/WriteNode.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <system_error>

#include <OpenImageIO/imageio.h>

#include "comp/core/FramePattern.h"

namespace comp {
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
    const int frame = static_cast<int>(std::floor(ctx.frame + 0.5));
    const std::string path = expandFramePattern(param<std::string>("file"), frame);

    // Asking about this node (not its input) reports an unconnected input by our name.
    const ImageInfo info = engine.info(*this, ctx);
    if (info.channels.empty()) throw Error(name() + ": input has no channels");

    // Formats can't store an empty image; write a single black pixel instead.
    Box window = info.dataWindow;
    if (window.empty())
        window = {info.displayWindow.x0, info.displayWindow.y0, info.displayWindow.x0 + 1,
                  info.displayWindow.y0 + 1};

    // Render the input directly; going through this node would only add a copy.
    const Image img = engine.render(requireInput(0), ctx, window);

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
    spec.attribute("Software", "comp");

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

    const bool ok = out->open(tmpPath, spec) &&
                    out->write_image(OIIO::TypeDesc::FLOAT, img.data()) && out->close();
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

}  // namespace comp
