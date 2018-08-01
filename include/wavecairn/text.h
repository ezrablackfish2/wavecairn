#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wavecairn {

bool is_valid_utf8(std::string_view text) noexcept;
std::string replace_invalid_utf8(std::string_view text);
std::string trim(std::string_view text);
std::string ascii_lower(std::string_view text);
std::string ascii_upper(std::string_view text);
std::string collapse_spaces(std::string_view text);
std::string sanitize_label(std::string_view text, std::size_t maximum = 96);
std::string normalize_path(std::string_view path);
std::string join_path(std::string_view left, std::string_view right);
bool path_is_relative(std::string_view path) noexcept;
bool path_has_parent_escape(std::string_view path) noexcept;
std::vector<std::string> split(std::string_view text, char delimiter, bool keep_empty = false);
std::string join(const std::vector<std::string>& values, std::string_view delimiter);
std::optional<std::int64_t> parse_integer(std::string_view text, int base = 10);
std::optional<double> parse_decimal(std::string_view text);
std::string format_decimal(double value, unsigned precision = 3);
std::string percent_encode(std::string_view text);
std::string percent_decode(std::string_view text);

}  // namespace wavecairn
