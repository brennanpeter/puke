#include "puke/nodes/ReadNode.h"

#include "Specs.h"

#include "puke/color/ColorConfig.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "puke/core/FramePattern.h"

namespace puke {
namespace {

namespace OIIO = OIIO_NAMESPACE;

// Frames are sampled at the nearest whole frame for now.
int wholeFrame(double frame) { return static_cast<int>(std::floor(frame + 0.5)); }

struct OpenedFile {
    std::unique_ptr<OIIO::ImageInput> in;
    OIIO::ImageSpec spec;
};

OpenedFile openFile(const std::string& nodeName, const std::string& path) {
    auto in = OIIO::ImageInput::open(path);
    if (!in) throw Error(nodeName + ": cannot open '" + path + "': " + OIIO::geterror());
    OpenedFile f{std::move(in), {}};
    f.spec = f.in->spec();
    if (f.spec.deep) throw Error(nodeName + ": deep images are not supported yet: " + path);
    if (f.spec.depth > 1) throw Error(nodeName + ": volume images are not supported: " + path);
    return f;
}

ImageInfo infoFromSpec(const OIIO::ImageSpec& s) {
    ImageInfo info;
    info.dataWindow = {s.x, s.y, s.x + s.width, s.y + s.height};
    info.displayWindow = {s.full_x, s.full_y, s.full_x + s.full_width, s.full_y + s.full_height};
    info.channels.assign(s.channelnames.begin(), s.channelnames.end());
    return info;
}

}  // namespace

bool isImageFile(const std::string& path) {
    const bool known = OIIO::ImageInput::create(path) != nullptr;
    if (!known) (void)OIIO::geterror();  // don't leave the message for someone else
    return known;
}

std::string ReadNode::resolvePath(const RenderContext& ctx) const {
    const auto pattern = param<std::string>("file");
    int frame = wholeFrame(ctx.frame);

    if (hasParam("first") && hasParam("last")) {
        const int first = param<int>("first");
        const int last = param<int>("last");
        if (frame < first || frame > last) {
            const auto outside = param<std::string>("outside", "hold");
            if (outside != "hold" && outside != "black" && outside != "error")
                throw Error(name() + ": outside must be hold, black or error, not '" + outside + "'");
            if (outside == "black") return {};
            if (outside == "error")
                throw Error(name() + ": frame " + std::to_string(frame) + " is outside " +
                            std::to_string(first) + "-" + std::to_string(last));
            frame = std::clamp(frame, first, last);  // "hold"
        }
    }
    return expandFramePattern(pattern, frame);
}

ImageInfo ReadNode::fileInfo(const RenderContext& ctx) const {
    auto path = resolvePath(ctx);
    if (path.empty()) {
        // Black frame outside the range: borrow the format from the first frame.
        RenderContext firstCtx = ctx;
        firstCtx.frame = param<int>("first");
        auto info = infoFromSpec(openFile(name(), resolvePath(firstCtx)).spec);
        info.dataWindow = {};
        return info;
    }
    return infoFromSpec(openFile(name(), path).spec);
}

void ReadNode::appendHash(Hasher& h, const RenderContext& ctx) const {
    const std::string path = resolvePath(ctx);
    h.add(path);
    if (path.empty()) return;
    // If the file can't be stat'ed, hash nothing more: compute() will report
    // the real error when it tries to open it.
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec) h.add(static_cast<std::uint64_t>(size));
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (!ec) h.add(static_cast<long long>(mtime.time_since_epoch().count()));
    h.add(ColorConfig::current()->hash());
}

ImageInfo ReadNode::computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const>) const {
    ImageInfo info = fileInfo(ctx);
    info.dataWindow = info.dataWindow.scaled(ctx.scale);  // proxy: the file shrunk
    info.displayWindow = info.displayWindow.scaled(ctx.scale);
    return info;
}

Image ReadNode::compute(const RenderContext& ctx, const Box& region,
                        std::span<const Image* const> inputs) const {
    if (ctx.scale == 1) return readRegion(ctx, region);
    // Proxy: read the full-size pixels under the region, then average them
    // down (in the working space, so averaging happens in linear light).
    const Box full = region.scaled(1 / ctx.scale);
    (void)inputs;
    return downscale(readRegion(ctx, full), region, ctx.scale);
}

Image ReadNode::readRegion(const RenderContext& ctx, const Box& region) const {
    auto path = resolvePath(ctx);
    if (path.empty()) return Image(region, fileInfo(ctx).channels);  // all zeros

    OpenedFile f = openFile(name(), path);
    const ImageInfo fi = infoFromSpec(f.spec);
    const int nch = f.spec.nchannels;

    Image out(region, fi.channels);
    const Box want = region.intersect(fi.dataWindow);
    if (want.empty()) return out;

    // Read whole rows covering the wanted region from the file's data window,
    // then copy the wanted columns into the output.
    // TODO: tile-aligned reads for tiled files, and an open-file/tile cache.
    const Box rows{fi.dataWindow.x0, want.y0, fi.dataWindow.x1, want.y1};
    std::vector<float> buf(static_cast<std::size_t>(rows.area()) * nch);

    bool ok;
    if (f.spec.tile_width > 0) {
        // Tiled files: read the full image, then crop. Simple, not fast.
        buf.resize(static_cast<std::size_t>(fi.dataWindow.area()) * nch);
        ok = f.in->read_image(0, 0, 0, nch, OIIO::TypeDesc::FLOAT, buf.data());
        if (ok) {
            const std::size_t rowFloats = static_cast<std::size_t>(rows.width()) * nch;
            const std::size_t skip = static_cast<std::size_t>(want.y0 - fi.dataWindow.y0) * rowFloats;
            std::memmove(buf.data(), buf.data() + skip, rows.height() * rowFloats * sizeof(float));
        }
    } else {
        ok = f.in->read_scanlines(0, 0, want.y0, want.y1, 0, 0, nch, OIIO::TypeDesc::FLOAT,
                                  buf.data());
    }
    if (!ok) throw Error(name() + ": failed reading '" + path + "': " + f.in->geterror());

    const std::size_t copyBytes = static_cast<std::size_t>(want.width()) * nch * sizeof(float);
    for (int y = want.y0; y < want.y1; ++y) {
        const float* src = buf.data() +
                           (static_cast<std::size_t>(y - rows.y0) * rows.width() + (want.x0 - rows.x0)) * nch;
        std::memcpy(out.pixel(want.x0, y), src, copyBytes);
    }

    // Into the working space. "auto": float files are already there; integer
    // files are display-referred, so sRGB.
    std::string colorspace = param<std::string>("colorspace", "auto");
    if (colorspace == "auto") colorspace = f.spec.format.is_floating_point() ? "scene_linear" : "matte_paint";
    try {
        ColorConfig::current()->convert(out, colorspace, "scene_linear", want);
    } catch (const Error& e) {
        throw Error(name() + ": " + e.what());
    }
    return out;
}

std::vector<ParamSpec> ReadNode::paramSpecs() const {
    using namespace specs;
    return {
        make("file", Kind::File, std::string(), "Image or sequence; frame tokens: #### %04d @@@"),
        optional(make("first", Kind::Int, 1, "First frame of the sequence", 0, 2000)),
        optional(make("last", Kind::Int, 1, "Last frame of the sequence", 0, 2000)),
        choice("outside", {"hold", "black", "error"}, "hold", "Frames outside first..last"),
        colorSpace("colorspace", "auto", "The file's color space (auto: float files linear, integer files sRGB)", true),
    };
}

}  // namespace puke
