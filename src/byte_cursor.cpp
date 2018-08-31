#include "wavecairn/byte_cursor.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace wavecairn {

ByteCursor::ByteCursor(const std::uint8_t* data, std::size_t size) noexcept
    : data_(data), size_(size) {}

ByteCursor::ByteCursor(std::span<const std::uint8_t> data) noexcept
    : ByteCursor(data.data(), data.size()) {}

std::size_t ByteCursor::size() const noexcept { return size_; }
std::size_t ByteCursor::offset() const noexcept { return offset_; }
std::size_t ByteCursor::remaining() const noexcept { return size_ - offset_; }
bool ByteCursor::empty() const noexcept { return offset_ == size_; }

bool ByteCursor::can_read(std::size_t count) const noexcept {
    return count <= remaining();
}

std::optional<std::uint8_t> ByteCursor::read_u8() noexcept {
    if (!can_read(1)) return std::nullopt;
    return data_[offset_++];
}

std::optional<std::uint16_t> ByteCursor::read_u16_le() noexcept {
    if (!can_read(2)) return std::nullopt;
    const auto result = static_cast<std::uint16_t>(data_[offset_]) |
                        (static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
    offset_ += 2;
    return result;
}

std::optional<std::uint32_t> ByteCursor::read_u24_le() noexcept {
    if (!can_read(3)) return std::nullopt;
    const auto result = static_cast<std::uint32_t>(data_[offset_]) |
                        (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8) |
                        (static_cast<std::uint32_t>(data_[offset_ + 2]) << 16);
    offset_ += 3;
    return result;
}

std::optional<std::uint32_t> ByteCursor::read_u32_le() noexcept {
    if (!can_read(4)) return std::nullopt;
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        result |= static_cast<std::uint32_t>(data_[offset_++]) << shift;
    }
    return result;
}

std::optional<std::uint64_t> ByteCursor::read_u64_le() noexcept {
    if (!can_read(8)) return std::nullopt;
    std::uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        result |= static_cast<std::uint64_t>(data_[offset_++]) << shift;
    }
    return result;
}

std::optional<std::int16_t> ByteCursor::read_i16_le() noexcept {
    const auto value = read_u16_le();
    if (!value) return std::nullopt;
    return static_cast<std::int16_t>(*value);
}

std::optional<std::int32_t> ByteCursor::read_i32_le() noexcept {
    const auto value = read_u32_le();
    if (!value) return std::nullopt;
    return static_cast<std::int32_t>(*value);
}

std::optional<float> ByteCursor::read_f32_le() noexcept {
    const auto bits = read_u32_le();
    if (!bits) return std::nullopt;
    return std::bit_cast<float>(*bits);
}

std::optional<double> ByteCursor::read_f64_le() noexcept {
    const auto bits = read_u64_le();
    if (!bits) return std::nullopt;
    return std::bit_cast<double>(*bits);
}

std::optional<std::uint64_t> ByteCursor::read_varuint() noexcept {
    std::uint64_t value = 0;
    unsigned shift = 0;
    for (unsigned count = 0; count < 10; ++count) {
        const auto byte = read_u8();
        if (!byte) return std::nullopt;
        const auto payload = static_cast<std::uint64_t>(*byte & 0x7fU);
        if (shift >= 64 && payload != 0) return std::nullopt;
        value |= payload << shift;
        if ((*byte & 0x80U) == 0) return value;
        shift += 7;
    }
    return std::nullopt;
}

std::optional<std::int64_t> ByteCursor::read_varint() noexcept {
    const auto encoded = read_varuint();
    if (!encoded) return std::nullopt;
    const auto sign = -static_cast<std::int64_t>(*encoded & 1U);
    return static_cast<std::int64_t>((*encoded >> 1U) ^ static_cast<std::uint64_t>(sign));
}

std::optional<std::span<const std::uint8_t>> ByteCursor::read_bytes(std::size_t count) noexcept {
    if (!can_read(count)) return std::nullopt;
    const auto result = std::span<const std::uint8_t>(data_ + offset_, count);
    offset_ += count;
    return result;
}

std::optional<std::string> ByteCursor::read_string8(std::size_t maximum) {
    const auto length = read_u8();
    if (!length || *length > maximum) return std::nullopt;
    const auto data = read_bytes(*length);
    if (!data) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(data->data()), data->size());
}

std::optional<std::string> ByteCursor::read_string16(std::size_t maximum) {
    const auto length = read_u16_le();
    if (!length || *length > maximum) return std::nullopt;
    const auto data = read_bytes(*length);
    if (!data) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(data->data()), data->size());
}

bool ByteCursor::skip(std::size_t count) noexcept {
    if (!can_read(count)) return false;
    offset_ += count;
    return true;
}

bool ByteCursor::seek(std::size_t offset) noexcept {
    if (offset > size_) return false;
    offset_ = offset;
    return true;
}

std::span<const std::uint8_t> ByteCursor::remaining_span() const noexcept {
    return std::span<const std::uint8_t>(data_ + offset_, remaining());
}

ByteCursor ByteCursor::slice(std::size_t count, bool* ok) noexcept {
    if (!can_read(count)) {
        if (ok) *ok = false;
        return ByteCursor(nullptr, 0);
    }
    ByteCursor result(data_ + offset_, count);
    offset_ += count;
    if (ok) *ok = true;
    return result;
}

void ByteWriter::reserve(std::size_t amount) { bytes_.reserve(amount); }
void ByteWriter::write_u8(std::uint8_t value) { bytes_.push_back(value); }

void ByteWriter::write_u16_le(std::uint16_t value) {
    bytes_.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes_.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void ByteWriter::write_u24_le(std::uint32_t value) {
    for (unsigned shift = 0; shift < 24; shift += 8) {
        bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void ByteWriter::write_u32_le(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void ByteWriter::write_u64_le(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void ByteWriter::write_i16_le(std::int16_t value) { write_u16_le(static_cast<std::uint16_t>(value)); }
void ByteWriter::write_i32_le(std::int32_t value) { write_u32_le(static_cast<std::uint32_t>(value)); }
void ByteWriter::write_f32_le(float value) { write_u32_le(std::bit_cast<std::uint32_t>(value)); }
void ByteWriter::write_f64_le(double value) { write_u64_le(std::bit_cast<std::uint64_t>(value)); }

void ByteWriter::write_varuint(std::uint64_t value) {
    while (value >= 0x80U) {
        write_u8(static_cast<std::uint8_t>((value & 0x7fU) | 0x80U));
        value >>= 7U;
    }
    write_u8(static_cast<std::uint8_t>(value));
}

void ByteWriter::write_varint(std::int64_t value) {
    const auto encoded = (static_cast<std::uint64_t>(value) << 1U) ^
                         static_cast<std::uint64_t>(value >> 63U);
    write_varuint(encoded);
}

void ByteWriter::write_bytes(std::span<const std::uint8_t> bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::write_string8(std::string_view text) {
    const auto length = std::min<std::size_t>(text.size(), 255);
    write_u8(static_cast<std::uint8_t>(length));
    write_bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), length));
}

void ByteWriter::write_string16(std::string_view text) {
    const auto length = std::min<std::size_t>(text.size(), 65535);
    write_u16_le(static_cast<std::uint16_t>(length));
    write_bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), length));
}

void ByteWriter::patch_u16_le(std::size_t offset, std::uint16_t value) {
    if (offset + 2 > bytes_.size()) throw std::out_of_range("patch_u16_le");
    bytes_[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes_[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void ByteWriter::patch_u32_le(std::size_t offset, std::uint32_t value) {
    if (offset + 4 > bytes_.size()) throw std::out_of_range("patch_u32_le");
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes_[offset + shift / 8] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
    }
}

const std::vector<std::uint8_t>& ByteWriter::bytes() const noexcept { return bytes_; }
std::vector<std::uint8_t> ByteWriter::take() { return std::move(bytes_); }
std::size_t ByteWriter::size() const noexcept { return bytes_.size(); }
void ByteWriter::clear() { bytes_.clear(); }

}  // namespace wavecairn
