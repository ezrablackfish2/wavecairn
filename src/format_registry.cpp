#include "wavecairn/format_registry.h"

#include "wavecairn/text.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

namespace wavecairn {
namespace {

constexpr OpcodeDescriptor unknown_descriptor{
    0xff, "unknown", CommandCategory::unknown, "Unrecognized command", 0, false, false};

constexpr auto descriptors = std::to_array<OpcodeDescriptor>({
    {0x10, "media.add", CommandCategory::media, "Register a media asset", 35, true, false},
    {0x11, "media.alias.add", CommandCategory::media, "Create an alternate media identifier", 18, true, false},
    {0x12, "media.path", CommandCategory::media, "Update a package-relative media path", 10, true, false},
    {0x13, "media.remove", CommandCategory::media, "Remove an asset or alias", 9, true, true},
    {0x14, "media.alias.promote", CommandCategory::media, "Select and promote a preferred alias", 9, true, true},
    {0x15, "media.status", CommandCategory::media, "Mark an asset online or offline", 9, true, false},
    {0x16, "media.capacity", CommandCategory::media, "Apply media-pool capacity policy", 5, true, true},
    {0x17, "media.path.probe", CommandCategory::media, "Inspect path storage across normalization", 13, false, true},

    {0x20, "track.add", CommandCategory::track, "Create an audio or instrument track", 20, true, false},
    {0x21, "track.mix", CommandCategory::track, "Change mute, solo, gain and pan", 17, true, false},
    {0x22, "track.rename", CommandCategory::track, "Rename an existing track", 9, true, false},
    {0x23, "track.remove", CommandCategory::track, "Remove a track and dependent timeline data", 8, true, true},
    {0x24, "track.capacity", CommandCategory::track, "Reserve track storage", 2, true, false},
    {0x25, "track.sort", CommandCategory::track, "Order tracks by stable display name", 0, true, true},
    {0x26, "track.clear-solo", CommandCategory::track, "Clear transient track state", 0, true, false},
    {0x27, "track.channels", CommandCategory::track, "Change track channel count", 10, true, false},

    {0x28, "clip.add", CommandCategory::clip, "Place a media clip on a track", 41, true, false},
    {0x29, "clip.move", CommandCategory::clip, "Move and trim a clip", 20, true, false},
    {0x2a, "clip.envelope", CommandCategory::clip, "Set clip gain, fades and lock state", 21, true, false},
    {0x2b, "clip.remove", CommandCategory::clip, "Remove a clip", 8, true, true},
    {0x2c, "clip.selection", CommandCategory::clip, "Manage selection across clip duplication", 9, true, true},
    {0x2d, "clip.sort", CommandCategory::clip, "Canonicalize clip ordering", 0, true, true},
    {0x2e, "clip.group.move", CommandCategory::clip, "Move all unlocked clips in a group", 8, true, false},
    {0x2f, "clip.capacity", CommandCategory::clip, "Reserve clip storage", 2, true, false},

    {0x30, "tempo.add", CommandCategory::tempo, "Add a constant or ramped tempo segment", 32, true, false},
    {0x31, "tempo.remove", CommandCategory::tempo, "Remove a tempo segment", 8, true, true},
    {0x32, "tempo.probe", CommandCategory::tempo, "Inspect a segment across compaction", 12, false, true},
    {0x33, "tempo.sort", CommandCategory::tempo, "Sort tempo segments", 0, true, true},
    {0x34, "tempo.shift", CommandCategory::tempo, "Shift all tempo anchors", 4, true, false},
    {0x35, "tempo.merge", CommandCategory::tempo, "Merge coincident tempo anchors", 0, true, true},
    {0x36, "tempo.scale", CommandCategory::tempo, "Scale the map around a global tempo", 8, true, false},
    {0x37, "tempo.capacity", CommandCategory::tempo, "Reserve tempo-map storage", 2, true, false},

    {0x38, "automation.lane.add", CommandCategory::automation, "Create an automation lane", 22, true, false},
    {0x39, "automation.points.add", CommandCategory::automation, "Append automation points", 9, true, false},
    {0x3a, "automation.erase", CommandCategory::automation, "Erase points in a sample range", 16, true, true},
    {0x3b, "automation.cursor", CommandCategory::automation, "Track a point across lane rebuilding", 13, false, true},
    {0x3c, "automation.merge", CommandCategory::automation, "Merge two lanes", 16, true, true},
    {0x3d, "automation.normalize", CommandCategory::automation, "Sort and deduplicate all lanes", 0, true, true},
    {0x3e, "automation.toggle", CommandCategory::automation, "Enable or disable a lane", 8, true, false},
    {0x3f, "automation.capacity", CommandCategory::automation, "Reserve point storage for a lane", 10, true, false},

    {0x40, "plugin.add", CommandCategory::plugin, "Add a plug-in node", 25, true, false},
    {0x41, "plugin.connect", CommandCategory::plugin, "Connect plug-in buses", 25, true, false},
    {0x42, "plugin.state", CommandCategory::plugin, "Change bypass, role and latency", 13, true, false},
    {0x43, "plugin.parameter", CommandCategory::plugin, "Write a plug-in parameter", 14, true, false},
    {0x44, "plugin.schedule", CommandCategory::plugin, "Schedule a node across graph expansion", 12, false, true},
    {0x45, "plugin.remove", CommandCategory::plugin, "Remove a node and attached edges", 8, true, true},
    {0x46, "plugin.capacity", CommandCategory::plugin, "Reserve graph storage", 2, true, false},
    {0x47, "plugin.canonicalize", CommandCategory::plugin, "Normalize graph ordering", 0, true, true},

    {0x48, "send.add", CommandCategory::send, "Add a mixer send", 29, true, false},
    {0x49, "send.update", CommandCategory::send, "Change send level and placement", 17, true, false},
    {0x4a, "send.remove", CommandCategory::send, "Remove a mixer send", 8, true, true},
    {0x4b, "send.selection", CommandCategory::send, "Inspect a send across route normalization", 12, false, true},
    {0x4c, "send.toggle", CommandCategory::send, "Toggle sends between two tracks", 16, true, false},
    {0x4d, "send.capacity", CommandCategory::send, "Reserve send storage", 2, true, false},
    {0x4e, "send.limit", CommandCategory::send, "Limit aggregate send gain", 0, true, false},
    {0x4f, "send.deduplicate", CommandCategory::send, "Remove duplicate sends", 0, true, true},

    {0x50, "marker.add", CommandCategory::marker, "Add a timeline marker", 23, true, false},
    {0x51, "marker.move", CommandCategory::marker, "Move or resize a marker", 16, true, false},
    {0x52, "marker.remove", CommandCategory::marker, "Remove a marker", 8, true, true},
    {0x53, "marker.probe", CommandCategory::marker, "Inspect marker text across normalization", 12, false, true},
    {0x54, "marker.shift", CommandCategory::marker, "Shift all markers", 4, true, false},
    {0x55, "marker.sanitize", CommandCategory::marker, "Normalize marker text", 0, true, true},
    {0x56, "marker.capacity", CommandCategory::marker, "Reserve marker storage", 2, true, false},
    {0x57, "marker.deduplicate", CommandCategory::marker, "Remove coincident named markers", 0, true, true},

    {0x58, "take.add", CommandCategory::take, "Add a recorded take", 41, true, false},
    {0x59, "take.rate", CommandCategory::take, "Rate and activate a take", 13, true, false},
    {0x5a, "take.remove", CommandCategory::take, "Remove a take", 8, true, true},
    {0x5b, "take.audition", CommandCategory::take, "Audition a take across compaction", 12, false, true},
    {0x5c, "take.comp", CommandCategory::take, "Choose the best take for a track", 8, true, false},
    {0x5d, "take.lane", CommandCategory::take, "Move active takes to a lane", 12, true, false},
    {0x5e, "take.capacity", CommandCategory::take, "Reserve take storage", 2, true, false},
    {0x5f, "take.sort", CommandCategory::take, "Canonicalize take ordering", 0, true, true},

    {0x60, "render.job.add", CommandCategory::render, "Create a render job", 31, true, false},
    {0x61, "render.job.update", CommandCategory::render, "Update render range and priority", 18, true, false},
    {0x62, "render.partition", CommandCategory::render, "Partition jobs into worker slices", 6, true, true},
    {0x63, "render.plan.probe", CommandCategory::render, "Inspect a slice across repartition", 5, false, true},
    {0x64, "render.job.remove", CommandCategory::render, "Remove a job and its slices", 8, true, true},
    {0x65, "render.job.sort", CommandCategory::render, "Sort jobs by scheduling priority", 0, true, true},
    {0x66, "render.capacity", CommandCategory::render, "Reserve plan storage", 2, true, false},
    {0x67, "render.slices.clear", CommandCategory::render, "Discard generated render slices", 0, true, true},

    {0x68, "cache.block.add", CommandCategory::cache, "Insert decoded sample data", 17, true, false},
    {0x69, "cache.block.migrate", CommandCategory::cache, "Move a sample block between cache tiers", 12, true, true},
    {0x6a, "cache.trim", CommandCategory::cache, "Trim unpinned sample blocks", 2, true, true},
    {0x6b, "cache.retired.clear", CommandCategory::cache, "Release retired cache blocks", 0, true, true},

    {0x6c, "waveform.build", CommandCategory::waveform, "Build waveform pyramid levels", 16, true, false},
    {0x6d, "waveform.probe", CommandCategory::waveform, "Inspect level data across rebuilding", 14, false, true},
    {0x6e, "waveform.remove", CommandCategory::waveform, "Remove a waveform pyramid", 8, true, true},
    {0x6f, "waveform.capacity", CommandCategory::waveform, "Reserve waveform storage", 2, true, false},

    {0x70, "event.listener.add", CommandCategory::event, "Register an event listener", 14, true, false},
    {0x71, "event.publish", CommandCategory::event, "Queue or publish a routed event", 6, true, false},
    {0x72, "event.listener.defer", CommandCategory::event, "Defer delivery across listener removal", 12, false, true},
    {0x73, "event.dispatch", CommandCategory::event, "Dispatch all queued events", 0, true, false},
    {0x74, "event.listener.remove", CommandCategory::event, "Remove an event listener", 8, true, true},
    {0x75, "event.clear", CommandCategory::event, "Discard queued events", 0, true, false},
    {0x76, "event.listener.toggle", CommandCategory::event, "Toggle listener delivery", 8, true, false},
    {0x77, "event.capacity", CommandCategory::event, "Reserve listener and event storage", 2, true, false},

    {0x78, "snapshot.add", CommandCategory::snapshot, "Store a session snapshot", 10, true, false},
    {0x79, "snapshot.restore", CommandCategory::snapshot, "Restore track state across snapshot rebuilding", 14, true, true},
    {0x7a, "snapshot.remove", CommandCategory::snapshot, "Remove a session snapshot", 8, true, true},
    {0x7b, "snapshot.sort", CommandCategory::snapshot, "Canonicalize snapshot ordering", 0, true, true},

    {0x7c, "journal.append", CommandCategory::journal, "Append a command journal record", 7, true, false},
    {0x7d, "journal.probe", CommandCategory::journal, "Inspect payload storage across compaction", 7, false, true},
    {0x7e, "journal.commit", CommandCategory::journal, "Commit records through a sequence", 4, true, false},
    {0x7f, "journal.compact", CommandCategory::journal, "Remove empty uncommitted records", 0, true, true},

    {0x80, "project.record.add", CommandCategory::project, "Add project metadata", 12, true, false},
    {0x81, "project.record.select", CommandCategory::project, "Inspect metadata across compaction", 12, false, true},
    {0x82, "project.record.delete", CommandCategory::project, "Tombstone project metadata", 8, true, false},
    {0x83, "project.record.sort", CommandCategory::project, "Canonicalize project metadata", 0, true, true},

    {0x84, "export.path.add", CommandCategory::export_data, "Add a package-relative export path", 2, true, false},
    {0x85, "export.path.probe", CommandCategory::export_data, "Inspect path storage across normalization", 5, false, true},
    {0x86, "export.path.deduplicate", CommandCategory::export_data, "Deduplicate export paths", 0, true, true},
    {0x87, "export.path.clear", CommandCategory::export_data, "Clear export paths", 0, true, false},

    {0x88, "package.section.add", CommandCategory::package, "Add a package section", 8, true, false},
    {0x89, "package.section.select", CommandCategory::package, "Inspect a section across canonicalization", 5, false, true},
    {0x8a, "package.section.remove", CommandCategory::package, "Remove sections by tag", 4, true, true},
    {0x8b, "package.canonicalize", CommandCategory::package, "Normalize section ordering and text", 0, true, true},

    {0x8c, "query.rebuild", CommandCategory::query, "Rebuild searchable routing rows", 0, true, true},
    {0x8d, "query.select", CommandCategory::query, "Inspect a row across result rebuilding", 5, false, true},
    {0x8e, "query.execute", CommandCategory::query, "Execute a routing metadata query", 9, false, false},
    {0x8f, "query.clear", CommandCategory::query, "Clear cached query rows", 0, true, true},

    {0x90, "repair.canonicalize", CommandCategory::repair, "Canonicalize package data", 0, true, true},
    {0x91, "repair.metadata.probe", CommandCategory::repair, "Inspect metadata text across rebuilding", 5, false, true},
    {0x92, "repair.merge-text", CommandCategory::repair, "Annotate duplicate text sections", 0, true, true},
    {0x93, "repair.remove-empty", CommandCategory::repair, "Remove empty optional sections", 0, true, true},

    {0x94, "undo.push", CommandCategory::undo, "Push an undo frame", 12, true, false},
    {0x95, "undo.branch", CommandCategory::undo, "Inspect branch payload across history trimming", 3, false, true},
    {0x96, "undo.step", CommandCategory::undo, "Move a frame to the redo stack", 0, true, true},
    {0x97, "redo.step", CommandCategory::undo, "Restore a frame from the redo stack", 0, true, true},
});

}  // namespace

const OpcodeDescriptor& describe_opcode(std::uint8_t opcode) noexcept {
    const auto found = std::lower_bound(descriptors.begin(), descriptors.end(), opcode,
        [](const OpcodeDescriptor& descriptor, std::uint8_t value) { return descriptor.opcode < value; });
    return found != descriptors.end() && found->opcode == opcode ? *found : unknown_descriptor;
}

std::string_view command_category_name(CommandCategory category) noexcept {
    switch (category) {
        case CommandCategory::control: return "control";
        case CommandCategory::media: return "media";
        case CommandCategory::track: return "track";
        case CommandCategory::clip: return "clip";
        case CommandCategory::tempo: return "tempo";
        case CommandCategory::automation: return "automation";
        case CommandCategory::plugin: return "plugin";
        case CommandCategory::send: return "send";
        case CommandCategory::marker: return "marker";
        case CommandCategory::take: return "take";
        case CommandCategory::render: return "render";
        case CommandCategory::cache: return "cache";
        case CommandCategory::waveform: return "waveform";
        case CommandCategory::event: return "event";
        case CommandCategory::snapshot: return "snapshot";
        case CommandCategory::journal: return "journal";
        case CommandCategory::project: return "project";
        case CommandCategory::export_data: return "export";
        case CommandCategory::package: return "package";
        case CommandCategory::query: return "query";
        case CommandCategory::repair: return "repair";
        case CommandCategory::undo: return "undo";
        case CommandCategory::unknown: return "unknown";
    }
    return "unknown";
}

std::vector<OpcodeDescriptor> descriptors_for_category(CommandCategory category) {
    std::vector<OpcodeDescriptor> result;
    for (const auto& descriptor : descriptors) if (descriptor.category == category) result.push_back(descriptor);
    return result;
}

std::optional<std::uint8_t> opcode_from_name(std::string_view name) noexcept {
    const auto lowered = ascii_lower(name);
    const auto found = std::find_if(descriptors.begin(), descriptors.end(), [&lowered](const OpcodeDescriptor& descriptor) {
        return descriptor.name == lowered;
    });
    return found == descriptors.end() ? std::nullopt : std::optional<std::uint8_t>(found->opcode);
}

std::string format_opcode_table() {
    std::ostringstream out;
    out << "opcode  category      mutation  lifecycle  name                     purpose\n";
    out << "------  ------------  --------  ---------  -----------------------  ------------------------------\n";
    for (const auto& descriptor : descriptors) {
        out << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(descriptor.opcode)
            << std::dec << std::setfill(' ') << "    "
            << std::left << std::setw(12) << command_category_name(descriptor.category) << "  "
            << std::setw(8) << (descriptor.mutating ? "yes" : "no") << "  "
            << std::setw(9) << (descriptor.lifecycle_sensitive ? "yes" : "no") << "  "
            << std::setw(23) << descriptor.name << "  " << descriptor.purpose << '\n';
    }
    return out.str();
}

}  // namespace wavecairn
