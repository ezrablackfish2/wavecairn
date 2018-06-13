#pragma once

#include "wavecairn/byte_cursor.h"
#include "wavecairn/ids.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace wavecairn {

inline EntityId read_entity_id(ByteCursor& cursor) noexcept {
    const auto value = cursor.read_u64_le();
    return EntityId(value.value_or(0));
}

inline std::string read_label(ByteCursor& cursor, std::size_t maximum = 96) {
    const auto text = cursor.read_string8(maximum);
    return text.value_or(std::string{});
}

inline double finite_or(double value, double fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

inline float bounded(float value, float minimum, float maximum, float fallback = 0.0F) noexcept {
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, minimum, maximum);
}

inline std::int64_t positive_or(std::int64_t value, std::int64_t fallback) noexcept {
    return value > 0 ? value : fallback;
}

}  // namespace wavecairn
