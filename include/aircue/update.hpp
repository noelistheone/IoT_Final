#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace aircue {
// ESP app descriptions contain a 32-byte version field. Accept canonical numeric
// major.minor.patch releases only, validating every component before comparison.
inline bool parse_release_version(const char* text, std::array<uint16_t, 3>& parts) {
    if (!text) return false;
    size_t position = 0;
    for (size_t part = 0; part < parts.size(); ++part) {
        size_t start = position;
        unsigned value = 0;
        while (position < 32 && text[position] >= '0' && text[position] <= '9') {
            value = value * 10 + unsigned(text[position++] - '0');
            if (value > 9999) return false;
        }
        if (position == start || position == 32 || (position - start > 1 && text[start] == '0')) return false;
        parts[part] = static_cast<uint16_t>(value);
        if (part + 1 == parts.size()) return text[position] == '\0';
        if (text[position++] != '.') return false;
    }
    return false;
}

inline bool newer_release_version(const char* candidate, const char* current) {
    std::array<uint16_t, 3> next{}, previous{};
    return parse_release_version(candidate, next) && parse_release_version(current, previous) && next > previous;
}
}
