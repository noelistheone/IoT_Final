#pragma once
#include <algorithm>
#include <cstring>
#include <string_view>

namespace aircue {
// Include the newline on the wire without growing the serialized JSON string.
inline size_t ble_json_fragment(std::string_view json, size_t offset, char* out, size_t capacity) noexcept {
    if (!capacity || offset > json.size()) return 0;
    size_t count = std::min(capacity, json.size() - offset);
    if (count) std::memcpy(out, json.data() + offset, count);
    if (count < capacity) out[count++] = '\n';
    return count;
}
}
