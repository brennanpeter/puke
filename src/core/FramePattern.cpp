#include "puke/core/FramePattern.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <optional>

namespace puke {
namespace {

struct Token {
    std::size_t pos;
    std::size_t len;
    int width;
};

std::optional<Token> findRun(const std::string& s, char c) {
    auto pos = s.find(c);
    if (pos == std::string::npos) return std::nullopt;
    std::size_t end = pos;
    while (end < s.size() && s[end] == c) ++end;
    return Token{pos, end - pos, static_cast<int>(end - pos)};
}

// Matches %d or %0Nd.
std::optional<Token> findPrintf(const std::string& s) {
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (s[i] != '%') continue;
        std::size_t j = i + 1;
        int width = 0;
        if (s[j] == '0') {
            ++j;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
                width = width * 10 + (s[j++] - '0');
        }
        if (j < s.size() && s[j] == 'd') return Token{i, j - i + 1, width};
    }
    return std::nullopt;
}

std::optional<Token> findToken(const std::string& s) {
    if (auto t = findRun(s, '#')) return t;
    if (auto t = findPrintf(s)) return t;
    if (auto t = findRun(s, '@')) return t;
    return std::nullopt;
}

}  // namespace

bool hasFrameToken(const std::string& pattern) { return findToken(pattern).has_value(); }

std::string expandFramePattern(const std::string& pattern, int frame) {
    auto tok = findToken(pattern);
    if (!tok) return pattern;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*d", tok->width, frame);

    std::string out = pattern;
    out.replace(tok->pos, tok->len, buf);
    return out;
}

namespace {

// Whether `digits` (an optional '-', then digits) is what a token of this
// width expands to for some frame: exactly `width` characters, or more
// with no padding zero (a frame past the padding). Width 0: any.
bool matchesWidth(const std::string& digits, int width) {
    if (width == 0) return true;
    const int n = static_cast<int>(digits.size());
    if (n == width) return true;
    const std::size_t lead = digits[0] == '-' ? 1 : 0;
    return n > width && digits[lead] != '0';
}

std::optional<int> parseFrame(const std::string& s) {
    std::size_t i = s.empty() || s[0] != '-' ? 0 : 1;
    if (i == s.size() || s.size() - i > 9) return std::nullopt;
    for (std::size_t j = i; j < s.size(); ++j)
        if (!std::isdigit(static_cast<unsigned char>(s[j]))) return std::nullopt;
    return std::stoi(s);
}

}  // namespace

std::optional<FileSequence> findFrames(const std::string& pattern) {
    namespace fs = std::filesystem;
    const fs::path path(pattern);
    const std::string name = path.filename().string();
    const auto tok = findToken(name);
    if (!tok || hasFrameToken(path.parent_path().string())) return std::nullopt;
    const std::string prefix = name.substr(0, tok->pos), suffix = name.substr(tok->pos + tok->len);
    const fs::path dir = path.has_parent_path() ? path.parent_path() : fs::path(".");

    std::optional<FileSequence> seq;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.size() <= prefix.size() + suffix.size() || n.compare(0, prefix.size(), prefix) != 0 ||
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        const std::string digits = n.substr(prefix.size(), n.size() - prefix.size() - suffix.size());
        const auto frame = parseFrame(digits);
        if (!frame || !matchesWidth(digits, tok->width) || it->is_directory(ec)) continue;
        if (!seq) seq = FileSequence{pattern, *frame, *frame, 0};
        seq->first = std::min(seq->first, *frame);
        seq->last = std::max(seq->last, *frame);
        ++seq->count;
    }
    return seq;
}

std::optional<FileSequence> sequenceOf(const std::string& file) {
    namespace fs = std::filesystem;
    const fs::path path(file);
    const std::string name = path.filename().string();
    if (hasFrameToken(name)) return std::nullopt;
    // The last run of digits before the extension.
    std::size_t end = name.rfind('.');
    if (end == std::string::npos || end == 0) end = name.size();
    while (end > 0 && !std::isdigit(static_cast<unsigned char>(name[end - 1]))) --end;
    std::size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(name[start - 1]))) --start;
    if (start == end) return std::nullopt;
    const std::string patternName = name.substr(0, start) + std::string(end - start, '#') + name.substr(end);
    auto seq = findFrames((path.parent_path() / patternName).string());
    if (!seq || seq->count < 2) return std::nullopt;
    return seq;
}

}  // namespace puke
