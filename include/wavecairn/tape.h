#pragma once

#include "wavecairn/command.h"
#include "wavecairn/diagnostics.h"
#include "wavecairn/status.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wavecairn {

struct TapeLimits {
    std::size_t maximum_bytes{4 * 1024 * 1024};
    std::size_t maximum_commands{4096};
    std::size_t maximum_payload{65535};
    bool permit_draft_checksums{true};
};

Result<CommandTape> parse_command_tape(std::span<const std::uint8_t> bytes,
                                       Diagnostics* diagnostics = nullptr,
                                       const TapeLimits& limits = {});

std::vector<std::uint8_t> serialize_command_tape(const CommandTape& tape,
                                                 bool draft_checksums = false);

bool command_is_barrier(const Command& command) noexcept;
bool command_is_mutating(const Command& command) noexcept;
std::uint32_t command_domain(const Command& command) noexcept;

}  // namespace wavecairn
