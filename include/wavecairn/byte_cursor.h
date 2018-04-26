#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wavecairn {

class ByteCursor {
public:
    ByteCursor(const std::uint8_t* data, std::size_t size) noexcept;
    explicit ByteCursor(std::span<const std::uint8_t> data) noexcept;

    std::size_t size() const noexcept;
    std::size_t offset() const noexcept;
    std::size_t remaining() const noexcept;
    bool empty() const noexcept;
    bool can_read(std::size_t count) const noexcept;

    std::optional<std::uint8_t> read_u8() noexcept;
    std::optional<std::uint16_t> read_u16_le() noexcept;
    std::optional<std::uint32_t> read_u24_le() noexcept;
    std::optional<std::uint32_t> read_u32_le() noexcept;
    std::optional<std::uint64_t> read_u64_le() noexcept;
    std::optional<std::int16_t> read_i16_le() noexcept;
    std::optional<std::int32_t> read_i32_le() noexcept;
    std::optional<float> read_f32_le() noexcept;
    std::optional<double> read_f64_le() noexcept;
    std::optional<std::uint64_t> read_varuint() noexcept;
    std::optional<std::int64_t> read_varint() noexcept;

    std::optional<std::span<const std::uint8_t>> read_bytes(std::size_t count) noexcept;
    std::optional<std::string> read_string8(std::size_t maximum = 255);
    std::optional<std::string> read_string16(std::size_t maximum = 65535);

    bool skip(std::size_t count) noexcept;
    bool seek(std::size_t offset) noexcept;
    std::span<const std::uint8_t> remaining_span() const noexcept;
    ByteCursor slice(std::size_t count, bool* ok = nullptr) noexcept;

private:
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
    std::size_t offset_{0};
};

class ByteWriter {
public:
    void reserve(std::size_t amount);
    void write_u8(std::uint8_t value);
    void write_u16_le(std::uint16_t value);
    void write_u24_le(std::uint32_t value);
    void write_u32_le(std::uint32_t value);
    void write_u64_le(std::uint64_t value);
    void write_i16_le(std::int16_t value);
    void write_i32_le(std::int32_t value);
    void write_f32_le(float value);
    void write_f64_le(double value);
    void write_varuint(std::uint64_t value);
    void write_varint(std::int64_t value);
    void write_bytes(std::span<const std::uint8_t> bytes);
    void write_string8(std::string_view text);
    void write_string16(std::string_view text);
    void patch_u16_le(std::size_t offset, std::uint16_t value);
    void patch_u32_le(std::size_t offset, std::uint32_t value);

    const std::vector<std::uint8_t>& bytes() const noexcept;
    std::vector<std::uint8_t> take();
    std::size_t size() const noexcept;
    void clear();

private:
    std::vector<std::uint8_t> bytes_;
};

}  // namespace wavecairn
