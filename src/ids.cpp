#include "wavecairn/ids.h"

#include "wavecairn/checksum.h"

#include <array>
#include <charconv>
#include <iomanip>
#include <sstream>

namespace wavecairn {

std::string EntityId::to_string() const {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value_;
    return out.str();
}

std::optional<EntityId> EntityId::parse(std::string_view text) {
    if (text.empty() || text.size() > 16) return std::nullopt;
    std::uint64_t value = 0;
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto result = std::from_chars(first, last, value, 16);
    if (result.ec != std::errc{} || result.ptr != last || value == 0) return std::nullopt;
    return EntityId(value);
}

EntityId EntityId::from_name(std::string_view category, std::string_view name) {
    auto value = combine_hash(fnv1a64(category), fnv1a64(name));
    if (value == 0) value = 1;
    return EntityId(value);
}

std::size_t EntityIdHash::operator()(EntityId id) const noexcept {
    const auto value = mix64(id.value());
    if constexpr (sizeof(std::size_t) >= sizeof(value)) {
        return static_cast<std::size_t>(value);
    }
    return static_cast<std::size_t>(value ^ (value >> 32U));
}

}  // namespace wavecairn
