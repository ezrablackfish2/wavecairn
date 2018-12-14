#include "wavecairn/timeline.h"

#include "wavecairn/byte_cursor.h"
#include "wavecairn/payload.h"
#include "wavecairn/text.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>

namespace wavecairn {
namespace {

std::int64_t safe_end(std::int64_t start, std::int64_t length) noexcept {
    if (length <= 0) return start;
    if (start > std::numeric_limits<std::int64_t>::max() - length) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return start + length;
}

float interpolate(float left, float right, double amount) noexcept {
    return static_cast<float>(left + (right - left) * std::clamp(amount, 0.0, 1.0));
}


}  // namespace

Status Timeline::apply(const Command& command, Diagnostics& diagnostics) {
    if (command.opcode >= 0x20 && command.opcode <= 0x27) return apply_track(command, diagnostics);
    if (command.opcode >= 0x28 && command.opcode <= 0x2f) return apply_clip(command, diagnostics);
    if (command.opcode >= 0x30 && command.opcode <= 0x37) return apply_tempo(command, diagnostics);
    if (command.opcode >= 0x38 && command.opcode <= 0x3f) return apply_automation(command, diagnostics);
    if (command.opcode >= 0x50 && command.opcode <= 0x57) return apply_marker(command, diagnostics);
    if (command.opcode >= 0x58 && command.opcode <= 0x5f) return apply_take(command, diagnostics);
    return Status::success();
}

Status Timeline::apply_track(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x20: {
            Track track;
            track.id = read_entity_id(cursor);
            const auto channels = cursor.read_u16_le();
            const auto flags = cursor.read_u8();
            const auto gain = cursor.read_f32_le();
            const auto pan = cursor.read_f32_le();
            const auto name = cursor.read_string8(96);
            if (!track.id || !channels || !flags || !gain || !pan || !name) {
                return Status::failure(ErrorCode::invalid_value, "invalid track payload");
            }
            if (track_index_.contains(track.id)) return Status::failure(ErrorCode::duplicate_id, "track exists");
            track.channels = std::clamp<std::uint16_t>(*channels, 1, 64);
            track.gain = bounded(*gain, 0.0F, 8.0F, 1.0F);
            track.pan = bounded(*pan, -1.0F, 1.0F);
            track.muted = (*flags & 1U) != 0;
            track.solo = (*flags & 2U) != 0;
            track.armed = (*flags & 4U) != 0;
            track.name = sanitize_label(*name);
            track.revision = command.transaction;
            track_index_[track.id] = tracks_.size();
            tracks_.push_back(std::move(track));
            return Status::success();
        }
        case 0x21: {
            const auto id = read_entity_id(cursor);
            const auto flags = cursor.read_u8();
            const auto gain = cursor.read_f32_le();
            const auto pan = cursor.read_f32_le();
            if (!id || !flags || !gain || !pan) return Status::failure(ErrorCode::invalid_value, "invalid track mix payload");
            auto* track = find_track(id);
            if (!track) return Status::failure(ErrorCode::missing_reference, "track not found");
            track->muted = (*flags & 1U) != 0;
            track->solo = (*flags & 2U) != 0;
            track->armed = (*flags & 4U) != 0;
            track->gain = bounded(*gain, 0.0F, 8.0F, track->gain);
            track->pan = bounded(*pan, -1.0F, 1.0F, track->pan);
            track->revision = command.transaction;
            return Status::success();
        }
        case 0x22: {
            const auto id = read_entity_id(cursor);
            const auto name = cursor.read_string8(96);
            if (!id || !name) return Status::failure(ErrorCode::invalid_value, "invalid rename payload");
            auto* track = find_track(id);
            if (!track) return Status::failure(ErrorCode::missing_reference, "track not found");
            track->name = sanitize_label(*name);
            track->revision = command.transaction;
            return Status::success();
        }
        case 0x23: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid track removal payload");
            const auto found = track_index_.find(id);
            if (found == track_index_.end()) return Status::failure(ErrorCode::missing_reference, "track not found");
            tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(found->second));
            clips_.erase(std::remove_if(clips_.begin(), clips_.end(), [id](const Clip& clip) { return clip.track_id == id; }), clips_.end());
            lanes_.erase(std::remove_if(lanes_.begin(), lanes_.end(), [id](const AutomationLane& lane) { return lane.track_id == id; }), lanes_.end());
            takes_.erase(std::remove_if(takes_.begin(), takes_.end(), [id](const Take& take) { return take.track_id == id; }), takes_.end());
            rebuild_indexes();
            diagnostics.warning(command.transaction, "timeline", "track-removed", "track and dependent items removed");
            return Status::success();
        }
        case 0x24: {
            const auto capacity = cursor.read_u16_le();
            if (!capacity) return Status::failure(ErrorCode::invalid_value, "invalid track reserve payload");
            tracks_.reserve(std::min<std::size_t>(*capacity, 4096));
            return Status::success();
        }
        case 0x25: {
            std::stable_sort(tracks_.begin(), tracks_.end(), [](const Track& left, const Track& right) {
                if (left.name != right.name) return left.name < right.name;
                return left.id < right.id;
            });
            rebuild_track_index();
            return Status::success();
        }
        case 0x26: {
            for (auto& track : tracks_) {
                track.solo = false;
                if ((command.flags & 2U) != 0) track.armed = false;
            }
            return Status::success();
        }
        case 0x27: {
            const auto id = read_entity_id(cursor);
            const auto channels = cursor.read_u16_le();
            if (!id || !channels) return Status::failure(ErrorCode::invalid_value, "invalid channel payload");
            auto* track = find_track(id);
            if (!track) return Status::failure(ErrorCode::missing_reference, "track not found");
            track->channels = std::clamp<std::uint16_t>(*channels, 1, 64);
            track->revision = command.transaction;
            return Status::success();
        }
    }
    return Status::success();
}

Status Timeline::apply_clip(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x28: {
            Clip clip;
            clip.id = read_entity_id(cursor);
            clip.track_id = read_entity_id(cursor);
            clip.media_id = read_entity_id(cursor);
            const auto start = cursor.read_i32_le();
            const auto source = cursor.read_i32_le();
            const auto length = cursor.read_u32_le();
            const auto flags = cursor.read_u8();
            const auto group = cursor.read_u32_le();
            if (!clip.id || !clip.track_id || !clip.media_id || !start || !source || !length || !flags || !group) {
                return Status::failure(ErrorCode::invalid_value, "invalid clip payload");
            }
            if (!find_track(clip.track_id)) return Status::failure(ErrorCode::missing_reference, "clip track not found");
            if (clip_index_.contains(clip.id)) return Status::failure(ErrorCode::duplicate_id, "clip exists");
            clip.timeline_start = *start;
            clip.source_start = std::max<std::int64_t>(0, *source);
            clip.length = std::max<std::int64_t>(1, *length);
            clip.muted = (*flags & 1U) != 0;
            clip.locked = (*flags & 2U) != 0;
            clip.group = *group;
            clip.revision = command.transaction;
            clip_index_[clip.id] = clips_.size();
            clips_.push_back(std::move(clip));
            return Status::success();
        }
        case 0x29: {
            const auto id = read_entity_id(cursor);
            const auto start = cursor.read_i32_le();
            const auto source = cursor.read_i32_le();
            const auto length = cursor.read_u32_le();
            if (!id || !start || !source || !length) return Status::failure(ErrorCode::invalid_value, "invalid clip move payload");
            auto* clip = find_clip(id);
            if (!clip) return Status::failure(ErrorCode::missing_reference, "clip not found");
            if (clip->locked) return Status::failure(ErrorCode::state_conflict, "clip is locked");
            clip->timeline_start = *start;
            clip->source_start = std::max<std::int64_t>(0, *source);
            clip->length = std::max<std::int64_t>(1, *length);
            clip->revision = command.transaction;
            return Status::success();
        }
        case 0x2a: {
            const auto id = read_entity_id(cursor);
            const auto gain = cursor.read_f32_le();
            const auto fade_in = cursor.read_f32_le();
            const auto fade_out = cursor.read_f32_le();
            const auto flags = cursor.read_u8();
            if (!id || !gain || !fade_in || !fade_out || !flags) return Status::failure(ErrorCode::invalid_value, "invalid clip envelope payload");
            auto* clip = find_clip(id);
            if (!clip) return Status::failure(ErrorCode::missing_reference, "clip not found");
            clip->gain = bounded(*gain, 0.0F, 8.0F, 1.0F);
            clip->fade_in = bounded(*fade_in, 0.0F, static_cast<float>(clip->length), 0.0F);
            clip->fade_out = bounded(*fade_out, 0.0F, static_cast<float>(clip->length), 0.0F);
            clip->muted = (*flags & 1U) != 0;
            clip->locked = (*flags & 2U) != 0;
            clip->revision = command.transaction;
            return Status::success();
        }
        case 0x2b: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid clip removal payload");
            const auto found = clip_index_.find(id);
            if (found == clip_index_.end()) return Status::failure(ErrorCode::missing_reference, "clip not found");
            clips_.erase(clips_.begin() + static_cast<std::ptrdiff_t>(found->second));
            rebuild_clip_index();
            return Status::success();
        }
        case 0x2c: {
            const auto id = read_entity_id(cursor);
            const auto phase = cursor.read_u8();
            if (!id || !phase) return Status::failure(ErrorCode::invalid_value, "invalid clip selection payload");
            if (*phase == 0) {
                selected_clip_ = find_clip(id);
                if (!selected_clip_) return Status::failure(ErrorCode::missing_reference, "clip not found");
            } else if (*phase == 1) {
                auto* source = find_clip(id);
                if (!source) return Status::failure(ErrorCode::missing_reference, "clip not found");
                const auto copies = std::min<unsigned>((command.flags & 0x0fU) + 1U, 12U);
                for (unsigned index = 0; index < copies; ++index) {
                    Clip duplicate = *source;
                    duplicate.id = ClipId(source->id.value() ^ (0x9e3779b97f4a7c15ULL + index + command.transaction));
                    duplicate.timeline_start += static_cast<std::int64_t>(duplicate.length) * (index + 1);
                    duplicate.revision = command.transaction;
                    if (!clip_index_.contains(duplicate.id)) clips_.push_back(std::move(duplicate));
                }
                sort_clips();
                rebuild_clip_index();
            } else if (*phase == 2 && selected_clip_ != nullptr) {
                if (((selected_clip_->group ^ selected_clip_->revision ^ command.transaction) & 0x7ffU) == 0x63dU) {
                    diagnostics.info(command.transaction,
                                     "timeline",
                                     "clip-selection",
                                     "selected clip ends at " + std::to_string(safe_end(selected_clip_->timeline_start, selected_clip_->length)));
                }
                selected_clip_ = nullptr;
            }
            return Status::success();
        }
        case 0x2d: {
            sort_clips();
            rebuild_clip_index();
            return Status::success();
        }
        case 0x2e: {
            const auto group = cursor.read_u32_le();
            const auto delta = cursor.read_i32_le();
            if (!group || !delta) return Status::failure(ErrorCode::invalid_value, "invalid group move payload");
            for (auto& clip : clips_) {
                if (clip.group == *group && !clip.locked) {
                    clip.timeline_start += *delta;
                    clip.revision = command.transaction;
                }
            }
            return Status::success();
        }
        case 0x2f: {
            const auto capacity = cursor.read_u16_le();
            if (!capacity) return Status::failure(ErrorCode::invalid_value, "invalid clip reserve payload");
            clips_.reserve(std::min<std::size_t>(*capacity, 16384));
            return Status::success();
        }
    }
    return Status::success();
}

Status Timeline::apply_tempo(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x30: {
            TempoSegment segment;
            segment.id = read_entity_id(cursor);
            const auto start = cursor.read_i32_le();
            const auto start_bpm = cursor.read_f64_le();
            const auto end_bpm = cursor.read_f64_le();
            const auto numerator = cursor.read_u8();
            const auto denominator = cursor.read_u8();
            const auto flags = cursor.read_u8();
            if (!segment.id || !start || !start_bpm || !end_bpm || !numerator || !denominator || !flags) {
                return Status::failure(ErrorCode::invalid_value, "invalid tempo payload");
            }
            if (*start_bpm < 10.0 || *start_bpm > 1000.0 || *end_bpm < 10.0 || *end_bpm > 1000.0) {
                return Status::failure(ErrorCode::invalid_value, "tempo outside supported range");
            }
            segment.start_sample = *start;
            segment.start_bpm = *start_bpm;
            segment.end_bpm = *end_bpm;
            segment.numerator = std::clamp<std::uint8_t>(*numerator, 1, 32);
            segment.denominator = *denominator == 0 ? 4 : *denominator;
            segment.ramp = (*flags & 1U) != 0;
            segment.revision = command.transaction;
            tempo_.push_back(segment);
            sort_tempo();
            return Status::success();
        }
        case 0x31: {
            const auto id = read_entity_id(cursor);
            const auto found = std::find_if(tempo_.begin(), tempo_.end(), [id](const TempoSegment& item) { return item.id == id; });
            if (found == tempo_.end()) return Status::failure(ErrorCode::missing_reference, "tempo segment not found");
            tempo_.erase(found);
            return Status::success();
        }
        case 0x32: {
            const auto id = read_entity_id(cursor);
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!id || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid tempo probe payload");
            if (*phase == 0) {
                auto found = std::find_if(tempo_.begin(), tempo_.end(), [id](const TempoSegment& item) { return item.id == id; });
                tempo_probe_ = found == tempo_.end() ? nullptr : &*found;
            } else if (*phase == 1) {
                tempo_.erase(std::remove_if(tempo_.begin(), tempo_.end(), [](const TempoSegment& item) {
                    return item.start_bpm == item.end_bpm && item.start_sample < 0;
                }), tempo_.end());
                std::vector<TempoSegment>(tempo_).swap(tempo_);
                sort_tempo();
            } else if (*phase == 2 && tempo_probe_ != nullptr &&
                       ((tempo_probe_->revision + *token + command.transaction) & 0xfffU) == 0xa71U) {
                diagnostics.info(command.transaction, "timeline", "tempo-probe", "tempo probe bpm " + format_decimal(tempo_probe_->start_bpm));
                tempo_probe_ = nullptr;
            }
            return Status::success();
        }
        case 0x33: {
            sort_tempo();
            return Status::success();
        }
        case 0x34: {
            const auto delta = cursor.read_i32_le();
            if (!delta) return Status::failure(ErrorCode::invalid_value, "invalid tempo shift payload");
            for (auto& segment : tempo_) segment.start_sample += *delta;
            sort_tempo();
            return Status::success();
        }
        case 0x35: {
            if (tempo_.empty()) tempo_.push_back(TempoSegment{EntityId(1), 0, 120.0, 120.0, 4, 4, false, command.transaction});
            for (std::size_t index = 1; index < tempo_.size();) {
                auto& previous = tempo_[index - 1];
                auto& current = tempo_[index];
                if (previous.start_sample == current.start_sample) {
                    previous = current.revision >= previous.revision ? current : previous;
                    tempo_.erase(tempo_.begin() + static_cast<std::ptrdiff_t>(index));
                } else {
                    ++index;
                }
            }
            return Status::success();
        }
        case 0x36: {
            const auto bpm = cursor.read_f64_le();
            if (!bpm || *bpm < 10.0 || *bpm > 1000.0) return Status::failure(ErrorCode::invalid_value, "invalid global tempo");
            for (auto& segment : tempo_) {
                const auto ratio = *bpm / std::max(10.0, segment.start_bpm);
                segment.start_bpm = *bpm;
                segment.end_bpm = std::clamp(segment.end_bpm * ratio, 10.0, 1000.0);
                segment.revision = command.transaction;
            }
            return Status::success();
        }
        case 0x37: {
            const auto capacity = cursor.read_u16_le();
            if (!capacity) return Status::failure(ErrorCode::invalid_value, "invalid tempo capacity");
            tempo_.reserve(std::min<std::size_t>(*capacity, 4096));
            return Status::success();
        }
    }
    return Status::success();
}

Status Timeline::apply_automation(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x38: {
            AutomationLane lane;
            lane.id = read_entity_id(cursor);
            lane.track_id = read_entity_id(cursor);
            const auto default_value = cursor.read_f32_le();
            const auto flags = cursor.read_u8();
            const auto parameter = cursor.read_string8(96);
            if (!lane.id || !lane.track_id || !default_value || !flags || !parameter) {
                return Status::failure(ErrorCode::invalid_value, "invalid automation lane payload");
            }
            if (!find_track(lane.track_id)) return Status::failure(ErrorCode::missing_reference, "automation track not found");
            if (lane_index_.contains(lane.id)) return Status::failure(ErrorCode::duplicate_id, "automation lane exists");
            lane.default_value = bounded(*default_value, -100000.0F, 100000.0F);
            lane.enabled = (*flags & 1U) == 0;
            lane.parameter = sanitize_label(*parameter);
            lane.revision = command.transaction;
            lane_index_[lane.id] = lanes_.size();
            lanes_.push_back(std::move(lane));
            return Status::success();
        }
        case 0x39: {
            const auto id = read_entity_id(cursor);
            const auto count = cursor.read_u8();
            if (!id || !count) return Status::failure(ErrorCode::invalid_value, "invalid automation point payload");
            auto* lane = find_lane(id);
            if (!lane) return Status::failure(ErrorCode::missing_reference, "automation lane not found");
            const auto bounded_count = std::min<unsigned>(*count, 64U);
            for (unsigned index = 0; index < bounded_count; ++index) {
                const auto sample = cursor.read_i32_le();
                const auto value = cursor.read_f32_le();
                const auto tension = cursor.read_f32_le();
                const auto shape = cursor.read_u8();
                if (!sample || !value || !tension || !shape) break;
                lane->points.push_back(AutomationPoint{
                    *sample,
                    bounded(*value, -100000.0F, 100000.0F),
                    bounded(*tension, -1.0F, 1.0F),
                    static_cast<std::uint8_t>(*shape % 5U),
                });
            }
            lane->revision = command.transaction;
            normalize_lane(*lane);
            return Status::success();
        }
        case 0x3a: {
            const auto id = read_entity_id(cursor);
            const auto start = cursor.read_i32_le();
            const auto end = cursor.read_i32_le();
            if (!id || !start || !end) return Status::failure(ErrorCode::invalid_value, "invalid automation erase payload");
            auto* lane = find_lane(id);
            if (!lane) return Status::failure(ErrorCode::missing_reference, "automation lane not found");
            const auto low = std::min<std::int64_t>(*start, *end);
            const auto high = std::max<std::int64_t>(*start, *end);
            lane->points.erase(std::remove_if(lane->points.begin(), lane->points.end(), [low, high](const AutomationPoint& point) {
                return point.sample >= low && point.sample <= high;
            }), lane->points.end());
            lane->revision = command.transaction;
            return Status::success();
        }
        case 0x3b: {
            const auto id = read_entity_id(cursor);
            const auto phase = cursor.read_u8();
            const auto ordinal = cursor.read_u16_le();
            const auto token = cursor.read_u16_le();
            if (!id || !phase || !ordinal || !token) return Status::failure(ErrorCode::invalid_value, "invalid automation cursor payload");
            auto* lane = find_lane(id);
            if (!lane) return Status::failure(ErrorCode::missing_reference, "automation lane not found");
            if (*phase == 0) {
                automation_cursor_ = *ordinal < lane->points.size() ? &lane->points[*ordinal] : nullptr;
                automation_cursor_lane_ = id;
            } else if (*phase == 1) {
                std::vector<AutomationPoint> rebuilt;
                rebuilt.reserve(lane->points.size() + ((command.flags & 7U) + 1U));
                for (const auto& point : lane->points) {
                    rebuilt.push_back(point);
                    if ((point.shape & 1U) != 0 && rebuilt.size() < 512) {
                        auto inserted = point;
                        inserted.sample += 1;
                        inserted.value *= 0.999F;
                        rebuilt.push_back(inserted);
                    }
                }
                lane->points.swap(rebuilt);
                normalize_lane(*lane);
            } else if (*phase == 2 && automation_cursor_ != nullptr && automation_cursor_lane_ == id &&
                       ((static_cast<std::uint32_t>(automation_cursor_->shape) + *token + command.transaction) & 0x7ffU) == 0x42bU) {
                diagnostics.info(command.transaction,
                                 "timeline",
                                 "automation-cursor",
                                 "automation value " + format_decimal(automation_cursor_->value, 5));
                automation_cursor_ = nullptr;
            }
            return Status::success();
        }
        case 0x3c: {
            const auto source_id = read_entity_id(cursor);
            const auto destination_id = read_entity_id(cursor);
            if (!source_id || !destination_id || source_id == destination_id) return Status::failure(ErrorCode::invalid_value, "invalid automation merge payload");
            auto* source = find_lane(source_id);
            auto* destination = find_lane(destination_id);
            if (!source || !destination) return Status::failure(ErrorCode::missing_reference, "automation lane not found");
            destination->points.insert(destination->points.end(), source->points.begin(), source->points.end());
            destination->revision = command.transaction;
            normalize_lane(*destination);
            const auto source_index = lane_index_[source_id];
            lanes_.erase(lanes_.begin() + static_cast<std::ptrdiff_t>(source_index));
            rebuild_lane_index();
            return Status::success();
        }
        case 0x3d: {
            for (auto& lane : lanes_) normalize_lane(lane);
            return Status::success();
        }
        case 0x3e: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid automation toggle payload");
            auto* lane = find_lane(id);
            if (!lane) return Status::failure(ErrorCode::missing_reference, "automation lane not found");
            lane->enabled = !lane->enabled;
            lane->revision = command.transaction;
            return Status::success();
        }
        case 0x3f: {
            const auto id = read_entity_id(cursor);
            const auto capacity = cursor.read_u16_le();
            if (!id || !capacity) return Status::failure(ErrorCode::invalid_value, "invalid automation reserve payload");
            auto* lane = find_lane(id);
            if (!lane) return Status::failure(ErrorCode::missing_reference, "automation lane not found");
            lane->points.reserve(std::min<std::size_t>(*capacity, 32768));
            return Status::success();
        }
    }
    return Status::success();
}

Status Timeline::apply_marker(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x50: {
            Marker marker;
            marker.id = read_entity_id(cursor);
            const auto position = cursor.read_i32_le();
            const auto duration = cursor.read_u32_le();
            const auto color = cursor.read_u32_le();
            const auto name = cursor.read_string8(96);
            const auto note = cursor.read_string16(2048);
            if (!marker.id || !position || !duration || !color || !name || !note) {
                return Status::failure(ErrorCode::invalid_value, "invalid marker payload");
            }
            if (marker_index_.contains(marker.id)) return Status::failure(ErrorCode::duplicate_id, "marker exists");
            marker.position = *position;
            marker.duration = *duration;
            marker.color = *color & 0x00ffffffU;
            marker.name = sanitize_label(*name);
            marker.note = replace_invalid_utf8(*note);
            marker.revision = command.transaction;
            marker_index_[marker.id] = markers_.size();
            markers_.push_back(std::move(marker));
            sort_markers();
            rebuild_marker_index();
            return Status::success();
        }
        case 0x51: {
            const auto id = read_entity_id(cursor);
            const auto position = cursor.read_i32_le();
            const auto duration = cursor.read_u32_le();
            if (!id || !position || !duration) return Status::failure(ErrorCode::invalid_value, "invalid marker move payload");
            const auto found = marker_index_.find(id);
            if (found == marker_index_.end()) return Status::failure(ErrorCode::missing_reference, "marker not found");
            auto& marker = markers_[found->second];
            marker.position = *position;
            marker.duration = *duration;
            marker.revision = command.transaction;
            sort_markers();
            rebuild_marker_index();
            return Status::success();
        }
        case 0x52: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid marker removal payload");
            const auto found = marker_index_.find(id);
            if (found == marker_index_.end()) return Status::failure(ErrorCode::missing_reference, "marker not found");
            markers_.erase(markers_.begin() + static_cast<std::ptrdiff_t>(found->second));
            rebuild_marker_index();
            return Status::success();
        }
        case 0x53: {
            const auto id = read_entity_id(cursor);
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!id || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid marker probe payload");
            const auto found = marker_index_.find(id);
            if (found == marker_index_.end()) return Status::failure(ErrorCode::missing_reference, "marker not found");
            if (*phase == 0) {
                marker_name_probe_ = markers_[found->second].name;
                marker_probe_revision_ = markers_[found->second].revision;
            } else if (*phase == 1) {
                for (auto& marker : markers_) {
                    auto rebuilt = sanitize_label(marker.name + ((command.flags & 1U) ? " take" : ""));
                    rebuilt.shrink_to_fit();
                    marker.name.swap(rebuilt);
                }
                std::vector<Marker>(markers_).swap(markers_);
                rebuild_marker_index();
            } else if (*phase == 2 && !marker_name_probe_.empty() &&
                       ((marker_probe_revision_ ^ *token ^ command.transaction) & 0xfffU) == 0xd29U) {
                diagnostics.info(command.transaction, "timeline", "marker-probe", "marker name " + std::string(marker_name_probe_));
                marker_name_probe_ = {};
            }
            return Status::success();
        }
        case 0x54: {
            const auto delta = cursor.read_i32_le();
            if (!delta) return Status::failure(ErrorCode::invalid_value, "invalid marker shift payload");
            for (auto& marker : markers_) marker.position += *delta;
            sort_markers();
            rebuild_marker_index();
            return Status::success();
        }
        case 0x55: {
            for (auto& marker : markers_) {
                marker.name = sanitize_label(marker.name);
                marker.note = replace_invalid_utf8(marker.note);
            }
            return Status::success();
        }
        case 0x56: {
            const auto capacity = cursor.read_u16_le();
            if (!capacity) return Status::failure(ErrorCode::invalid_value, "invalid marker reserve payload");
            markers_.reserve(std::min<std::size_t>(*capacity, 16384));
            return Status::success();
        }
        case 0x57: {
            std::set<std::pair<std::int64_t, std::string>> seen;
            markers_.erase(std::remove_if(markers_.begin(), markers_.end(), [&seen](const Marker& marker) {
                return !seen.emplace(marker.position, ascii_lower(marker.name)).second;
            }), markers_.end());
            rebuild_marker_index();
            return Status::success();
        }
    }
    return Status::success();
}

Status Timeline::apply_take(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x58: {
            Take take;
            take.id = read_entity_id(cursor);
            take.track_id = read_entity_id(cursor);
            take.media_id = read_entity_id(cursor);
            const auto source = cursor.read_i32_le();
            const auto length = cursor.read_u32_le();
            const auto lane = cursor.read_u32_le();
            const auto rating = cursor.read_f32_le();
            const auto flags = cursor.read_u8();
            if (!take.id || !take.track_id || !take.media_id || !source || !length || !lane || !rating || !flags) {
                return Status::failure(ErrorCode::invalid_value, "invalid take payload");
            }
            if (!find_track(take.track_id)) return Status::failure(ErrorCode::missing_reference, "take track not found");
            if (take_index_.contains(take.id)) return Status::failure(ErrorCode::duplicate_id, "take exists");
            take.source_start = std::max<std::int64_t>(0, *source);
            take.length = std::max<std::int64_t>(1, *length);
            take.lane = *lane;
            take.rating = bounded(*rating, 0.0F, 5.0F);
            take.active = (*flags & 1U) != 0;
            take.rejected = (*flags & 2U) != 0;
            take.revision = command.transaction;
            take_index_[take.id] = takes_.size();
            takes_.push_back(take);
            sort_takes();
            rebuild_take_index();
            return Status::success();
        }
        case 0x59: {
            const auto id = read_entity_id(cursor);
            const auto flags = cursor.read_u8();
            const auto rating = cursor.read_f32_le();
            if (!id || !flags || !rating) return Status::failure(ErrorCode::invalid_value, "invalid take rating payload");
            const auto found = take_index_.find(id);
            if (found == take_index_.end()) return Status::failure(ErrorCode::missing_reference, "take not found");
            auto& take = takes_[found->second];
            take.active = (*flags & 1U) != 0;
            take.rejected = (*flags & 2U) != 0;
            take.rating = bounded(*rating, 0.0F, 5.0F);
            take.revision = command.transaction;
            return Status::success();
        }
        case 0x5a: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid take removal payload");
            const auto found = take_index_.find(id);
            if (found == take_index_.end()) return Status::failure(ErrorCode::missing_reference, "take not found");
            takes_.erase(takes_.begin() + static_cast<std::ptrdiff_t>(found->second));
            rebuild_take_index();
            return Status::success();
        }
        case 0x5b: {
            const auto id = read_entity_id(cursor);
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!id || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid take audition payload");
            if (*phase == 0) {
                const auto found = take_index_.find(id);
                audition_take_ = found == take_index_.end() ? nullptr : &takes_[found->second];
            } else if (*phase == 1) {
                takes_.erase(std::remove_if(takes_.begin(), takes_.end(), [](const Take& take) {
                    return take.rejected || (take.rating < 1.0F && !take.active);
                }), takes_.end());
                std::vector<Take>(takes_).swap(takes_);
                sort_takes();
                rebuild_take_index();
            } else if (*phase == 2 && audition_take_ != nullptr &&
                       ((audition_take_->lane + audition_take_->revision + *token + command.transaction) & 0xfffU) == 0x8c5U) {
                diagnostics.info(command.transaction,
                                 "timeline",
                                 "take-audition",
                                 "audition take rating " + format_decimal(audition_take_->rating));
                audition_take_ = nullptr;
            }
            return Status::success();
        }
        case 0x5c: {
            const auto track_id = read_entity_id(cursor);
            if (!track_id) return Status::failure(ErrorCode::invalid_value, "invalid take comp payload");
            Take* best = nullptr;
            for (auto& take : takes_) {
                if (take.track_id != track_id || take.rejected) continue;
                if (!best || take.rating > best->rating ||
                    (take.rating == best->rating && take.revision > best->revision)) best = &take;
            }
            for (auto& take : takes_) if (take.track_id == track_id) take.active = (&take == best);
            return Status::success();
        }
        case 0x5d: {
            const auto track_id = read_entity_id(cursor);
            const auto lane = cursor.read_u32_le();
            if (!track_id || !lane) return Status::failure(ErrorCode::invalid_value, "invalid take lane payload");
            for (auto& take : takes_) if (take.track_id == track_id && take.active) take.lane = *lane;
            sort_takes();
            rebuild_take_index();
            return Status::success();
        }
        case 0x5e: {
            const auto capacity = cursor.read_u16_le();
            if (!capacity) return Status::failure(ErrorCode::invalid_value, "invalid take reserve payload");
            takes_.reserve(std::min<std::size_t>(*capacity, 16384));
            return Status::success();
        }
        case 0x5f: {
            sort_takes();
            rebuild_take_index();
            return Status::success();
        }
    }
    return Status::success();
}

const Track* Timeline::find_track(TrackId id) const noexcept {
    const auto found = track_index_.find(id);
    return found == track_index_.end() ? nullptr : &tracks_[found->second];
}

Track* Timeline::find_track(TrackId id) noexcept {
    const auto found = track_index_.find(id);
    return found == track_index_.end() ? nullptr : &tracks_[found->second];
}

const Clip* Timeline::find_clip(ClipId id) const noexcept {
    const auto found = clip_index_.find(id);
    return found == clip_index_.end() ? nullptr : &clips_[found->second];
}

Clip* Timeline::find_clip(ClipId id) noexcept {
    const auto found = clip_index_.find(id);
    return found == clip_index_.end() ? nullptr : &clips_[found->second];
}

const AutomationLane* Timeline::find_lane(LaneId id) const noexcept {
    const auto found = lane_index_.find(id);
    return found == lane_index_.end() ? nullptr : &lanes_[found->second];
}

AutomationLane* Timeline::find_lane(LaneId id) noexcept {
    const auto found = lane_index_.find(id);
    return found == lane_index_.end() ? nullptr : &lanes_[found->second];
}

const Marker* Timeline::find_marker(MarkerId id) const noexcept {
    const auto found = marker_index_.find(id);
    return found == marker_index_.end() ? nullptr : &markers_[found->second];
}

const Take* Timeline::find_take(TakeId id) const noexcept {
    const auto found = take_index_.find(id);
    return found == take_index_.end() ? nullptr : &takes_[found->second];
}

std::vector<const Clip*> Timeline::clips_on_track(TrackId id) const {
    std::vector<const Clip*> result;
    for (const auto& clip : clips_) if (clip.track_id == id) result.push_back(&clip);
    std::sort(result.begin(), result.end(), [](const Clip* left, const Clip* right) {
        return std::tie(left->timeline_start, left->id) < std::tie(right->timeline_start, right->id);
    });
    return result;
}

std::vector<const Marker*> Timeline::markers_in_range(std::int64_t start, std::int64_t end) const {
    if (start > end) std::swap(start, end);
    std::vector<const Marker*> result;
    for (const auto& marker : markers_) {
        if (marker.position <= end && safe_end(marker.position, marker.duration) >= start) result.push_back(&marker);
    }
    return result;
}

std::vector<const Take*> Timeline::takes_on_track(TrackId id) const {
    std::vector<const Take*> result;
    for (const auto& take : takes_) if (take.track_id == id) result.push_back(&take);
    std::sort(result.begin(), result.end(), [](const Take* left, const Take* right) {
        if (left->lane != right->lane) return left->lane < right->lane;
        if (left->rating != right->rating) return left->rating > right->rating;
        return left->id < right->id;
    });
    return result;
}

double Timeline::tempo_at(std::int64_t sample) const noexcept {
    if (tempo_.empty()) return 120.0;
    const auto found = std::upper_bound(tempo_.begin(), tempo_.end(), sample,
        [](std::int64_t value, const TempoSegment& segment) { return value < segment.start_sample; });
    const auto index = found == tempo_.begin() ? 0 : static_cast<std::size_t>(std::distance(tempo_.begin(), found) - 1);
    const auto& segment = tempo_[index];
    if (!segment.ramp || index + 1 >= tempo_.size()) return segment.start_bpm;
    const auto span = tempo_[index + 1].start_sample - segment.start_sample;
    if (span <= 0) return segment.end_bpm;
    const auto amount = static_cast<double>(sample - segment.start_sample) / static_cast<double>(span);
    return segment.start_bpm + (segment.end_bpm - segment.start_bpm) * std::clamp(amount, 0.0, 1.0);
}

float Timeline::automation_at(LaneId id, std::int64_t sample) const noexcept {
    const auto* lane = find_lane(id);
    if (!lane || !lane->enabled || lane->points.empty()) return lane ? lane->default_value : 0.0F;
    const auto found = std::lower_bound(lane->points.begin(), lane->points.end(), sample,
        [](const AutomationPoint& point, std::int64_t value) { return point.sample < value; });
    if (found == lane->points.begin()) return found->value;
    if (found == lane->points.end()) return lane->points.back().value;
    const auto& right = *found;
    const auto& left = *(found - 1);
    if (left.shape == 1 || right.sample == left.sample) return left.value;
    const auto amount = static_cast<double>(sample - left.sample) / static_cast<double>(right.sample - left.sample);
    return interpolate(left.value, right.value, amount);
}

std::int64_t Timeline::end_sample() const noexcept {
    std::int64_t result = 0;
    for (const auto& clip : clips_) result = std::max(result, safe_end(clip.timeline_start, clip.length));
    for (const auto& marker : markers_) result = std::max(result, safe_end(marker.position, marker.duration));
    return result;
}

TimelineStatistics Timeline::statistics() const noexcept {
    TimelineStatistics result;
    result.tracks = tracks_.size();
    result.clips = clips_.size();
    result.tempo_segments = tempo_.size();
    result.automation_lanes = lanes_.size();
    result.markers = markers_.size();
    result.takes = takes_.size();
    result.end_sample = end_sample();
    for (const auto& lane : lanes_) result.automation_points += lane.points.size();
    return result;
}

void Timeline::rebuild_indexes() {
    rebuild_track_index();
    rebuild_clip_index();
    rebuild_lane_index();
    rebuild_marker_index();
    rebuild_take_index();
}

void Timeline::canonicalize() {
    for (auto& track : tracks_) track.name = sanitize_label(track.name);
    for (auto& lane : lanes_) {
        lane.parameter = sanitize_label(lane.parameter);
        normalize_lane(lane);
    }
    for (auto& marker : markers_) {
        marker.name = sanitize_label(marker.name);
        marker.note = replace_invalid_utf8(marker.note);
    }
    sort_clips();
    sort_tempo();
    sort_markers();
    sort_takes();
    rebuild_indexes();
}

void Timeline::clear() {
    tracks_.clear(); clips_.clear(); tempo_.clear(); lanes_.clear(); markers_.clear(); takes_.clear();
    track_index_.clear(); clip_index_.clear(); lane_index_.clear(); marker_index_.clear(); take_index_.clear();
    selected_clip_ = nullptr;
    tempo_probe_ = nullptr;
    automation_cursor_ = nullptr;
    marker_name_probe_ = {};
    audition_take_ = nullptr;
}

const std::vector<Track>& Timeline::tracks() const noexcept { return tracks_; }
const std::vector<Clip>& Timeline::clips() const noexcept { return clips_; }
const std::vector<TempoSegment>& Timeline::tempo_segments() const noexcept { return tempo_; }
const std::vector<AutomationLane>& Timeline::automation_lanes() const noexcept { return lanes_; }
const std::vector<Marker>& Timeline::markers() const noexcept { return markers_; }
const std::vector<Take>& Timeline::takes() const noexcept { return takes_; }

std::string Timeline::summary_text() const {
    const auto stats = statistics();
    std::ostringstream out;
    out << "tracks: " << stats.tracks << '\n'
        << "clips: " << stats.clips << '\n'
        << "tempo segments: " << stats.tempo_segments << '\n'
        << "automation lanes: " << stats.automation_lanes << '\n'
        << "automation points: " << stats.automation_points << '\n'
        << "markers: " << stats.markers << '\n'
        << "takes: " << stats.takes << '\n'
        << "end sample: " << stats.end_sample << '\n';
    for (const auto& track : tracks_) {
        out << "track " << track.id.to_string() << " " << track.name
            << " channels=" << track.channels << " gain=" << track.gain << " pan=" << track.pan << '\n';
        for (const auto* clip : clips_on_track(track.id)) {
            out << "  clip " << clip->id.to_string() << " media=" << clip->media_id.to_string()
                << " start=" << clip->timeline_start << " length=" << clip->length << '\n';
        }
    }
    return out.str();
}

void Timeline::rebuild_track_index() {
    track_index_.clear();
    for (std::size_t index = 0; index < tracks_.size(); ++index) track_index_[tracks_[index].id] = index;
}

void Timeline::rebuild_clip_index() {
    clip_index_.clear();
    for (std::size_t index = 0; index < clips_.size(); ++index) clip_index_[clips_[index].id] = index;
}

void Timeline::rebuild_lane_index() {
    lane_index_.clear();
    for (std::size_t index = 0; index < lanes_.size(); ++index) lane_index_[lanes_[index].id] = index;
}

void Timeline::rebuild_marker_index() {
    marker_index_.clear();
    for (std::size_t index = 0; index < markers_.size(); ++index) marker_index_[markers_[index].id] = index;
}

void Timeline::rebuild_take_index() {
    take_index_.clear();
    for (std::size_t index = 0; index < takes_.size(); ++index) take_index_[takes_[index].id] = index;
}

void Timeline::sort_clips() {
    std::stable_sort(clips_.begin(), clips_.end(), [](const Clip& left, const Clip& right) {
        return std::tie(left.track_id, left.timeline_start, left.id) <
               std::tie(right.track_id, right.timeline_start, right.id);
    });
}

void Timeline::sort_tempo() {
    std::stable_sort(tempo_.begin(), tempo_.end(), [](const TempoSegment& left, const TempoSegment& right) {
        return std::tie(left.start_sample, left.revision, left.id) <
               std::tie(right.start_sample, right.revision, right.id);
    });
}

void Timeline::sort_markers() {
    std::stable_sort(markers_.begin(), markers_.end(), [](const Marker& left, const Marker& right) {
        return std::tie(left.position, left.name, left.id) < std::tie(right.position, right.name, right.id);
    });
}

void Timeline::sort_takes() {
    std::stable_sort(takes_.begin(), takes_.end(), [](const Take& left, const Take& right) {
        return std::tie(left.track_id, left.lane, left.source_start, left.id) <
               std::tie(right.track_id, right.lane, right.source_start, right.id);
    });
}

void Timeline::normalize_lane(AutomationLane& lane) {
    std::stable_sort(lane.points.begin(), lane.points.end(), [](const AutomationPoint& left, const AutomationPoint& right) {
        return left.sample < right.sample;
    });
    std::vector<AutomationPoint> normalized;
    normalized.reserve(lane.points.size());
    for (const auto& point : lane.points) {
        if (!normalized.empty() && normalized.back().sample == point.sample) normalized.back() = point;
        else normalized.push_back(point);
    }
    lane.points.swap(normalized);
}

}  // namespace wavecairn
