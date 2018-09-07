#include "wavecairn/command_builder.h"

#include "wavecairn/byte_cursor.h"
#include "wavecairn/package.h"
#include "wavecairn/tape.h"

#include <algorithm>

namespace wavecairn {
namespace {

void write_id(ByteWriter& writer, EntityId id) { writer.write_u64_le(id.value()); }

}  // namespace

CommandBuilder::CommandBuilder(std::uint32_t session_seed) {
    tape_.header.version = 3;
    tape_.header.session_seed = session_seed;
}

CommandBuilder& CommandBuilder::transaction(std::uint32_t value) noexcept {
    transaction_ = value;
    return *this;
}

CommandBuilder& CommandBuilder::flags(std::uint8_t value) noexcept {
    flags_ = value;
    return *this;
}

CommandBuilder& CommandBuilder::barrier(bool enabled) noexcept {
    if (enabled) flags_ |= 0x01U;
    else flags_ &= static_cast<std::uint8_t>(~0x01U);
    return *this;
}

CommandBuilder& CommandBuilder::add_media(MediaId id,
                                          MediaKind kind,
                                          std::string_view name,
                                          std::string_view path,
                                          std::uint64_t frame_count,
                                          std::uint64_t content_hash,
                                          std::uint32_t sample_rate,
                                          std::uint16_t channels) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_u8(static_cast<std::uint8_t>(kind));
    writer.write_u8(0);
    writer.write_u16_le(channels);
    writer.write_u32_le(sample_rate);
    writer.write_u64_le(frame_count);
    writer.write_u64_le(content_hash);
    writer.write_string8(name);
    writer.write_string16(path);
    append(0x10, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_media_alias(MediaId alias,
                                                MediaId target,
                                                std::string_view label,
                                                bool preferred) {
    ByteWriter writer;
    write_id(writer, alias);
    write_id(writer, target);
    writer.write_u8(preferred ? 1U : 0U);
    writer.write_string8(label);
    append(0x11, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::update_media_path(MediaId id, std::string_view path) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_string16(path);
    append(0x12, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::set_media_offline(MediaId id, bool offline) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_u8(offline ? 1U : 0U);
    append(0x15, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_track(TrackId id,
                                          std::string_view name,
                                          std::uint16_t channels,
                                          float gain,
                                          float pan,
                                          std::uint8_t state) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_u16_le(channels);
    writer.write_u8(state);
    writer.write_f32_le(gain);
    writer.write_f32_le(pan);
    writer.write_string8(name);
    append(0x20, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::set_track_mix(TrackId id,
                                              float gain,
                                              float pan,
                                              std::uint8_t state) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_u8(state);
    writer.write_f32_le(gain);
    writer.write_f32_le(pan);
    append(0x21, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_clip(ClipId id,
                                         TrackId track,
                                         MediaId media,
                                         std::int32_t timeline_start,
                                         std::int32_t source_start,
                                         std::uint32_t length,
                                         std::uint32_t group,
                                         std::uint8_t state) {
    ByteWriter writer;
    write_id(writer, id);
    write_id(writer, track);
    write_id(writer, media);
    writer.write_i32_le(timeline_start);
    writer.write_i32_le(source_start);
    writer.write_u32_le(length);
    writer.write_u8(state);
    writer.write_u32_le(group);
    append(0x28, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::move_clip(ClipId id,
                                          std::int32_t timeline_start,
                                          std::int32_t source_start,
                                          std::uint32_t length) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_i32_le(timeline_start);
    writer.write_i32_le(source_start);
    writer.write_u32_le(length);
    append(0x29, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::set_clip_envelope(ClipId id,
                                                  float gain,
                                                  float fade_in,
                                                  float fade_out,
                                                  std::uint8_t state) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_f32_le(gain);
    writer.write_f32_le(fade_in);
    writer.write_f32_le(fade_out);
    writer.write_u8(state);
    append(0x2a, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_tempo(EntityId id,
                                          std::int32_t start_sample,
                                          double start_bpm,
                                          double end_bpm,
                                          std::uint8_t numerator,
                                          std::uint8_t denominator,
                                          bool ramp) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_i32_le(start_sample);
    writer.write_f64_le(start_bpm);
    writer.write_f64_le(end_bpm);
    writer.write_u8(numerator);
    writer.write_u8(denominator);
    writer.write_u8(ramp ? 1U : 0U);
    append(0x30, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_automation_lane(LaneId id,
                                                    TrackId track,
                                                    std::string_view parameter,
                                                    float default_value,
                                                    bool disabled) {
    ByteWriter writer;
    write_id(writer, id);
    write_id(writer, track);
    writer.write_f32_le(default_value);
    writer.write_u8(disabled ? 1U : 0U);
    writer.write_string8(parameter);
    append(0x38, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_automation_points(LaneId id,
                                                      std::span<const std::int32_t> samples,
                                                      std::span<const float> values) {
    ByteWriter writer;
    write_id(writer, id);
    const auto count = std::min<std::size_t>({samples.size(), values.size(), 64});
    writer.write_u8(static_cast<std::uint8_t>(count));
    for (std::size_t index = 0; index < count; ++index) {
        writer.write_i32_le(samples[index]);
        writer.write_f32_le(values[index]);
        writer.write_f32_le(0.0F);
        writer.write_u8(0);
    }
    append(0x39, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_marker(MarkerId id,
                                           std::int32_t position,
                                           std::uint32_t duration,
                                           std::string_view name,
                                           std::string_view note,
                                           std::uint32_t color) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_i32_le(position);
    writer.write_u32_le(duration);
    writer.write_u32_le(color);
    writer.write_string8(name);
    writer.write_string16(note);
    append(0x50, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_take(TakeId id,
                                         TrackId track,
                                         MediaId media,
                                         std::int32_t source_start,
                                         std::uint32_t length,
                                         std::uint32_t lane,
                                         float rating,
                                         std::uint8_t state) {
    ByteWriter writer;
    write_id(writer, id);
    write_id(writer, track);
    write_id(writer, media);
    writer.write_i32_le(source_start);
    writer.write_u32_le(length);
    writer.write_u32_le(lane);
    writer.write_f32_le(rating);
    writer.write_u8(state);
    append(0x58, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_plugin(PluginId id,
                                           TrackId track,
                                           std::string_view vendor,
                                           std::string_view product,
                                           std::span<const float> parameters,
                                           std::uint32_t latency,
                                           std::uint8_t state) {
    ByteWriter writer;
    write_id(writer, id);
    write_id(writer, track);
    writer.write_u8(state);
    writer.write_u32_le(latency);
    const auto count = std::min<std::size_t>(parameters.size(), 64);
    writer.write_u8(static_cast<std::uint8_t>(count));
    writer.write_string8(vendor);
    writer.write_string8(product);
    for (std::size_t index = 0; index < count; ++index) writer.write_f32_le(parameters[index]);
    append(0x40, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::connect_plugin(PluginId source,
                                               PluginId destination,
                                               std::uint16_t source_bus,
                                               std::uint16_t destination_bus,
                                               float gain,
                                               bool sidechain) {
    ByteWriter writer;
    write_id(writer, source);
    write_id(writer, destination);
    writer.write_u16_le(source_bus);
    writer.write_u16_le(destination_bus);
    writer.write_f32_le(gain);
    writer.write_u8(sidechain ? 1U : 0U);
    append(0x41, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_send(EntityId id,
                                         TrackId source,
                                         TrackId destination,
                                         float level,
                                         float pan,
                                         std::uint8_t state) {
    ByteWriter writer;
    write_id(writer, id);
    write_id(writer, source);
    write_id(writer, destination);
    writer.write_f32_le(level);
    writer.write_f32_le(pan);
    writer.write_u8(state);
    append(0x48, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_listener(EntityId id,
                                             std::uint32_t topic,
                                             std::string_view name,
                                             bool disabled) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_u32_le(topic);
    writer.write_u8(disabled ? 1U : 0U);
    writer.write_string8(name);
    append(0x70, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::publish_event(std::uint32_t topic,
                                              std::span<const std::uint8_t> payload,
                                              bool dispatch_immediately) {
    ByteWriter writer;
    writer.write_u32_le(topic);
    const auto length = std::min<std::size_t>(payload.size(), 65535);
    writer.write_u16_le(static_cast<std::uint16_t>(length));
    writer.write_bytes(payload.first(length));
    append(0x71, writer.take(), dispatch_immediately ? 1U : 0U);
    return *this;
}

CommandBuilder& CommandBuilder::add_render_job(EntityId id,
                                               TrackId track,
                                               std::int32_t start_sample,
                                               std::int32_t end_sample,
                                               std::uint32_t sample_rate,
                                               std::uint16_t channels,
                                               std::uint16_t priority,
                                               std::uint8_t state) {
    ByteWriter writer;
    write_id(writer, id);
    write_id(writer, track);
    writer.write_i32_le(start_sample);
    writer.write_i32_le(end_sample);
    writer.write_u32_le(sample_rate);
    writer.write_u16_le(channels);
    writer.write_u16_le(priority);
    writer.write_u8(state);
    append(0x60, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::partition_render(std::uint16_t workers,
                                                 std::uint32_t preferred_slice) {
    ByteWriter writer;
    writer.write_u16_le(workers);
    writer.write_u32_le(preferred_slice);
    append(0x62, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_sample_block(EntityId id,
                                                 std::uint32_t sample_rate,
                                                 std::uint16_t channels,
                                                 std::span<const float> samples,
                                                 bool pinned) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_u32_le(sample_rate);
    writer.write_u16_le(channels);
    const auto count = std::min<std::size_t>(samples.size(), 4096);
    writer.write_u16_le(static_cast<std::uint16_t>(count));
    writer.write_u8(pinned ? 1U : 0U);
    for (std::size_t index = 0; index < count; ++index) writer.write_f32_le(samples[index]);
    append(0x68, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::build_waveform(EntityId media_id, EntityId block_id) {
    ByteWriter writer;
    write_id(writer, media_id);
    write_id(writer, block_id);
    append(0x6c, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_export_path(std::string_view path) {
    ByteWriter writer;
    writer.write_string16(path);
    append(0x84, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_project_record(EntityId id,
                                                   std::string_view key,
                                                   std::string_view value,
                                                   bool tombstone) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_u8(tombstone ? 1U : 0U);
    writer.write_string8(key);
    writer.write_string16(value);
    append(0x80, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_package_section(std::string_view tag,
                                                    std::span<const std::uint8_t> payload,
                                                    std::uint16_t section_flags) {
    ByteWriter writer;
    const auto normalized = make_section_tag(tag);
    writer.write_bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(normalized.data()), normalized.size()));
    writer.write_u16_le(section_flags);
    const auto length = std::min<std::size_t>(payload.size(), 65535);
    writer.write_u16_le(static_cast<std::uint16_t>(length));
    writer.write_bytes(payload.first(length));
    append(0x88, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::add_snapshot(EntityId id,
                                             std::string_view label,
                                             std::span<const TrackId> tracks) {
    ByteWriter writer;
    write_id(writer, id);
    const auto count = std::min<std::size_t>(tracks.size(), 64);
    writer.write_u8(static_cast<std::uint8_t>(count));
    writer.write_string8(label);
    for (std::size_t index = 0; index < count; ++index) {
        write_id(writer, tracks[index]);
        writer.write_f32_le(1.0F);
        writer.write_f32_le(0.0F);
        writer.write_u32_le(transaction_);
        writer.write_string8("track-" + tracks[index].to_string().substr(12));
    }
    append(0x78, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::append_journal(std::uint32_t sequence,
                                               std::uint8_t opcode,
                                               std::span<const std::uint8_t> payload,
                                               bool committed) {
    ByteWriter writer;
    writer.write_u32_le(sequence);
    writer.write_u8(opcode);
    const auto length = std::min<std::size_t>(payload.size(), 65535);
    writer.write_u16_le(static_cast<std::uint16_t>(length));
    writer.write_bytes(payload.first(length));
    append(0x7c, writer.take(), committed ? 1U : 0U);
    return *this;
}

CommandBuilder& CommandBuilder::push_undo(EntityId id,
                                          std::string_view label,
                                          std::span<const std::uint8_t> payload,
                                          bool checkpoint) {
    ByteWriter writer;
    write_id(writer, id);
    writer.write_u8(checkpoint ? 1U : 0U);
    const auto length = std::min<std::size_t>(payload.size(), 65535);
    writer.write_u16_le(static_cast<std::uint16_t>(length));
    writer.write_string8(label);
    writer.write_bytes(payload.first(length));
    append(0x94, writer.take());
    return *this;
}

CommandBuilder& CommandBuilder::raw(std::uint8_t opcode, std::span<const std::uint8_t> payload) {
    append(opcode, std::vector<std::uint8_t>(payload.begin(), payload.end()));
    return *this;
}

const CommandTape& CommandBuilder::tape() const noexcept { return tape_; }

CommandTape CommandBuilder::take() {
    auto result = std::move(tape_);
    tape_ = CommandTape{};
    tape_.header.version = 3;
    tape_.header.session_seed = result.header.session_seed;
    transaction_ = 1;
    flags_ = 0;
    return result;
}

std::vector<std::uint8_t> CommandBuilder::serialize(bool draft_checksums) const {
    return serialize_command_tape(tape_, draft_checksums);
}

void CommandBuilder::clear() {
    tape_.commands.clear();
    transaction_ = 1;
    flags_ = 0;
}

void CommandBuilder::append(std::uint8_t opcode,
                            std::vector<std::uint8_t> payload,
                            std::uint8_t extra_flags) {
    tape_.commands.push_back(Command{opcode,
                                     static_cast<std::uint8_t>(flags_ | extra_flags),
                                     transaction_,
                                     std::move(payload)});
    ++transaction_;
}

}  // namespace wavecairn
