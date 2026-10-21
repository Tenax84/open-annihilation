// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One spelling for every float a script prints. A NaN of either sign and the
// two infinities are written here, because C libraries spell them differently.
// A finite value uses %.14g, which the libraries this engine builds with all
// round the same way. The length is what snprintf would return.

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

const char* special_number_text(double value) {
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return std::signbit(value) ? "-inf" : "inf";
    return nullptr;
}

int write_special(char* buffer, std::size_t size, const char* text) {
    const std::size_t length = std::strlen(text);
    if (size == 0)
        return static_cast<int>(length);
    const std::size_t copied = length < size ? length : size - 1;
    if (copied > 0)
        std::memcpy(buffer, text, copied);
    buffer[copied] = '\0';
    return static_cast<int>(length);
}

} // namespace

extern "C" int oa_script_host_number_text(char* buffer, std::size_t size, double value) {
    if (const char* text = special_number_text(value))
        return write_special(buffer, size, text);
    return std::snprintf(buffer, size, "%.14g", value);
}
