#pragma once

#include "wavecairn/command.h"
#include "wavecairn/diagnostics.h"
#include "wavecairn/ids.h"
#include "wavecairn/status.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wavecairn {

struct Track {
    TrackId id;
    std::string name;
    std::uint16_t channels{2};
    float gain{1.0F};
    float pan{0.0F};
    bool muted{false};
    bool solo{false};
    bool armed{false};
    std::uint32_t revision{0};
};

struct Clip {
    ClipId id;
    TrackId track_id;
    MediaId media_id;
    std::int64_t timeline_start{0};
    std::int64_t source_start{0};
    std::int64_t length{0};
    float gain{1.0F};
    float fade_in{0.0F};
    float fade_out{0.0F};
    std::uint32_t group{0};
    std::uint32_t revision{0};
    bool muted{false};
    bool locked{false};
};

struct TempoSegment {
    EntityId id;
    std::int64_t start_sample{0};
    double start_bpm{120.0};
    double end_bpm{120.0};
    std::uint8_t numerator{4};
    std::uint8_t denominator{4};
    bool ramp{false};
    std::uint32_t revision{0};
};

struct AutomationPoint {
    std::int64_t sample{0};
    float value{0.0F};
    float tension{0.0F};
    std::uint8_t shape{0};
};

struct AutomationLane {
    LaneId id;
    TrackId track_id;
    std::string parameter;
    std::vector<AutomationPoint> points;
    float default_value{0.0F};
    std::uint32_t revision{0};
    bool enabled{true};
};

struct Marker {
    MarkerId id;
    std::int64_t position{0};
    std::int64_t duration{0};
    std::string name;
    std::string note;
    std::uint32_t color{0};
    std::uint32_t revision{0};
};

struct Take {
    TakeId id;
    TrackId track_id;
    MediaId media_id;
    std::int64_t source_start{0};
    std::int64_t length{0};
    std::uint32_t lane{0};
    float rating{0.0F};
    bool active{false};
    bool rejected{false};
    std::uint32_t revision{0};
};

struct TimelineStatistics {
    std::size_t tracks{0};
    std::size_t clips{0};
    std::size_t tempo_segments{0};
    std::size_t automation_lanes{0};
    std::size_t automation_points{0};
    std::size_t markers{0};
    std::size_t takes{0};
    std::int64_t end_sample{0};
};

class Timeline {
public:
    Status apply(const Command& command, Diagnostics& diagnostics);

    const Track* find_track(TrackId id) const noexcept;
    Track* find_track(TrackId id) noexcept;
    const Clip* find_clip(ClipId id) const noexcept;
    Clip* find_clip(ClipId id) noexcept;
    const AutomationLane* find_lane(LaneId id) const noexcept;
    AutomationLane* find_lane(LaneId id) noexcept;
    const Marker* find_marker(MarkerId id) const noexcept;
    const Take* find_take(TakeId id) const noexcept;

    std::vector<const Clip*> clips_on_track(TrackId id) const;
    std::vector<const Marker*> markers_in_range(std::int64_t start, std::int64_t end) const;
    std::vector<const Take*> takes_on_track(TrackId id) const;

    double tempo_at(std::int64_t sample) const noexcept;
    float automation_at(LaneId id, std::int64_t sample) const noexcept;
    std::int64_t end_sample() const noexcept;
    TimelineStatistics statistics() const noexcept;

    void rebuild_indexes();
    void canonicalize();
    void clear();

    const std::vector<Track>& tracks() const noexcept;
    const std::vector<Clip>& clips() const noexcept;
    const std::vector<TempoSegment>& tempo_segments() const noexcept;
    const std::vector<AutomationLane>& automation_lanes() const noexcept;
    const std::vector<Marker>& markers() const noexcept;
    const std::vector<Take>& takes() const noexcept;

    std::string summary_text() const;

private:
    Status apply_track(const Command& command, Diagnostics& diagnostics);
    Status apply_clip(const Command& command, Diagnostics& diagnostics);
    Status apply_tempo(const Command& command, Diagnostics& diagnostics);
    Status apply_automation(const Command& command, Diagnostics& diagnostics);
    Status apply_marker(const Command& command, Diagnostics& diagnostics);
    Status apply_take(const Command& command, Diagnostics& diagnostics);

    void rebuild_track_index();
    void rebuild_clip_index();
    void rebuild_lane_index();
    void rebuild_marker_index();
    void rebuild_take_index();
    void sort_clips();
    void sort_tempo();
    void sort_markers();
    void sort_takes();
    void normalize_lane(AutomationLane& lane);

    std::vector<Track> tracks_;
    std::vector<Clip> clips_;
    std::vector<TempoSegment> tempo_;
    std::vector<AutomationLane> lanes_;
    std::vector<Marker> markers_;
    std::vector<Take> takes_;

    std::unordered_map<TrackId, std::size_t, EntityIdHash> track_index_;
    std::unordered_map<ClipId, std::size_t, EntityIdHash> clip_index_;
    std::unordered_map<LaneId, std::size_t, EntityIdHash> lane_index_;
    std::unordered_map<MarkerId, std::size_t, EntityIdHash> marker_index_;
    std::unordered_map<TakeId, std::size_t, EntityIdHash> take_index_;

    Clip* selected_clip_{nullptr};
    TempoSegment* tempo_probe_{nullptr};
    AutomationPoint* automation_cursor_{nullptr};
    LaneId automation_cursor_lane_;
    std::string_view marker_name_probe_;
    std::uint32_t marker_probe_revision_{0};
    Take* audition_take_{nullptr};
};

}  // namespace wavecairn
