#pragma once

#include <optional>
#include <string>
#include <utility>

namespace wavecairn {

enum class ErrorCode {
    ok = 0,
    truncated,
    invalid_magic,
    unsupported_version,
    invalid_length,
    invalid_checksum,
    invalid_value,
    duplicate_id,
    missing_reference,
    state_conflict,
    resource_limit,
    io_error,
    internal_error,
};

class Status {
public:
    Status() = default;
    Status(ErrorCode code, std::string message);

    static Status success();
    static Status failure(ErrorCode code, std::string message);

    bool ok() const noexcept;
    explicit operator bool() const noexcept;
    ErrorCode code() const noexcept;
    const std::string& message() const noexcept;

private:
    ErrorCode code_{ErrorCode::ok};
    std::string message_;
};

template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)), status_(Status::success()) {}
    Result(Status status) : status_(std::move(status)) {}

    bool ok() const noexcept { return status_.ok(); }
    explicit operator bool() const noexcept { return ok(); }
    const Status& status() const noexcept { return status_; }
    T& value() & { return *value_; }
    const T& value() const & { return *value_; }
    T&& value() && { return std::move(*value_); }

private:
    std::optional<T> value_;
    Status status_;
};

}  // namespace wavecairn
