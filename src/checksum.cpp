#include "wavecairn/checksum.h"

#include <array>

namespace wavecairn {
namespace {

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        std::uint32_t value = index;
        for (unsigned bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0 ? 0xedb88320U ^ (value >> 1U) : value >> 1U;
        }
        table[index] = value;
    }
    return table;
}

constexpr auto crc_table = make_crc_table();

}  // namespace

std::uint16_t fletcher16(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t sum1 = 0xffU;
    std::uint32_t sum2 = 0xffU;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto block = bytes.size() - offset > 20 ? 20 : bytes.size() - offset;
        for (std::size_t index = 0; index < block; ++index) {
            sum1 += bytes[offset + index];
            sum2 += sum1;
        }
        sum1 = (sum1 & 0xffU) + (sum1 >> 8U);
        sum2 = (sum2 & 0xffU) + (sum2 >> 8U);
        offset += block;
    }
    sum1 = (sum1 & 0xffU) + (sum1 >> 8U);
    sum2 = (sum2 & 0xffU) + (sum2 >> 8U);
    return static_cast<std::uint16_t>((sum2 << 8U) | sum1);
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t value = 0xffffffffU;
    for (const auto byte : bytes) {
        value = crc_table[(value ^ byte) & 0xffU] ^ (value >> 8U);
    }
    return value ^ 0xffffffffU;
}

std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t value = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

std::uint64_t fnv1a64(std::string_view text) noexcept {
    return fnv1a64(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t combine_hash(std::uint64_t left, std::uint64_t right) noexcept {
    return mix64(left ^ (right + 0x9e3779b97f4a7c15ULL + (left << 6U) + (left >> 2U)));
}

}  // namespace wavecairn
