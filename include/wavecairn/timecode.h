#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wavecairn {

struct Rational {
    std::int64_t numerator{0};
    std::int64_t denominator{1};

    void normalize() noexcept;
    double to_double() const noexcept;
    friend bool operator==(const Rational&, const Rational&) = default;
};

class SamplePosition {
public:
    explicit constexpr SamplePosition(std::int64_t value = 0) noexcept : value_(value) {}
    constexpr std::int64_t value() const noexcept { return value_; }
    constexpr SamplePosition operator+(std::int64_t amount) const noexcept {
        return SamplePosition(value_ + amount);
    }
    constexpr SamplePosition operator-(std::int64_t amount) const noexcept {
        return SamplePosition(value_ - amount);
    }
    friend constexpr auto operator<=>(SamplePosition, SamplePosition) noexcept = default;

private:
    std::int64_t value_{0};
};

struct TimecodeRate {
    std::uint32_t numerator{30};
    std::uint32_t denominator{1};
    bool drop_frame{false};

    double frames_per_second() const noexcept;
    static std::optional<TimecodeRate> parse(std::string_view text);
};

struct Timecode {
    bool negative{false};
    std::uint32_t hours{0};
    std::uint8_t minutes{0};
    std::uint8_t seconds{0};
    std::uint8_t frames{0};

    std::string format(const TimecodeRate& rate) const;
    static std::optional<Timecode> parse(std::string_view text, const TimecodeRate& rate);
};

std::int64_t timecode_to_frames(const Timecode& value, const TimecodeRate& rate) noexcept;
Timecode frames_to_timecode(std::int64_t frames, const TimecodeRate& rate) noexcept;
SamplePosition frames_to_samples(std::int64_t frames,
                                 const TimecodeRate& rate,
                                 std::uint32_t sample_rate) noexcept;

}  // namespace wavecairn
