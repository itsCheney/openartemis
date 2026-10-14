#pragma once
// Shortest round-trip text for a double, the way
// std::to_chars(first, last, value) prints it ("0.5", "12.25", "100").
//
// Apple's libc++ only ships floating-point std::to_chars from iOS 16.3 /
// macOS 13.3, so Apple builds use an snprintf equivalent: the fewest
// significant digits that read back to the same double, in fixed or
// scientific notation, whichever is shorter (as to_chars chooses).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(__APPLE__)
#include <charconv>
#endif

namespace oa::util {

inline std::string shortest_double(double value) {
#if defined(__APPLE__)
    char buf[64];
    if (!std::isfinite(value)) {
        std::snprintf(buf, sizeof buf, "%g", value);
        return buf;
    }
    int digits = 1;
    for (; digits < 17; ++digits) {
        std::snprintf(buf, sizeof buf, "%.*e", digits - 1, value);
        if (std::strtod(buf, nullptr) == value) break;
    }
    char scientific[64];
    std::snprintf(scientific, sizeof scientific, "%.*e", digits - 1, value);
    const char* e = std::strchr(scientific, 'e');
    const int exponent = e ? std::atoi(e + 1) : 0;
    const int decimals = digits - 1 - exponent;
    char fixed[400];
    std::snprintf(fixed, sizeof fixed, "%.*f", decimals > 0 ? decimals : 0, value);
    // to_chars takes the shorter form, fixed on a tie.
    return std::strlen(fixed) <= std::strlen(scientific) ? fixed : scientific;
#else
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    if (res.ec == std::errc()) return std::string(buf, res.ptr);
    return std::to_string(value);
#endif
}

} // namespace oa::util
