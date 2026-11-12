#include "comp/core/FramePattern.h"

#include <cctype>
#include <cstdio>
#include <optional>

namespace comp {
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

}  // namespace comp
