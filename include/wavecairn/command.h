#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace wavecairn {

struct Command {
    std::uint8_t opcode{0};
    std::uint8_t flags{0};
    std::uint32_t transaction{0};
    std::vector<std::uint8_t> payload;

    std::span<const std::uint8_t> bytes() const noexcept { return payload; }
};

struct TapeHeader {
    std::uint8_t version{0};
    std::uint8_t flags{0};
    std::uint32_t session_seed{0};
};

struct CommandTape {
    TapeHeader header;
    std::vector<Command> commands;
};

}  // namespace wavecairn
