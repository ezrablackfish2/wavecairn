#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace wavecairn {

std::uint16_t fletcher16(std::span<const std::uint8_t> bytes) noexcept;
std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;
std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) noexcept;
std::uint64_t fnv1a64(std::string_view text) noexcept;
std::uint64_t mix64(std::uint64_t value) noexcept;
std::uint64_t combine_hash(std::uint64_t left, std::uint64_t right) noexcept;

}  // namespace wavecairn
