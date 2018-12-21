#include "wavecairn/validator.h"

#include "wavecairn/checksum.h"
#include "wavecairn/package.h"
#include "wavecairn/text.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace wavecairn {
namespace {

bool ranges_overlap(std::int64_t left_start,
                    std::int64_t left_length,
                    std::int64_t right_start,
                    std::int64_t right_length) noexcept {
    const auto left_end = left_start + std::max<std::int64_t>(0, left_length);
    const auto right_end = right_start + std::max<std::int64_t>(0, right_length);
    return left_start < right_end && right_start < left_end;
}

void add_check(ValidationSummary& summary) noexcept { ++summary.checks; }

}  // namespace

SessionValidator::SessionValidator(ValidationPolicy policy) : policy_(policy) {}

ValidationSummary SessionValidator::validate(const Session& session, Diagnostics& diagnostics) const {
    ValidationSummary summary;
    validate_metadata(session, diagnostics, summary);
    validate_media(session, diagnostics, summary);
    validate_timeline(session, diagnostics, summary);
    validate_routing(session, diagnostics, summary);
    validate_rendering(session, diagnostics, summary);
    validate_history(session, diagnostics, summary);
    validate_package_data(session, diagnostics, summary);
    summary.infos = diagnostics.count(Severity::info);
    summary.warnings = diagnostics.count(Severity::warning);
    summary.errors = diagnostics.count(Severity::error);
    summary.usable = summary.errors == 0;
    return summary;
}

void SessionValidator::validate_metadata(const Session& session,
                                         Diagnostics& diagnostics,
                                         ValidationSummary& summary) const {
    const auto& metadata = session.metadata();
    add_check(summary);
    if (!metadata.id) diagnostics.error(metadata.revision, "validator", "session-id", "session id is missing");
    add_check(summary);
    if (trim(metadata.title).empty()) diagnostics.warning(metadata.revision, "validator", "title", "session title is empty");
    add_check(summary);
    if (!is_valid_utf8(metadata.title) || !is_valid_utf8(metadata.creator) || !is_valid_utf8(metadata.application)) {
        diagnostics.error(metadata.revision, "validator", "metadata-utf8", "session metadata contains invalid UTF-8");
    }
    add_check(summary);
    if (metadata.sample_rate < 8000 || metadata.sample_rate > 768000) {
        diagnostics.error(metadata.revision, "validator", "sample-rate", "session sample rate is outside the supported range");
    }
}

void SessionValidator::validate_media(const Session& session,
                                      Diagnostics& diagnostics,
                                      ValidationSummary& summary) const {
    std::unordered_set<EntityId, EntityIdHash> ids;
    std::unordered_map<std::string, MediaId> paths;
    for (const auto& asset : session.media().assets()) {
        add_check(summary);
        if (!asset.id || !ids.insert(asset.id).second) {
            diagnostics.error(asset.generation, "validator", "media-id", "duplicate or empty media id");
        }
        add_check(summary);
        if (policy_.reject_absolute_paths && (!path_is_relative(asset.relative_path) || path_has_parent_escape(asset.relative_path))) {
            diagnostics.error(asset.generation, "validator", "media-path", "media path escapes the package root");
        }
        add_check(summary);
        if (asset.logical_name.empty()) diagnostics.warning(asset.generation, "validator", "media-name", "media asset has an empty display name");
        add_check(summary);
        if (asset.format.channels == 0 || asset.format.channels > 64) {
            diagnostics.error(asset.generation, "validator", "media-channels", "media channel count is invalid");
        }
        add_check(summary);
        if (asset.format.sample_rate < 8000 || asset.format.sample_rate > 768000) {
            diagnostics.error(asset.generation, "validator", "media-rate", "media sample rate is invalid");
        }
        if (policy_.warn_offline_media && asset.offline) {
            diagnostics.warning(asset.generation, "validator", "media-offline", asset.logical_name + " is offline");
        }
        const auto lowered = ascii_lower(normalize_path(asset.relative_path));
        const auto found = paths.find(lowered);
        if (found != paths.end() && !lowered.empty()) {
            diagnostics.warning(asset.generation,
                                "validator",
                                "media-path-conflict",
                                "media assets " + found->second.to_string() + " and " + asset.id.to_string() + " share a path");
        } else {
            paths.emplace(lowered, asset.id);
        }
    }
    for (const auto& alias : session.media().aliases()) {
        add_check(summary);
        if (!alias.alias_id || !ids.insert(alias.alias_id).second) {
            diagnostics.error(alias.revision, "validator", "alias-id", "duplicate or empty alias id");
        }
        add_check(summary);
        if (!session.media().resolve(alias.target_id)) {
            diagnostics.error(alias.revision, "validator", "alias-target", "media alias target cannot be resolved");
        }
        add_check(summary);
        if (alias.label.empty()) diagnostics.warning(alias.revision, "validator", "alias-label", "media alias has an empty label");
    }
}

void SessionValidator::validate_timeline(const Session& session,
                                         Diagnostics& diagnostics,
                                         ValidationSummary& summary) const {
    const auto& timeline = session.timeline();
    if (timeline.tracks().size() > policy_.maximum_tracks) {
        diagnostics.error(0, "validator", "track-limit", "session exceeds configured track limit");
    }
    if (timeline.clips().size() > policy_.maximum_clips) {
        diagnostics.error(0, "validator", "clip-limit", "session exceeds configured clip limit");
    }
    std::unordered_set<EntityId, EntityIdHash> track_ids;
    for (const auto& track : timeline.tracks()) {
        add_check(summary);
        if (!track.id || !track_ids.insert(track.id).second) {
            diagnostics.error(track.revision, "validator", "track-id", "duplicate or empty track id");
        }
        add_check(summary);
        if (track.channels == 0 || track.channels > 64) {
            diagnostics.error(track.revision, "validator", "track-channels", "track channel count is invalid");
        }
        add_check(summary);
        if (!std::isfinite(track.gain) || !std::isfinite(track.pan)) {
            diagnostics.error(track.revision, "validator", "track-mix", "track mix values are not finite");
        }
    }

    std::unordered_set<EntityId, EntityIdHash> clip_ids;
    for (const auto& clip : timeline.clips()) {
        add_check(summary);
        if (!clip.id || !clip_ids.insert(clip.id).second) {
            diagnostics.error(clip.revision, "validator", "clip-id", "duplicate or empty clip id");
        }
        add_check(summary);
        if (!timeline.find_track(clip.track_id)) {
            diagnostics.error(clip.revision, "validator", "clip-track", "clip references a missing track");
        }
        add_check(summary);
        if (policy_.require_media_for_clips && !session.media().resolve(clip.media_id)) {
            diagnostics.error(clip.revision, "validator", "clip-media", "clip references missing media");
        }
        add_check(summary);
        if (clip.length <= 0 || clip.source_start < 0) {
            diagnostics.error(clip.revision, "validator", "clip-range", "clip range is invalid");
        }
        add_check(summary);
        if (clip.fade_in < 0.0F || clip.fade_out < 0.0F || clip.fade_in + clip.fade_out > clip.length) {
            diagnostics.warning(clip.revision, "validator", "clip-fades", "clip fades overlap or extend beyond the clip");
        }
    }

    if (policy_.warn_overlapping_clips) {
        for (const auto& track : timeline.tracks()) {
            auto clips = timeline.clips_on_track(track.id);
            for (std::size_t left = 0; left < clips.size(); ++left) {
                for (std::size_t right = left + 1; right < clips.size(); ++right) {
                    if (clips[right]->timeline_start >= clips[left]->timeline_start + clips[left]->length) break;
                    add_check(summary);
                    if (ranges_overlap(clips[left]->timeline_start,
                                       clips[left]->length,
                                       clips[right]->timeline_start,
                                       clips[right]->length) &&
                        clips[left]->group != clips[right]->group) {
                        diagnostics.warning(std::max(clips[left]->revision, clips[right]->revision),
                                            "validator",
                                            "clip-overlap",
                                            "ungrouped clips overlap on track " + track.name);
                    }
                }
            }
        }
    }

    std::int64_t previous_tempo = std::numeric_limits<std::int64_t>::min();
    for (const auto& segment : timeline.tempo_segments()) {
        add_check(summary);
        if (segment.start_sample < previous_tempo) {
            diagnostics.error(segment.revision, "validator", "tempo-order", "tempo segments are not sorted");
        }
        previous_tempo = segment.start_sample;
        add_check(summary);
        if (segment.start_bpm < 10.0 || segment.start_bpm > 1000.0 ||
            segment.end_bpm < 10.0 || segment.end_bpm > 1000.0) {
            diagnostics.error(segment.revision, "validator", "tempo-range", "tempo value is outside the supported range");
        }
        add_check(summary);
        if (segment.numerator == 0 || segment.denominator == 0) {
            diagnostics.error(segment.revision, "validator", "meter", "time signature contains zero");
        }
    }

    for (const auto& lane : timeline.automation_lanes()) {
        add_check(summary);
        if (!timeline.find_track(lane.track_id)) {
            diagnostics.error(lane.revision, "validator", "automation-track", "automation lane references a missing track");
        }
        std::int64_t previous = std::numeric_limits<std::int64_t>::min();
        for (const auto& point : lane.points) {
            add_check(summary);
            if (point.sample < previous) diagnostics.error(lane.revision, "validator", "automation-order", "automation points are not sorted");
            previous = point.sample;
            if (!std::isfinite(point.value) || !std::isfinite(point.tension)) {
                diagnostics.error(lane.revision, "validator", "automation-value", "automation point is not finite");
            }
        }
    }

    std::unordered_set<EntityId, EntityIdHash> marker_ids;
    for (const auto& marker : timeline.markers()) {
        add_check(summary);
        if (!marker.id || !marker_ids.insert(marker.id).second) diagnostics.error(marker.revision, "validator", "marker-id", "duplicate or empty marker id");
        add_check(summary);
        if (!is_valid_utf8(marker.name) || !is_valid_utf8(marker.note)) diagnostics.error(marker.revision, "validator", "marker-utf8", "marker text is invalid UTF-8");
    }

    std::unordered_set<EntityId, EntityIdHash> take_ids;
    for (const auto& take : timeline.takes()) {
        add_check(summary);
        if (!take.id || !take_ids.insert(take.id).second) diagnostics.error(take.revision, "validator", "take-id", "duplicate or empty take id");
        add_check(summary);
        if (!timeline.find_track(take.track_id)) diagnostics.error(take.revision, "validator", "take-track", "take references a missing track");
        add_check(summary);
        if (!session.media().resolve(take.media_id)) diagnostics.warning(take.revision, "validator", "take-media", "take references missing media");
        add_check(summary);
        if (take.length <= 0 || take.source_start < 0) diagnostics.error(take.revision, "validator", "take-range", "take range is invalid");
    }
}

void SessionValidator::validate_routing(const Session& session,
                                        Diagnostics& diagnostics,
                                        ValidationSummary& summary) const {
    const auto& routing = session.routing();
    if (routing.plugins().size() > policy_.maximum_plugins) {
        diagnostics.error(0, "validator", "plugin-limit", "session exceeds configured plug-in limit");
    }
    std::unordered_set<EntityId, EntityIdHash> plugin_ids;
    for (const auto& plugin : routing.plugins()) {
        add_check(summary);
        if (!plugin.id || !plugin_ids.insert(plugin.id).second) {
            diagnostics.error(plugin.generation, "validator", "plugin-id", "duplicate or empty plug-in id");
        }
        add_check(summary);
        if (policy_.require_tracks_for_routing && !session.timeline().find_track(plugin.track_id)) {
            diagnostics.error(plugin.generation, "validator", "plugin-track", "plug-in references a missing track");
        }
        add_check(summary);
        if (plugin.product.empty()) diagnostics.warning(plugin.generation, "validator", "plugin-name", "plug-in product name is empty");
        for (const auto parameter : plugin.parameters) {
            add_check(summary);
            if (!std::isfinite(parameter)) diagnostics.error(plugin.generation, "validator", "plugin-parameter", "plug-in parameter is not finite");
        }
    }
    for (const auto& edge : routing.edges()) {
        add_check(summary);
        if (!routing.find_plugin(edge.source) || !routing.find_plugin(edge.destination)) {
            diagnostics.error(0, "validator", "plugin-edge", "plug-in edge references a missing node");
        }
        add_check(summary);
        if (!std::isfinite(edge.gain) || edge.gain < 0.0F) {
            diagnostics.error(0, "validator", "plugin-edge-gain", "plug-in edge gain is invalid");
        }
    }
    add_check(summary);
    if (routing.has_cycle()) diagnostics.error(0, "validator", "plugin-cycle", "plug-in graph contains a cycle");
    for (const auto& send : routing.sends()) {
        add_check(summary);
        if (send.source == send.destination) diagnostics.error(send.revision, "validator", "send-loop", "send routes a track to itself");
        add_check(summary);
        if (policy_.require_tracks_for_routing &&
            (!session.timeline().find_track(send.source) || !session.timeline().find_track(send.destination))) {
            diagnostics.error(send.revision, "validator", "send-track", "send references a missing track");
        }
        add_check(summary);
        if (!std::isfinite(send.level) || !std::isfinite(send.pan)) {
            diagnostics.error(send.revision, "validator", "send-value", "send mix values are not finite");
        }
    }
}

void SessionValidator::validate_rendering(const Session& session,
                                          Diagnostics& diagnostics,
                                          ValidationSummary& summary) const {
    const auto& rendering = session.rendering();
    std::unordered_set<EntityId, EntityIdHash> job_ids;
    for (const auto& job : rendering.jobs()) {
        add_check(summary);
        if (!job.id || !job_ids.insert(job.id).second) diagnostics.error(job.revision, "validator", "render-id", "duplicate or empty render job id");
        add_check(summary);
        if (!session.timeline().find_track(job.track_id)) diagnostics.warning(job.revision, "validator", "render-track", "render job references a missing track");
        add_check(summary);
        if (job.end_sample <= job.start_sample) diagnostics.error(job.revision, "validator", "render-range", "render range is empty");
        add_check(summary);
        if (job.sample_rate < 8000 || job.sample_rate > 768000) diagnostics.error(job.revision, "validator", "render-rate", "render sample rate is invalid");
        add_check(summary);
        if (job.channels == 0 || job.channels > 64) diagnostics.error(job.revision, "validator", "render-channels", "render channel count is invalid");
    }
    for (const auto& slice : rendering.slices()) {
        add_check(summary);
        if (!rendering.find_job(slice.job_id)) diagnostics.error(0, "validator", "render-slice-job", "render slice references a missing job");
        add_check(summary);
        if (slice.end_sample <= slice.start_sample) diagnostics.error(0, "validator", "render-slice-range", "render slice range is empty");
    }
}

void SessionValidator::validate_history(const Session& session,
                                        Diagnostics& diagnostics,
                                        ValidationSummary& summary) const {
    const auto& history = session.history();
    std::unordered_set<EntityId, EntityIdHash> snapshot_ids;
    for (const auto& snapshot : history.snapshots()) {
        add_check(summary);
        if (!snapshot.id || !snapshot_ids.insert(snapshot.id).second) diagnostics.error(snapshot.revision, "validator", "snapshot-id", "duplicate or empty snapshot id");
        add_check(summary);
        if (snapshot.label.empty()) diagnostics.warning(snapshot.revision, "validator", "snapshot-label", "snapshot label is empty");
        std::unordered_set<EntityId, EntityIdHash> track_ids;
        for (const auto& track : snapshot.tracks) {
            add_check(summary);
            if (!track.id || !track_ids.insert(track.id).second) diagnostics.error(snapshot.revision, "validator", "snapshot-track", "snapshot contains duplicate track state");
        }
    }
    std::uint32_t previous_sequence = 0;
    bool first = true;
    for (const auto& record : history.journal()) {
        add_check(summary);
        if (!first && record.sequence <= previous_sequence) diagnostics.warning(record.transaction, "validator", "journal-order", "journal sequence is not strictly increasing");
        previous_sequence = record.sequence;
        first = false;
        if (policy_.validate_history_digests) {
            add_check(summary);
            const auto expected = combine_hash(record.opcode, fnv1a64(record.payload));
            if (expected != record.digest) diagnostics.error(record.transaction, "validator", "journal-digest", "journal record digest does not match payload");
        }
    }
    std::unordered_set<EntityId, EntityIdHash> record_ids;
    for (const auto& record : history.records()) {
        add_check(summary);
        if (!record.id || !record_ids.insert(record.id).second) diagnostics.error(record.revision, "validator", "project-record-id", "duplicate or empty project record id");
        add_check(summary);
        if (record.key.empty()) diagnostics.warning(record.revision, "validator", "project-record-key", "project record key is empty");
    }
}

void SessionValidator::validate_package_data(const Session& session,
                                             Diagnostics& diagnostics,
                                             ValidationSummary& summary) const {
    if (!policy_.validate_package_sections) return;
    const auto& package = session.package().document();
    add_check(summary);
    if (package.sections.size() > policy_.maximum_sections) diagnostics.error(0, "validator", "section-limit", "package exceeds configured section limit");
    std::map<SectionTag, std::size_t> counts;
    for (const auto& section : package.sections) {
        ++counts[section.tag];
        add_check(summary);
        if (!section_tag_is_known(section.tag)) diagnostics.info(section.revision, "validator", "section-unknown", "unknown package section " + section_tag_string(section.tag));
        add_check(summary);
        if (section_tag_is_text(section.tag)) {
            const auto text = std::string_view(reinterpret_cast<const char*>(section.payload.data()), section.payload.size());
            if (!is_valid_utf8(text)) diagnostics.error(section.revision, "validator", "section-utf8", "text package section contains invalid UTF-8");
        }
    }
    add_check(summary);
    if (counts[make_section_tag("TAPE")] > 1) diagnostics.warning(0, "validator", "multiple-tapes", "package contains multiple command tape sections");
}

RepairSummary SessionRepairer::repair(Session& session, Diagnostics& diagnostics) const {
    RepairSummary summary;
    auto before_title = session.metadata().title;
    session.metadata().title = sanitize_label(session.metadata().title, 160);
    summary.labels_normalized += before_title != session.metadata().title;
    auto before_creator = session.metadata().creator;
    session.metadata().creator = sanitize_label(session.metadata().creator, 160);
    summary.labels_normalized += before_creator != session.metadata().creator;

    for (auto& asset : const_cast<std::vector<MediaAsset>&>(session.media().assets())) {
        const auto old_name = asset.logical_name;
        const auto old_path = asset.relative_path;
        asset.logical_name = sanitize_label(asset.logical_name);
        asset.relative_path = normalize_path(asset.relative_path);
        summary.labels_normalized += old_name != asset.logical_name;
        summary.paths_normalized += old_path != asset.relative_path;
    }

    const auto old_markers = session.timeline().markers().size();
    session.timeline().canonicalize();
    summary.duplicate_markers_removed = old_markers - session.timeline().markers().size();

    const auto old_edges = session.routing().edges().size();
    session.routing().canonicalize();
    summary.dangling_edges_removed = old_edges - session.routing().edges().size();

    const auto old_sections = session.package().document().sections.size();
    auto& sections = session.package().document().sections;
    sections.erase(std::remove_if(sections.begin(), sections.end(), [](const PackageSection& section) {
        return section.payload.empty() && (section.flags & 1U) == 0;
    }), sections.end());
    summary.empty_sections_removed = old_sections - sections.size();

    session.canonicalize();
    diagnostics.info(0,
                     "repair",
                     "complete",
                     "normalized " + std::to_string(summary.labels_normalized) + " labels and " +
                         std::to_string(summary.paths_normalized) + " paths");
    return summary;
}

}  // namespace wavecairn
