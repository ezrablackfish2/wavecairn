#pragma once

#include "wavecairn/command.h"
#include "wavecairn/ids.h"
#include "wavecairn/media_pool.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace wavecairn {

class CommandBuilder {
public:
    explicit CommandBuilder(std::uint32_t session_seed = 1);

    CommandBuilder& transaction(std::uint32_t value) noexcept;
    CommandBuilder& flags(std::uint8_t value) noexcept;
    CommandBuilder& barrier(bool enabled = true) noexcept;

    CommandBuilder& add_media(MediaId id,
                              MediaKind kind,
                              std::string_view name,
                              std::string_view path,
                              std::uint64_t frame_count,
                              std::uint64_t content_hash,
                              std::uint32_t sample_rate = 48000,
                              std::uint16_t channels = 2);
    CommandBuilder& add_media_alias(MediaId alias,
                                    MediaId target,
                                    std::string_view label,
                                    bool preferred = false);
    CommandBuilder& update_media_path(MediaId id, std::string_view path);
    CommandBuilder& set_media_offline(MediaId id, bool offline);

    CommandBuilder& add_track(TrackId id,
                              std::string_view name,
                              std::uint16_t channels = 2,
                              float gain = 1.0F,
                              float pan = 0.0F,
                              std::uint8_t state = 0);
    CommandBuilder& set_track_mix(TrackId id,
                                  float gain,
                                  float pan,
                                  std::uint8_t state = 0);
    CommandBuilder& add_clip(ClipId id,
                             TrackId track,
                             MediaId media,
                             std::int32_t timeline_start,
                             std::int32_t source_start,
                             std::uint32_t length,
                             std::uint32_t group = 0,
                             std::uint8_t state = 0);
    CommandBuilder& move_clip(ClipId id,
                              std::int32_t timeline_start,
                              std::int32_t source_start,
                              std::uint32_t length);
    CommandBuilder& set_clip_envelope(ClipId id,
                                      float gain,
                                      float fade_in,
                                      float fade_out,
                                      std::uint8_t state = 0);

    CommandBuilder& add_tempo(EntityId id,
                              std::int32_t start_sample,
                              double start_bpm,
                              double end_bpm,
                              std::uint8_t numerator = 4,
                              std::uint8_t denominator = 4,
                              bool ramp = false);
    CommandBuilder& add_automation_lane(LaneId id,
                                        TrackId track,
                                        std::string_view parameter,
                                        float default_value = 0.0F,
                                        bool disabled = false);
    CommandBuilder& add_automation_points(LaneId id,
                                          std::span<const std::int32_t> samples,
                                          std::span<const float> values);
    CommandBuilder& add_marker(MarkerId id,
                               std::int32_t position,
                               std::uint32_t duration,
                               std::string_view name,
                               std::string_view note = {},
                               std::uint32_t color = 0);
    CommandBuilder& add_take(TakeId id,
                             TrackId track,
                             MediaId media,
                             std::int32_t source_start,
                             std::uint32_t length,
                             std::uint32_t lane,
                             float rating,
                             std::uint8_t state = 0);

    CommandBuilder& add_plugin(PluginId id,
                               TrackId track,
                               std::string_view vendor,
                               std::string_view product,
                               std::span<const float> parameters = {},
                               std::uint32_t latency = 0,
                               std::uint8_t state = 0);
    CommandBuilder& connect_plugin(PluginId source,
                                   PluginId destination,
                                   std::uint16_t source_bus = 0,
                                   std::uint16_t destination_bus = 0,
                                   float gain = 1.0F,
                                   bool sidechain = false);
    CommandBuilder& add_send(EntityId id,
                             TrackId source,
                             TrackId destination,
                             float level = 1.0F,
                             float pan = 0.0F,
                             std::uint8_t state = 0);
    CommandBuilder& add_listener(EntityId id,
                                 std::uint32_t topic,
                                 std::string_view name,
                                 bool disabled = false);
    CommandBuilder& publish_event(std::uint32_t topic,
                                  std::span<const std::uint8_t> payload,
                                  bool dispatch_immediately = false);

    CommandBuilder& add_render_job(EntityId id,
                                   TrackId track,
                                   std::int32_t start_sample,
                                   std::int32_t end_sample,
                                   std::uint32_t sample_rate = 48000,
                                   std::uint16_t channels = 2,
                                   std::uint16_t priority = 0,
                                   std::uint8_t state = 0);
    CommandBuilder& partition_render(std::uint16_t workers,
                                     std::uint32_t preferred_slice);
    CommandBuilder& add_sample_block(EntityId id,
                                     std::uint32_t sample_rate,
                                     std::uint16_t channels,
                                     std::span<const float> samples,
                                     bool pinned = false);
    CommandBuilder& build_waveform(EntityId media_id, EntityId block_id);
    CommandBuilder& add_export_path(std::string_view path);

    CommandBuilder& add_project_record(EntityId id,
                                       std::string_view key,
                                       std::string_view value,
                                       bool tombstone = false);
    CommandBuilder& add_package_section(std::string_view tag,
                                        std::span<const std::uint8_t> payload,
                                        std::uint16_t section_flags = 0);
    CommandBuilder& add_snapshot(EntityId id,
                                 std::string_view label,
                                 std::span<const TrackId> tracks);
    CommandBuilder& append_journal(std::uint32_t sequence,
                                   std::uint8_t opcode,
                                   std::span<const std::uint8_t> payload,
                                   bool committed = false);
    CommandBuilder& push_undo(EntityId id,
                              std::string_view label,
                              std::span<const std::uint8_t> payload,
                              bool checkpoint = false);

    CommandBuilder& raw(std::uint8_t opcode, std::span<const std::uint8_t> payload);

    const CommandTape& tape() const noexcept;
    CommandTape take();
    std::vector<std::uint8_t> serialize(bool draft_checksums = false) const;
    void clear();

private:
    void append(std::uint8_t opcode, std::vector<std::uint8_t> payload, std::uint8_t extra_flags = 0);

    CommandTape tape_;
    std::uint32_t transaction_{1};
    std::uint8_t flags_{0};
};

}  // namespace wavecairn
