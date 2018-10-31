#include "wavecairn/merge.h"

#include "wavecairn/command_builder.h"
#include "wavecairn/text.h"
#include "wavecairn/tape.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace wavecairn {
namespace {

bool text_equal(std::string_view lhs, std::string_view rhs) {
    return lhs == rhs;
}

bool float_equal(float lhs, float rhs) {
    return std::fabs(lhs - rhs) <= 0.00001F;
}

bool double_equal(double lhs, double rhs) {
    return std::fabs(lhs - rhs) <= 0.0000001;
}

template <typename Item>
std::uint32_t item_revision(const Item& item) {
    if constexpr (requires { item.revision; }) {
        return item.revision;
    } else if constexpr (requires { item.generation; }) {
        return item.generation;
    } else {
        return 0;
    }
}

template <typename Item>
bool should_replace(const Item& base,
                    const Item& incoming,
                    MergePreference preference) {
    switch (preference) {
        case MergePreference::keep_base:
            return false;
        case MergePreference::prefer_incoming:
            return true;
        case MergePreference::prefer_newer_revision:
            return item_revision(incoming) > item_revision(base);
    }
    return false;
}

std::string csv_escape_local(std::string_view value) {
    bool quote = false;
    for (char ch : value) {
        if (ch == ',' || ch == '"' || ch == '\n' || ch == '\r') {
            quote = true;
            break;
        }
    }
    if (!quote) return std::string(value);
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') out += "\"\"";
        else out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

void record(MergePlan& plan,
            std::string area,
            EntityId id,
            MergeDisposition disposition,
            std::string reason) {
    plan.decisions.push_back(MergeDecision{
        std::move(area),
        id,
        disposition,
        std::move(reason),
    });
    switch (disposition) {
        case MergeDisposition::added:
            ++plan.additions;
            break;
        case MergeDisposition::replaced:
            ++plan.replacements;
            break;
        case MergeDisposition::retained:
        case MergeDisposition::skipped:
            ++plan.retained;
            break;
        case MergeDisposition::conflict:
            ++plan.conflicts;
            break;
    }
}

bool same_asset(const MediaAsset& lhs, const MediaAsset& rhs) {
    return lhs.kind == rhs.kind &&
           text_equal(lhs.logical_name, rhs.logical_name) &&
           text_equal(lhs.relative_path, rhs.relative_path) &&
           lhs.format.sample_rate == rhs.format.sample_rate &&
           lhs.format.channels == rhs.format.channels &&
           lhs.format.bits_per_sample == rhs.format.bits_per_sample &&
           lhs.format.floating_point == rhs.format.floating_point &&
           lhs.frame_count == rhs.frame_count &&
           lhs.content_hash == rhs.content_hash &&
           lhs.offline == rhs.offline &&
           lhs.generated == rhs.generated;
}

bool same_alias(const MediaAlias& lhs, const MediaAlias& rhs) {
    return lhs.target_id == rhs.target_id &&
           text_equal(lhs.label, rhs.label) &&
           lhs.preferred == rhs.preferred;
}

bool same_track(const Track& lhs, const Track& rhs) {
    return text_equal(lhs.name, rhs.name) &&
           lhs.channels == rhs.channels &&
           float_equal(lhs.gain, rhs.gain) &&
           float_equal(lhs.pan, rhs.pan) &&
           lhs.muted == rhs.muted &&
           lhs.solo == rhs.solo &&
           lhs.armed == rhs.armed;
}

bool same_clip(const Clip& lhs, const Clip& rhs) {
    return lhs.track_id == rhs.track_id &&
           lhs.media_id == rhs.media_id &&
           lhs.timeline_start == rhs.timeline_start &&
           lhs.source_start == rhs.source_start &&
           lhs.length == rhs.length &&
           float_equal(lhs.gain, rhs.gain) &&
           float_equal(lhs.fade_in, rhs.fade_in) &&
           float_equal(lhs.fade_out, rhs.fade_out) &&
           lhs.group == rhs.group &&
           lhs.muted == rhs.muted &&
           lhs.locked == rhs.locked;
}

bool same_tempo(const TempoSegment& lhs, const TempoSegment& rhs) {
    return lhs.start_sample == rhs.start_sample &&
           double_equal(lhs.start_bpm, rhs.start_bpm) &&
           double_equal(lhs.end_bpm, rhs.end_bpm) &&
           lhs.numerator == rhs.numerator &&
           lhs.denominator == rhs.denominator &&
           lhs.ramp == rhs.ramp;
}

bool same_lane(const AutomationLane& lhs, const AutomationLane& rhs) {
    if (lhs.track_id != rhs.track_id ||
        !text_equal(lhs.parameter, rhs.parameter) ||
        !float_equal(lhs.default_value, rhs.default_value) ||
        lhs.enabled != rhs.enabled ||
        lhs.points.size() != rhs.points.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.points.size(); ++index) {
        const auto& a = lhs.points[index];
        const auto& b = rhs.points[index];
        if (a.sample != b.sample ||
            !float_equal(a.value, b.value) ||
            !float_equal(a.tension, b.tension) ||
            a.shape != b.shape) {
            return false;
        }
    }
    return true;
}

bool same_marker(const Marker& lhs, const Marker& rhs) {
    return lhs.position == rhs.position &&
           lhs.duration == rhs.duration &&
           text_equal(lhs.name, rhs.name) &&
           text_equal(lhs.note, rhs.note) &&
           lhs.color == rhs.color;
}

bool same_take(const Take& lhs, const Take& rhs) {
    return lhs.track_id == rhs.track_id &&
           lhs.media_id == rhs.media_id &&
           lhs.source_start == rhs.source_start &&
           lhs.length == rhs.length &&
           lhs.lane == rhs.lane &&
           float_equal(lhs.rating, rhs.rating) &&
           lhs.active == rhs.active &&
           lhs.rejected == rhs.rejected;
}

std::uint8_t track_state(const Track& track) {
    std::uint8_t state = 0;
    if (track.muted) {
        state |= 0x01U;
    }
    if (track.solo) {
        state |= 0x02U;
    }
    if (track.armed) {
        state |= 0x04U;
    }
    return state;
}

std::uint8_t clip_state(const Clip& clip) {
    std::uint8_t state = 0;
    if (clip.muted) {
        state |= 0x01U;
    }
    if (clip.locked) {
        state |= 0x02U;
    }
    return state;
}

std::uint8_t take_state(const Take& take) {
    std::uint8_t state = 0;
    if (take.active) {
        state |= 0x01U;
    }
    if (take.rejected) {
        state |= 0x02U;
    }
    return state;
}

void emit_asset(CommandBuilder& builder, const MediaAsset& asset) {
    builder.add_media(asset.id,
                      asset.kind,
                      asset.logical_name,
                      asset.relative_path,
                      asset.frame_count,
                      asset.content_hash,
                      asset.format.sample_rate,
                      asset.format.channels);
    if (asset.offline) {
        builder.set_media_offline(asset.id, true);
    }
}

void merge_media(const Session& base,
                 const Session& incoming,
                 const MergePolicy& policy,
                 CommandBuilder& builder,
                 MergePlan& plan) {
    if (!policy.include_media) {
        return;
    }

    std::unordered_map<MediaId, const MediaAsset*, EntityIdHash> existing;
    for (const auto& asset : base.media().assets()) {
        existing.emplace(asset.id, &asset);
    }

    for (const auto& asset : incoming.media().assets()) {
        const auto found = existing.find(asset.id);
        if (found == existing.end()) {
            emit_asset(builder, asset);
            record(plan, "media", asset.id, MergeDisposition::added,
                   "media identifier is absent from the base session");
            continue;
        }
        const auto& current = *found->second;
        if (same_asset(current, asset)) {
            record(plan, "media", asset.id, MergeDisposition::retained,
                   "media metadata and storage attributes are identical");
            continue;
        }
        if (should_replace(current, asset, policy.preference)) {
            builder.update_media_path(asset.id, asset.relative_path);
            if (policy.preserve_offline_state && current.offline != asset.offline) {
                builder.set_media_offline(asset.id, asset.offline);
            }
            record(plan, "media", asset.id, MergeDisposition::replaced,
                   "incoming media revision was selected by policy");
        } else {
            record(plan, "media", asset.id, MergeDisposition::conflict,
                   "media definitions differ and base revision was retained");
        }
    }

    std::unordered_map<MediaId, const MediaAlias*, EntityIdHash> aliases;
    for (const auto& alias : base.media().aliases()) {
        aliases.emplace(alias.alias_id, &alias);
    }
    for (const auto& alias : incoming.media().aliases()) {
        const auto found = aliases.find(alias.alias_id);
        if (found == aliases.end()) {
            builder.add_media_alias(alias.alias_id,
                                    alias.target_id,
                                    alias.label,
                                    alias.preferred);
            record(plan, "media-alias", alias.alias_id, MergeDisposition::added,
                   "alias is absent from the base session");
            continue;
        }
        if (same_alias(*found->second, alias)) {
            record(plan, "media-alias", alias.alias_id, MergeDisposition::retained,
                   "alias target and label are identical");
            continue;
        }
        record(plan, "media-alias", alias.alias_id, MergeDisposition::conflict,
               "alias definitions differ; aliases are not overwritten implicitly");
    }
}

void emit_track(CommandBuilder& builder, const Track& track) {
    builder.add_track(track.id,
                      track.name,
                      track.channels,
                      track.gain,
                      track.pan,
                      track_state(track));
}

void emit_clip(CommandBuilder& builder, const Clip& clip) {
    builder.add_clip(clip.id,
                     clip.track_id,
                     clip.media_id,
                     static_cast<std::int32_t>(clip.timeline_start),
                     static_cast<std::int32_t>(clip.source_start),
                     static_cast<std::uint32_t>(std::max<std::int64_t>(0, clip.length)),
                     clip.group,
                     clip_state(clip));
    builder.set_clip_envelope(clip.id,
                              clip.gain,
                              clip.fade_in,
                              clip.fade_out,
                              clip_state(clip));
}

void merge_tracks(const Session& base,
                  const Session& incoming,
                  const MergePolicy& policy,
                  CommandBuilder& builder,
                  MergePlan& plan) {
    std::unordered_map<TrackId, const Track*, EntityIdHash> existing;
    for (const auto& track : base.timeline().tracks()) {
        existing.emplace(track.id, &track);
    }
    for (const auto& track : incoming.timeline().tracks()) {
        const auto found = existing.find(track.id);
        if (found == existing.end()) {
            emit_track(builder, track);
            record(plan, "track", track.id, MergeDisposition::added,
                   "track identifier is absent from the base timeline");
            continue;
        }
        const auto& current = *found->second;
        if (same_track(current, track)) {
            record(plan, "track", track.id, MergeDisposition::retained,
                   "track channel and mix settings are identical");
            continue;
        }
        if (should_replace(current, track, policy.preference) &&
            policy.preserve_track_mix) {
            builder.set_track_mix(track.id,
                                  track.gain,
                                  track.pan,
                                  track_state(track));
            record(plan, "track", track.id, MergeDisposition::replaced,
                   "incoming track mix was selected by policy");
        } else {
            record(plan, "track", track.id, MergeDisposition::conflict,
                   "track definitions differ and cannot be recreated in place");
        }
    }
}

void merge_clips(const Session& base,
                 const Session& incoming,
                 const MergePolicy& policy,
                 CommandBuilder& builder,
                 MergePlan& plan) {
    std::unordered_map<ClipId, const Clip*, EntityIdHash> existing;
    for (const auto& clip : base.timeline().clips()) {
        existing.emplace(clip.id, &clip);
    }
    for (const auto& clip : incoming.timeline().clips()) {
        const auto found = existing.find(clip.id);
        if (found == existing.end()) {
            emit_clip(builder, clip);
            record(plan, "clip", clip.id, MergeDisposition::added,
                   "clip identifier is absent from the base timeline");
            continue;
        }
        const auto& current = *found->second;
        if (same_clip(current, clip)) {
            record(plan, "clip", clip.id, MergeDisposition::retained,
                   "clip placement and envelope are identical");
            continue;
        }
        if (should_replace(current, clip, policy.preference)) {
            builder.move_clip(clip.id,
                              static_cast<std::int32_t>(clip.timeline_start),
                              static_cast<std::int32_t>(clip.source_start),
                              static_cast<std::uint32_t>(std::max<std::int64_t>(0, clip.length)));
            if (policy.preserve_clip_envelopes) {
                builder.set_clip_envelope(clip.id,
                                          clip.gain,
                                          clip.fade_in,
                                          clip.fade_out,
                                          clip_state(clip));
            }
            record(plan, "clip", clip.id, MergeDisposition::replaced,
                   "incoming clip placement was selected by policy");
        } else {
            record(plan, "clip", clip.id, MergeDisposition::conflict,
                   "clip definitions differ and base revision was retained");
        }
    }
}

void merge_tempo(const Session& base,
                 const Session& incoming,
                 CommandBuilder& builder,
                 MergePlan& plan) {
    std::unordered_map<EntityId, const TempoSegment*, EntityIdHash> existing;
    for (const auto& segment : base.timeline().tempo_segments()) {
        existing.emplace(segment.id, &segment);
    }
    for (const auto& segment : incoming.timeline().tempo_segments()) {
        const auto found = existing.find(segment.id);
        if (found == existing.end()) {
            builder.add_tempo(segment.id,
                              static_cast<std::int32_t>(segment.start_sample),
                              segment.start_bpm,
                              segment.end_bpm,
                              segment.numerator,
                              segment.denominator,
                              segment.ramp);
            record(plan, "tempo", segment.id, MergeDisposition::added,
                   "tempo segment is absent from the base timeline");
        } else if (same_tempo(*found->second, segment)) {
            record(plan, "tempo", segment.id, MergeDisposition::retained,
                   "tempo segment is identical");
        } else {
            record(plan, "tempo", segment.id, MergeDisposition::conflict,
                   "tempo segment differs; destructive replacement is not automatic");
        }
    }
}

void merge_lanes(const Session& base,
                 const Session& incoming,
                 CommandBuilder& builder,
                 MergePlan& plan) {
    std::unordered_map<LaneId, const AutomationLane*, EntityIdHash> existing;
    for (const auto& lane : base.timeline().automation_lanes()) {
        existing.emplace(lane.id, &lane);
    }
    for (const auto& lane : incoming.timeline().automation_lanes()) {
        const auto found = existing.find(lane.id);
        if (found == existing.end()) {
            builder.add_automation_lane(lane.id,
                                        lane.track_id,
                                        lane.parameter,
                                        lane.default_value,
                                        !lane.enabled);
            std::vector<std::int32_t> samples;
            std::vector<float> values;
            samples.reserve(lane.points.size());
            values.reserve(lane.points.size());
            for (const auto& point : lane.points) {
                samples.push_back(static_cast<std::int32_t>(point.sample));
                values.push_back(point.value);
            }
            if (!samples.empty()) {
                builder.add_automation_points(lane.id, samples, values);
            }
            record(plan, "automation", lane.id, MergeDisposition::added,
                   "automation lane is absent from the base timeline");
        } else if (same_lane(*found->second, lane)) {
            record(plan, "automation", lane.id, MergeDisposition::retained,
                   "automation points and defaults are identical");
        } else {
            record(plan, "automation", lane.id, MergeDisposition::conflict,
                   "automation lane differs; point replacement requires explicit editing");
        }
    }
}

void merge_markers(const Session& base,
                   const Session& incoming,
                   CommandBuilder& builder,
                   MergePlan& plan) {
    std::unordered_map<MarkerId, const Marker*, EntityIdHash> existing;
    for (const auto& marker : base.timeline().markers()) {
        existing.emplace(marker.id, &marker);
    }
    for (const auto& marker : incoming.timeline().markers()) {
        const auto found = existing.find(marker.id);
        if (found == existing.end()) {
            builder.add_marker(marker.id,
                               static_cast<std::int32_t>(marker.position),
                               static_cast<std::uint32_t>(std::max<std::int64_t>(0, marker.duration)),
                               marker.name,
                               marker.note,
                               marker.color);
            record(plan, "marker", marker.id, MergeDisposition::added,
                   "marker is absent from the base timeline");
        } else if (same_marker(*found->second, marker)) {
            record(plan, "marker", marker.id, MergeDisposition::retained,
                   "marker position and annotation are identical");
        } else {
            record(plan, "marker", marker.id, MergeDisposition::conflict,
                   "marker annotations differ");
        }
    }
}

void merge_takes(const Session& base,
                 const Session& incoming,
                 CommandBuilder& builder,
                 MergePlan& plan) {
    std::unordered_map<TakeId, const Take*, EntityIdHash> existing;
    for (const auto& take : base.timeline().takes()) {
        existing.emplace(take.id, &take);
    }
    for (const auto& take : incoming.timeline().takes()) {
        const auto found = existing.find(take.id);
        if (found == existing.end()) {
            builder.add_take(take.id,
                             take.track_id,
                             take.media_id,
                             static_cast<std::int32_t>(take.source_start),
                             static_cast<std::uint32_t>(std::max<std::int64_t>(0, take.length)),
                             take.lane,
                             take.rating,
                             take_state(take));
            record(plan, "take", take.id, MergeDisposition::added,
                   "take is absent from the base timeline");
        } else if (same_take(*found->second, take)) {
            record(plan, "take", take.id, MergeDisposition::retained,
                   "take source range and rating are identical");
        } else {
            record(plan, "take", take.id, MergeDisposition::conflict,
                   "take definitions differ");
        }
    }
}

void merge_timeline(const Session& base,
                    const Session& incoming,
                    const MergePolicy& policy,
                    CommandBuilder& builder,
                    MergePlan& plan) {
    if (!policy.include_timeline) {
        return;
    }
    merge_tracks(base, incoming, policy, builder, plan);
    merge_clips(base, incoming, policy, builder, plan);
    merge_tempo(base, incoming, builder, plan);
    merge_lanes(base, incoming, builder, plan);
    merge_markers(base, incoming, builder, plan);
    merge_takes(base, incoming, builder, plan);
}

bool same_plugin(const PluginNode& lhs, const PluginNode& rhs) {
    return lhs.track_id == rhs.track_id &&
           text_equal(lhs.vendor, rhs.vendor) &&
           text_equal(lhs.product, rhs.product) &&
           lhs.parameters == rhs.parameters &&
           lhs.latency == rhs.latency &&
           lhs.bypassed == rhs.bypassed &&
           lhs.instrument == rhs.instrument;
}

void merge_routing(const Session& base,
                   const Session& incoming,
                   const MergePolicy& policy,
                   CommandBuilder& builder,
                   MergePlan& plan) {
    if (!policy.include_routing) {
        return;
    }
    std::unordered_map<PluginId, const PluginNode*, EntityIdHash> existing;
    for (const auto& plugin : base.routing().plugins()) {
        existing.emplace(plugin.id, &plugin);
    }
    for (const auto& plugin : incoming.routing().plugins()) {
        const auto found = existing.find(plugin.id);
        if (found == existing.end()) {
            std::uint8_t state = 0;
            if (plugin.bypassed) {
                state |= 0x01U;
            }
            if (plugin.instrument) {
                state |= 0x02U;
            }
            builder.add_plugin(plugin.id,
                               plugin.track_id,
                               plugin.vendor,
                               plugin.product,
                               plugin.parameters,
                               plugin.latency,
                               state);
            record(plan, "plugin", plugin.id, MergeDisposition::added,
                   "plugin instance is absent from the base routing graph");
        } else if (same_plugin(*found->second, plugin)) {
            record(plan, "plugin", plugin.id, MergeDisposition::retained,
                   "plugin identity and parameters are identical");
        } else {
            record(plan, "plugin", plugin.id, MergeDisposition::conflict,
                   "plugin state differs and cannot be replaced without removing graph edges");
        }
    }

    std::unordered_set<std::string> edges;
    for (const auto& edge : base.routing().edges()) {
        std::ostringstream key;
        key << edge.source.value() << ':' << edge.destination.value() << ':'
            << edge.source_bus << ':' << edge.destination_bus;
        edges.insert(key.str());
    }
    for (const auto& edge : incoming.routing().edges()) {
        std::ostringstream key;
        key << edge.source.value() << ':' << edge.destination.value() << ':'
            << edge.source_bus << ':' << edge.destination_bus;
        if (edges.contains(key.str())) {
            record(plan, "plugin-edge", edge.source, MergeDisposition::retained,
                   "routing edge already exists");
            continue;
        }
        builder.connect_plugin(edge.source,
                               edge.destination,
                               edge.source_bus,
                               edge.destination_bus,
                               edge.gain,
                               edge.sidechain);
        record(plan, "plugin-edge", edge.source, MergeDisposition::added,
               "routing edge is absent from the base graph");
    }

    std::unordered_map<EntityId, const SendRoute*, EntityIdHash> sends;
    for (const auto& send : base.routing().sends()) {
        sends.emplace(send.id, &send);
    }
    for (const auto& send : incoming.routing().sends()) {
        const auto found = sends.find(send.id);
        if (found == sends.end()) {
            std::uint8_t state = 0;
            if (send.pre_fader) state |= 0x01U;
            if (send.muted) state |= 0x02U;
            builder.add_send(send.id,
                             send.source,
                             send.destination,
                             send.level,
                             send.pan,
                             state);
            record(plan, "send", send.id, MergeDisposition::added,
                   "send route is absent from the base graph");
        } else {
            const auto& current = *found->second;
            const bool equal = current.source == send.source &&
                               current.destination == send.destination &&
                               float_equal(current.level, send.level) &&
                               float_equal(current.pan, send.pan) &&
                               current.pre_fader == send.pre_fader &&
                               current.muted == send.muted;
            record(plan, "send", send.id,
                   equal ? MergeDisposition::retained : MergeDisposition::conflict,
                   equal ? "send route is identical" : "send route differs");
        }
    }
}

void merge_rendering(const Session& base,
                     const Session& incoming,
                     const MergePolicy& policy,
                     CommandBuilder& builder,
                     MergePlan& plan) {
    if (!policy.include_rendering) {
        return;
    }
    std::unordered_map<EntityId, const RenderJob*, EntityIdHash> jobs;
    for (const auto& job : base.rendering().jobs()) {
        jobs.emplace(job.id, &job);
    }
    for (const auto& job : incoming.rendering().jobs()) {
        const auto found = jobs.find(job.id);
        if (found == jobs.end()) {
            std::uint8_t state = 0;
            if (job.normalize) {
                state |= 0x01U;
            }
            if (job.dither) {
                state |= 0x02U;
            }
            builder.add_render_job(job.id,
                                   job.track_id,
                                   static_cast<std::int32_t>(job.start_sample),
                                   static_cast<std::int32_t>(job.end_sample),
                                   job.sample_rate,
                                   job.channels,
                                   job.priority,
                                   state);
            record(plan, "render-job", job.id, MergeDisposition::added,
                   "render job is absent from the base plan");
        } else {
            const auto& current = *found->second;
            const bool equal = current.track_id == job.track_id &&
                               current.start_sample == job.start_sample &&
                               current.end_sample == job.end_sample &&
                               current.sample_rate == job.sample_rate &&
                               current.channels == job.channels &&
                               current.priority == job.priority &&
                               current.normalize == job.normalize &&
                               current.dither == job.dither;
            record(plan, "render-job", job.id,
                   equal ? MergeDisposition::retained : MergeDisposition::conflict,
                   equal ? "render job is identical" : "render job definition differs");
        }
    }
}

void merge_history(const Session& base,
                   const Session& incoming,
                   const MergePolicy& policy,
                   CommandBuilder& builder,
                   MergePlan& plan) {
    if (!policy.include_history) {
        return;
    }
    std::unordered_map<EntityId, const ProjectRecord*, EntityIdHash> records;
    for (const auto& item : base.history().records()) {
        records.emplace(item.id, &item);
    }
    for (const auto& item : incoming.history().records()) {
        const auto found = records.find(item.id);
        if (found == records.end()) {
            builder.add_project_record(item.id,
                                       item.key,
                                       item.value,
                                       item.tombstone);
            record(plan, "project-record", item.id, MergeDisposition::added,
                   "project record is absent from the base history");
        } else {
            const auto& current = *found->second;
            const bool equal = current.key == item.key &&
                               current.value == item.value &&
                               current.tombstone == item.tombstone;
            if (equal) {
                record(plan, "project-record", item.id, MergeDisposition::retained,
                       "project record is identical");
            } else if (should_replace(current, item, policy.preference)) {
                builder.add_project_record(item.id,
                                           item.key,
                                           item.value,
                                           item.tombstone);
                record(plan, "project-record", item.id, MergeDisposition::replaced,
                       "incoming project record was selected by policy");
            } else {
                record(plan, "project-record", item.id, MergeDisposition::conflict,
                       "project record differs and base revision was retained");
            }
        }
    }
}

void merge_package_sections(const Session& base,
                            const Session& incoming,
                            const MergePolicy& policy,
                            CommandBuilder& builder,
                            MergePlan& plan) {
    if (!policy.include_package_sections) {
        return;
    }
    std::unordered_multimap<std::string, const PackageSection*> existing;
    for (const auto& section : base.package().document().sections) {
        existing.emplace(section_tag_string(section.tag), &section);
    }
    for (const auto& section : incoming.package().document().sections) {
        const auto tag = section_tag_string(section.tag);
        bool exact = false;
        const auto range = existing.equal_range(tag);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second->flags == section.flags &&
                it->second->payload == section.payload) {
                exact = true;
                break;
            }
        }
        if (exact) {
            record(plan, "package-section", EntityId::from_name("section", tag),
                   MergeDisposition::retained,
                   "an equivalent package section already exists");
            continue;
        }
        builder.add_package_section(tag, section.payload, section.flags);
        record(plan, "package-section", EntityId::from_name("section", tag),
               MergeDisposition::added,
               "package section payload is absent from the base workspace");
    }
}

}  // namespace

bool MergePlan::clean() const noexcept {
    return conflicts == 0;
}

std::string MergePlan::summary_text() const {
    std::ostringstream out;
    out << "WaveCairn merge plan\n";
    out << "  commands: " << commands.commands.size() << '\n';
    out << "  additions: " << additions << '\n';
    out << "  replacements: " << replacements << '\n';
    out << "  retained: " << retained << '\n';
    out << "  conflicts: " << conflicts << '\n';
    return out.str();
}

MergePlan plan_merge(const Session& base,
                     const Session& incoming,
                     const MergePolicy& policy) {
    MergePlan plan;
    CommandBuilder builder(static_cast<std::uint32_t>(
        (base.fingerprint() ^ incoming.fingerprint()) & 0xffffffffULL));

    merge_media(base, incoming, policy, builder, plan);
    merge_timeline(base, incoming, policy, builder, plan);
    merge_routing(base, incoming, policy, builder, plan);
    merge_rendering(base, incoming, policy, builder, plan);
    merge_history(base, incoming, policy, builder, plan);
    merge_package_sections(base, incoming, policy, builder, plan);

    plan.commands = builder.take();
    plan.commands.header.session_seed = static_cast<std::uint32_t>(base.metadata().id.value());
    plan.commands.header.flags = 0;
    return plan;
}

Result<ProcessingReport> apply_merge(Session& base,
                                     const MergePlan& plan,
                                     Diagnostics& diagnostics,
                                     bool canonicalize) {
    ProcessingOptions options;
    options.profile = ProcessingProfile::complete;
    options.stop_on_error = false;
    options.validate_barriers = true;
    options.canonicalize_at_end = canonicalize;
    options.maximum_commands = std::max<std::size_t>(4096, plan.commands.commands.size() + 16);
    const auto bytes = serialize_command_tape(plan.commands);
    return process_command_tape(base, bytes, diagnostics, options);
}

std::string merge_disposition_name(MergeDisposition disposition) {
    switch (disposition) {
        case MergeDisposition::added:
            return "added";
        case MergeDisposition::retained:
            return "retained";
        case MergeDisposition::replaced:
            return "replaced";
        case MergeDisposition::skipped:
            return "skipped";
        case MergeDisposition::conflict:
            return "conflict";
    }
    return "unknown";
}

std::string merge_decisions_csv(const MergePlan& plan) {
    std::ostringstream out;
    out << "area,id,disposition,reason\n";
    for (const auto& decision : plan.decisions) {
        out << csv_escape_local(decision.area) << ','
            << csv_escape_local(decision.id.to_string()) << ','
            << csv_escape_local(merge_disposition_name(decision.disposition)) << ','
            << csv_escape_local(decision.reason) << '\n';
    }
    return out.str();
}

}  // namespace wavecairn
