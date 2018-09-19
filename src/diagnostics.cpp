#include "wavecairn/diagnostics.h"

#include <algorithm>
#include <sstream>

namespace wavecairn {
namespace {

std::string escape_json(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const unsigned char ch : input) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(ch >> 4) & 0x0f]);
                    out.push_back(hex[ch & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    return out;
}

}  // namespace

void Diagnostics::add(Severity severity,
                      std::uint32_t transaction,
                      std::string component,
                      std::string code,
                      std::string message) {
    entries_.push_back(Diagnostic{
        severity,
        transaction,
        std::move(component),
        std::move(code),
        std::move(message),
    });
}

void Diagnostics::trace(std::uint32_t transaction,
                        std::string_view component,
                        std::string_view code,
                        std::string message) {
    add(Severity::trace, transaction, std::string(component), std::string(code), std::move(message));
}

void Diagnostics::info(std::uint32_t transaction,
                       std::string_view component,
                       std::string_view code,
                       std::string message) {
    add(Severity::info, transaction, std::string(component), std::string(code), std::move(message));
}

void Diagnostics::warning(std::uint32_t transaction,
                          std::string_view component,
                          std::string_view code,
                          std::string message) {
    add(Severity::warning, transaction, std::string(component), std::string(code), std::move(message));
}

void Diagnostics::error(std::uint32_t transaction,
                        std::string_view component,
                        std::string_view code,
                        std::string message) {
    add(Severity::error, transaction, std::string(component), std::string(code), std::move(message));
}

std::size_t Diagnostics::size() const noexcept { return entries_.size(); }

bool Diagnostics::empty() const noexcept { return entries_.empty(); }

bool Diagnostics::has_errors() const noexcept {
    return std::any_of(entries_.begin(), entries_.end(), [](const Diagnostic& item) {
        return item.severity == Severity::error;
    });
}

std::size_t Diagnostics::count(Severity severity) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        entries_.begin(), entries_.end(), [severity](const Diagnostic& item) {
            return item.severity == severity;
        }));
}

const std::vector<Diagnostic>& Diagnostics::entries() const noexcept { return entries_; }

std::vector<Diagnostic> Diagnostics::take() {
    auto result = std::move(entries_);
    entries_.clear();
    return result;
}

void Diagnostics::clear() noexcept { entries_.clear(); }

std::string Diagnostics::to_text() const {
    std::ostringstream out;
    for (const auto& item : entries_) {
        out << severity_name(item.severity) << " [" << item.transaction << "] "
            << item.component << ":" << item.code << ": " << item.message << '\n';
    }
    return out.str();
}

std::string Diagnostics::to_json() const {
    std::ostringstream out;
    out << '[';
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        const auto& item = entries_[index];
        out << "{\"severity\":\"" << severity_name(item.severity)
            << "\",\"transaction\":" << item.transaction
            << ",\"component\":\"" << escape_json(item.component)
            << "\",\"code\":\"" << escape_json(item.code)
            << "\",\"message\":\"" << escape_json(item.message) << "\"}";
    }
    out << ']';
    return out.str();
}

std::string_view severity_name(Severity severity) noexcept {
    switch (severity) {
        case Severity::trace: return "trace";
        case Severity::info: return "info";
        case Severity::warning: return "warning";
        case Severity::error: return "error";
    }
    return "unknown";
}

}  // namespace wavecairn
