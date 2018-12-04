#include "wavecairn/text.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace wavecairn {
namespace {

bool is_continuation(unsigned char ch) noexcept { return (ch & 0xc0U) == 0x80U; }

int hex_value(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first < 0x80U) {
            ++index;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t value = 0;
        if ((first & 0xe0U) == 0xc0U) { count = 2; value = first & 0x1fU; }
        else if ((first & 0xf0U) == 0xe0U) { count = 3; value = first & 0x0fU; }
        else if ((first & 0xf8U) == 0xf0U) { count = 4; value = first & 0x07U; }
        else return false;
        if (index + count > text.size()) return false;
        for (std::size_t tail = 1; tail < count; ++tail) {
            const auto ch = static_cast<unsigned char>(text[index + tail]);
            if (!is_continuation(ch)) return false;
            value = (value << 6U) | (ch & 0x3fU);
        }
        if ((count == 2 && value < 0x80U) ||
            (count == 3 && value < 0x800U) ||
            (count == 4 && value < 0x10000U) ||
            value > 0x10ffffU ||
            (value >= 0xd800U && value <= 0xdfffU)) {
            return false;
        }
        index += count;
    }
    return true;
}

std::string replace_invalid_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first < 0x80U) {
            out.push_back(static_cast<char>(first));
            ++index;
            continue;
        }
        std::size_t count = 0;
        if ((first & 0xe0U) == 0xc0U) count = 2;
        else if ((first & 0xf0U) == 0xe0U) count = 3;
        else if ((first & 0xf8U) == 0xf0U) count = 4;
        bool valid = count != 0 && index + count <= text.size();
        for (std::size_t tail = 1; valid && tail < count; ++tail) {
            valid = is_continuation(static_cast<unsigned char>(text[index + tail]));
        }
        if (!valid) {
            out += "\xef\xbf\xbd";
            ++index;
        } else {
            out.append(text.substr(index, count));
            index += count;
        }
    }
    return out;
}

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::string ascii_lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch);
    });
    return result;
}

std::string ascii_upper(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - ('a' - 'A')) : static_cast<char>(ch);
    });
    return result;
}

std::string collapse_spaces(std::string_view text) {
    std::string out;
    bool pending_space = false;
    for (const unsigned char ch : text) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            pending_space = !out.empty();
        } else {
            if (pending_space) out.push_back(' ');
            pending_space = false;
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

std::string sanitize_label(std::string_view text, std::size_t maximum) {
    auto cleaned = collapse_spaces(trim(replace_invalid_utf8(text)));
    for (char& ch : cleaned) {
        if (static_cast<unsigned char>(ch) < 0x20U || ch == '/' || ch == '\\') ch = '_';
    }
    if (cleaned.size() > maximum) {
        cleaned.resize(maximum);
        while (!cleaned.empty() && (static_cast<unsigned char>(cleaned.back()) & 0xc0U) == 0x80U) {
            cleaned.pop_back();
        }
    }
    return cleaned;
}

std::string normalize_path(std::string_view path) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : path) {
        if (ch == '/' || ch == '\\') {
            if (!current.empty() && current != ".") {
                if (current == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
                else parts.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty() && current != ".") {
        if (current == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
        else parts.push_back(current);
    }
    return join(parts, "/");
}

std::string join_path(std::string_view left, std::string_view right) {
    if (left.empty()) return normalize_path(right);
    if (right.empty()) return normalize_path(left);
    return normalize_path(std::string(left) + "/" + std::string(right));
}

bool path_is_relative(std::string_view path) noexcept {
    if (path.empty()) return true;
    if (path.front() == '/' || path.front() == '\\') return false;
    return !(path.size() >= 2 && ((path[0] >= 'A' && path[0] <= 'Z') ||
                                 (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':');
}

bool path_has_parent_escape(std::string_view path) noexcept {
    int depth = 0;
    std::string current;
    for (std::size_t index = 0; index <= path.size(); ++index) {
        const char ch = index == path.size() ? '/' : path[index];
        if (ch == '/' || ch == '\\') {
            if (current == "..") {
                if (depth == 0) return true;
                --depth;
            } else if (!current.empty() && current != ".") {
                ++depth;
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    return false;
}

std::vector<std::string> split(std::string_view text, char delimiter, bool keep_empty) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(delimiter, start);
        const auto part = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        if (keep_empty || !part.empty()) result.emplace_back(part);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

std::string join(const std::vector<std::string>& values, std::string_view delimiter) {
    std::string result;
    std::size_t size = 0;
    for (const auto& value : values) size += value.size();
    size += delimiter.size() * (values.empty() ? 0 : values.size() - 1);
    result.reserve(size);
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) result.append(delimiter);
        result.append(values[index]);
    }
    return result;
}

std::optional<std::int64_t> parse_integer(std::string_view text, int base) {
    auto cleaned = trim(text);
    if (cleaned.empty()) return std::nullopt;
    std::int64_t value = 0;
    const auto result = std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), value, base);
    if (result.ec != std::errc{} || result.ptr != cleaned.data() + cleaned.size()) return std::nullopt;
    return value;
}

std::optional<double> parse_decimal(std::string_view text) {
    auto cleaned = trim(text);
    if (cleaned.empty()) return std::nullopt;
    char* end = nullptr;
    const auto value = std::strtod(cleaned.c_str(), &end);
    if (end != cleaned.c_str() + cleaned.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

std::string format_decimal(double value, unsigned precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    auto result = out.str();
    while (result.size() > 1 && result.back() == '0') result.pop_back();
    if (!result.empty() && result.back() == '.') result.pop_back();
    return result;
}

std::string percent_encode(std::string_view text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char ch : text) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (safe) out.push_back(static_cast<char>(ch));
        else {
            out.push_back('%');
            out.push_back(hex[(ch >> 4U) & 0x0fU]);
            out.push_back(hex[ch & 0x0fU]);
        }
    }
    return out;
}

std::string percent_decode(std::string_view text) {
    std::string out;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            const auto high = hex_value(text[index + 1]);
            const auto low = hex_value(text[index + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        out.push_back(text[index]);
    }
    return out;
}

}  // namespace wavecairn
