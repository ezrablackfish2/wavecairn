#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wavecairn {

enum class CommandCategory : std::uint8_t {
    control,
    media,
    track,
    clip,
    tempo,
    automation,
    plugin,
    send,
    marker,
    take,
    render,
    cache,
    waveform,
    event,
    snapshot,
    journal,
    project,
    export_data,
    package,
    query,
    repair,
    undo,
    unknown,
};

struct OpcodeDescriptor {
    std::uint8_t opcode{0};
    std::string_view name;
    CommandCategory category{CommandCategory::unknown};
    std::string_view purpose;
    std::uint16_t minimum_payload{0};
    bool mutating{false};
    bool lifecycle_sensitive{false};
};

const OpcodeDescriptor& describe_opcode(std::uint8_t opcode) noexcept;
std::string_view command_category_name(CommandCategory category) noexcept;
std::vector<OpcodeDescriptor> descriptors_for_category(CommandCategory category);
std::optional<std::uint8_t> opcode_from_name(std::string_view name) noexcept;
std::string format_opcode_table();

}  // namespace wavecairn
