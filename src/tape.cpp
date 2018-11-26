#include "wavecairn/tape.h"

#include "wavecairn/byte_cursor.h"
#include "wavecairn/checksum.h"

#include <algorithm>
#include <array>
#include <sstream>

namespace wavecairn {
namespace {

constexpr std::array<std::uint8_t, 4> tape_magic{'W', 'C', 'T', 'P'};

Status parse_failure(ErrorCode code, std::size_t offset, std::string message) {
    std::ostringstream out;
    out << "command tape at byte " << offset << ": " << message;
    return Status::failure(code, out.str());
}

}  // namespace

Result<CommandTape> parse_command_tape(std::span<const std::uint8_t> bytes,
                                       Diagnostics* diagnostics,
                                       const TapeLimits& limits) {
    if (bytes.size() > limits.maximum_bytes) {
        return Status::failure(ErrorCode::resource_limit, "command tape exceeds configured size limit");
    }
    ByteCursor cursor(bytes);
    const auto magic = cursor.read_bytes(tape_magic.size());
    if (!magic || !std::equal(magic->begin(), magic->end(), tape_magic.begin())) {
        return parse_failure(ErrorCode::invalid_magic, 0, "expected WCTP signature");
    }
    const auto version = cursor.read_u8();
    const auto flags = cursor.read_u8();
    const auto reserved = cursor.read_u16_le();
    const auto session_seed = cursor.read_u32_le();
    if (!version || !flags || !reserved || !session_seed) {
        return parse_failure(ErrorCode::truncated, cursor.offset(), "incomplete tape header");
    }
    if (*version != 3) {
        return parse_failure(ErrorCode::unsupported_version, 4, "only version 3 is supported");
    }
    if (*reserved != 0 && diagnostics) {
        diagnostics->warning(0, "tape", "reserved-bits", "nonzero reserved header field was ignored");
    }

    CommandTape tape;
    tape.header.version = *version;
    tape.header.flags = *flags;
    tape.header.session_seed = *session_seed;
    tape.commands.reserve(std::min<std::size_t>(bytes.size() / 12, 128));

    while (!cursor.empty()) {
        const auto start = cursor.offset();
        if (tape.commands.size() >= limits.maximum_commands) {
            return parse_failure(ErrorCode::resource_limit, start, "command count exceeds configured limit");
        }
        const auto opcode = cursor.read_u8();
        const auto command_flags = cursor.read_u8();
        const auto payload_size = cursor.read_u16_le();
        const auto transaction = cursor.read_u32_le();
        if (!opcode || !command_flags || !payload_size || !transaction) {
            return parse_failure(ErrorCode::truncated, start, "incomplete command header");
        }
        if (*payload_size > limits.maximum_payload) {
            return parse_failure(ErrorCode::resource_limit, start, "payload exceeds configured limit");
        }
        const auto payload = cursor.read_bytes(*payload_size);
        const auto expected = cursor.read_u16_le();
        if (!payload || !expected) {
            return parse_failure(ErrorCode::truncated, start, "command payload extends past end of tape");
        }
        const auto actual = fletcher16(*payload);
        const bool draft = *expected == 0xffffU && ((*command_flags & 0x80U) != 0);
        if (actual != *expected && !(draft && limits.permit_draft_checksums)) {
            if (diagnostics) {
                diagnostics->warning(*transaction, "tape", "checksum", "command checksum did not match");
            }
            continue;
        }
        Command command;
        command.opcode = *opcode;
        command.flags = *command_flags;
        command.transaction = *transaction;
        command.payload.assign(payload->begin(), payload->end());
        tape.commands.push_back(std::move(command));
    }

    if (tape.commands.empty() && diagnostics) {
        diagnostics->info(0, "tape", "empty", "tape contains no executable commands");
    }
    return tape;
}

std::vector<std::uint8_t> serialize_command_tape(const CommandTape& tape, bool draft_checksums) {
    ByteWriter writer;
    writer.reserve(12 + tape.commands.size() * 16);
    writer.write_bytes(tape_magic);
    writer.write_u8(tape.header.version == 0 ? 3 : tape.header.version);
    writer.write_u8(tape.header.flags);
    writer.write_u16_le(0);
    writer.write_u32_le(tape.header.session_seed);
    for (const auto& command : tape.commands) {
        const auto length = std::min<std::size_t>(command.payload.size(), 65535);
        writer.write_u8(command.opcode);
        writer.write_u8(draft_checksums ? static_cast<std::uint8_t>(command.flags | 0x80U) : command.flags);
        writer.write_u16_le(static_cast<std::uint16_t>(length));
        writer.write_u32_le(command.transaction);
        const auto payload = std::span<const std::uint8_t>(command.payload.data(), length);
        writer.write_bytes(payload);
        writer.write_u16_le(draft_checksums ? 0xffffU : fletcher16(payload));
    }
    return writer.take();
}

bool command_is_barrier(const Command& command) noexcept {
    return (command.flags & 0x01U) != 0 || command.opcode == 0x0fU || command.opcode == 0xffU;
}

bool command_is_mutating(const Command& command) noexcept {
    return command.opcode >= 0x10U && command.opcode <= 0x93U && (command.flags & 0x40U) == 0;
}

std::uint32_t command_domain(const Command& command) noexcept {
    if (command.opcode < 0x10U) return 0;
    return static_cast<std::uint32_t>((command.opcode - 0x10U) / 8U) + 1U;
}

}  // namespace wavecairn
