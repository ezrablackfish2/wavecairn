#include "wavecairn/status.h"

namespace wavecairn {

Status::Status(ErrorCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::success() { return Status{}; }

Status Status::failure(ErrorCode code, std::string message) {
    return Status(code, std::move(message));
}

bool Status::ok() const noexcept { return code_ == ErrorCode::ok; }

Status::operator bool() const noexcept { return ok(); }

ErrorCode Status::code() const noexcept { return code_; }

const std::string& Status::message() const noexcept { return message_; }

}  // namespace wavecairn
