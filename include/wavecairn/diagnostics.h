#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wavecairn {

enum class Severity : std::uint8_t {
    trace,
    info,
    warning,
    error,
};

struct Diagnostic {
    Severity severity{Severity::info};
    std::uint32_t transaction{0};
    std::string component;
    std::string code;
    std::string message;
};

class Diagnostics {
public:
    void add(Severity severity,
             std::uint32_t transaction,
             std::string component,
             std::string code,
             std::string message);

    void trace(std::uint32_t transaction,
               std::string_view component,
               std::string_view code,
               std::string message);
    void info(std::uint32_t transaction,
              std::string_view component,
              std::string_view code,
              std::string message);
    void warning(std::uint32_t transaction,
                 std::string_view component,
                 std::string_view code,
                 std::string message);
    void error(std::uint32_t transaction,
               std::string_view component,
               std::string_view code,
               std::string message);

    std::size_t size() const noexcept;
    bool empty() const noexcept;
    bool has_errors() const noexcept;
    std::size_t count(Severity severity) const noexcept;
    const std::vector<Diagnostic>& entries() const noexcept;
    std::vector<Diagnostic> take();
    void clear() noexcept;

    std::string to_text() const;
    std::string to_json() const;

private:
    std::vector<Diagnostic> entries_;
};

std::string_view severity_name(Severity severity) noexcept;

}  // namespace wavecairn
