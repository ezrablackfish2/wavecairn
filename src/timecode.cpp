#include "wavecairn/timecode.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numeric>
#include <sstream>
#include <vector>

namespace wavecairn {

void Rational::normalize() noexcept {
    if (denominator == 0) {
        numerator = 0;
        denominator = 1;
        return;
    }
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    const auto divisor = std::gcd(numerator, denominator);
    if (divisor != 0) {
        numerator /= divisor;
        denominator /= divisor;
    }
}

double Rational::to_double() const noexcept {
    return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
}

double TimecodeRate::frames_per_second() const noexcept {
    return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
}

std::optional<TimecodeRate> TimecodeRate::parse(std::string_view text) {
    TimecodeRate result;
    if (text == "23.976") return TimecodeRate{24000, 1001, false};
    if (text == "24") return TimecodeRate{24, 1, false};
    if (text == "25") return TimecodeRate{25, 1, false};
    if (text == "29.97") return TimecodeRate{30000, 1001, false};
    if (text == "29.97df") return TimecodeRate{30000, 1001, true};
    if (text == "30") return TimecodeRate{30, 1, false};
    if (text == "50") return TimecodeRate{50, 1, false};
    if (text == "59.94") return TimecodeRate{60000, 1001, false};
    if (text == "59.94df") return TimecodeRate{60000, 1001, true};
    if (text == "60") return TimecodeRate{60, 1, false};

    const auto slash = text.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 0;
    const auto first = std::from_chars(text.data(), text.data() + slash, numerator);
    const auto second = std::from_chars(text.data() + slash + 1, text.data() + text.size(), denominator);
    if (first.ec != std::errc{} || second.ec != std::errc{} || numerator == 0 || denominator == 0) {
        return std::nullopt;
    }
    result.numerator = numerator;
    result.denominator = denominator;
    return result;
}

std::string Timecode::format(const TimecodeRate& rate) const {
    std::ostringstream out;
    if (negative) out << '-';
    out.fill('0');
    out.width(2); out << hours << ':';
    out.width(2); out << static_cast<unsigned>(minutes) << ':';
    out.width(2); out << static_cast<unsigned>(seconds) << (rate.drop_frame ? ';' : ':');
    out.width(2); out << static_cast<unsigned>(frames);
    return out.str();
}

std::optional<Timecode> Timecode::parse(std::string_view text, const TimecodeRate& rate) {
    Timecode result;
    if (!text.empty() && text.front() == '-') {
        result.negative = true;
        text.remove_prefix(1);
    }
    if (text.size() < 11) return std::nullopt;
    std::vector<std::uint32_t> parts;
    std::uint32_t current = 0;
    bool have_digit = false;
    for (char ch : text) {
        if (ch >= '0' && ch <= '9') {
            have_digit = true;
            current = current * 10U + static_cast<std::uint32_t>(ch - '0');
        } else if (ch == ':' || ch == ';') {
            if (!have_digit) return std::nullopt;
            parts.push_back(current);
            current = 0;
            have_digit = false;
        } else {
            return std::nullopt;
        }
    }
    if (!have_digit) return std::nullopt;
    parts.push_back(current);
    if (parts.size() != 4) return std::nullopt;
    const auto nominal = static_cast<std::uint32_t>(std::lround(rate.frames_per_second()));
    if (parts[1] >= 60 || parts[2] >= 60 || parts[3] >= nominal) return std::nullopt;
    result.hours = parts[0];
    result.minutes = static_cast<std::uint8_t>(parts[1]);
    result.seconds = static_cast<std::uint8_t>(parts[2]);
    result.frames = static_cast<std::uint8_t>(parts[3]);
    return result;
}

std::int64_t timecode_to_frames(const Timecode& value, const TimecodeRate& rate) noexcept {
    const auto nominal = static_cast<std::int64_t>(std::lround(rate.frames_per_second()));
    std::int64_t total_minutes = static_cast<std::int64_t>(value.hours) * 60 + value.minutes;
    std::int64_t frames = ((static_cast<std::int64_t>(value.hours) * 3600 +
                            static_cast<std::int64_t>(value.minutes) * 60 + value.seconds) * nominal) +
                          value.frames;
    if (rate.drop_frame && (nominal == 30 || nominal == 60)) {
        const auto dropped = nominal == 30 ? 2 : 4;
        frames -= dropped * (total_minutes - total_minutes / 10);
    }
    return value.negative ? -frames : frames;
}

Timecode frames_to_timecode(std::int64_t frames, const TimecodeRate& rate) noexcept {
    Timecode result;
    if (frames < 0) {
        result.negative = true;
        frames = -frames;
    }
    const auto nominal = static_cast<std::int64_t>(std::lround(rate.frames_per_second()));
    if (rate.drop_frame && (nominal == 30 || nominal == 60)) {
        const std::int64_t dropped = nominal == 30 ? 2 : 4;
        const std::int64_t frames_per_10_minutes = nominal * 600 - dropped * 9;
        const std::int64_t complete = frames / frames_per_10_minutes;
        const std::int64_t remainder = frames % frames_per_10_minutes;
        frames += complete * dropped * 9;
        if (remainder >= dropped) {
            frames += dropped * ((remainder - dropped) / (nominal * 60 - dropped) + 1);
        }
    }
    const auto frames_per_hour = nominal * 3600;
    result.hours = static_cast<std::uint32_t>(frames / frames_per_hour);
    frames %= frames_per_hour;
    result.minutes = static_cast<std::uint8_t>(frames / (nominal * 60));
    frames %= nominal * 60;
    result.seconds = static_cast<std::uint8_t>(frames / nominal);
    result.frames = static_cast<std::uint8_t>(frames % nominal);
    return result;
}

SamplePosition frames_to_samples(std::int64_t frames,
                                 const TimecodeRate& rate,
                                 std::uint32_t sample_rate) noexcept {
    if (rate.numerator == 0) return SamplePosition{};
    const long double samples = static_cast<long double>(frames) * sample_rate * rate.denominator /
                                static_cast<long double>(rate.numerator);
    return SamplePosition(static_cast<std::int64_t>(std::llround(samples)));
}

}  // namespace wavecairn
