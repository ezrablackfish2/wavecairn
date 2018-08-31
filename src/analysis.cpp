#include "wavecairn/analysis.h"

#include "wavecairn/format_registry.h"
#include "wavecairn/text.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace wavecairn {
namespace {

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const unsigned char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20U) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(ch >> 4U) & 0x0fU]);
                    out.push_back(hex[ch & 0x0fU]);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    return out;
}

std::string bool_text(bool value) { return value ? "true" : "false"; }

std::string media_signature(const MediaAsset& asset) {
    std::ostringstream out;
    out << media_kind_name(asset.kind) << ':' << asset.relative_path << ':' << asset.frame_count << ':'
        << asset.format.sample_rate << ':' << asset.format.channels << ':' << asset.content_hash << ':'
        << asset.offline;
    return out.str();
}

std::string track_signature(const Track& track) {
    std::ostringstream out;
    out << track.name << ':' << track.channels << ':' << track.gain << ':' << track.pan << ':'
        << track.muted << ':' << track.solo << ':' << track.armed;
    return out.str();
}

std::string clip_signature(const Clip& clip) {
    std::ostringstream out;
    out << clip.track_id.to_string() << ':' << clip.media_id.to_string() << ':' << clip.timeline_start << ':'
        << clip.source_start << ':' << clip.length << ':' << clip.gain << ':' << clip.fade_in << ':'
        << clip.fade_out << ':' << clip.group << ':' << clip.muted << ':' << clip.locked;
    return out.str();
}

std::string plugin_signature(const PluginNode& plugin) {
    std::ostringstream out;
    out << plugin.track_id.to_string() << ':' << plugin.vendor << ':' << plugin.product << ':'
        << plugin.latency << ':' << plugin.bypassed << ':' << plugin.instrument << ':';
    for (float parameter : plugin.parameters) out << parameter << ',';
    return out.str();
}

void append_difference(std::vector<SessionDifference>& result,
                       std::string area,
                       std::string key,
                       std::string before,
                       std::string after) {
    if (before != after) {
        result.push_back(SessionDifference{
            std::move(area), std::move(key), std::move(before), std::move(after)});
    }
}

}  // namespace

std::vector<OccupancyBucket> build_occupancy_map(const Session& session,
                                                 std::int64_t bucket_size) {
    bucket_size = std::max<std::int64_t>(1, bucket_size);
    const auto end = std::max<std::int64_t>(0, session.timeline().end_sample());
    const auto bucket_count = static_cast<std::size_t>((end + bucket_size - 1) / bucket_size);
    std::vector<OccupancyBucket> result;
    result.reserve(bucket_count);
    for (std::size_t index = 0; index < bucket_count; ++index) {
        const auto start = static_cast<std::int64_t>(index) * bucket_size;
        result.push_back(OccupancyBucket{start, std::min(end, start + bucket_size), 0, 0, 0.0});
    }
    for (auto& bucket : result) {
        std::unordered_set<TrackId, EntityIdHash> tracks;
        for (const auto& clip : session.timeline().clips()) {
            const auto clip_end = clip.timeline_start + std::max<std::int64_t>(0, clip.length);
            if (clip.timeline_start >= bucket.end_sample || clip_end <= bucket.start_sample) continue;
            ++bucket.active_clips;
            tracks.insert(clip.track_id);
            if (!clip.muted) bucket.estimated_gain += clip.gain;
        }
        bucket.active_tracks = tracks.size();
    }
    return result;
}

std::vector<DependencyNode> build_dependency_graph(const Session& session) {
    std::unordered_map<EntityId, DependencyNode, EntityIdHash> nodes;
    auto ensure = [&nodes](EntityId id, std::string kind, std::string label) -> DependencyNode& {
        auto [it, inserted] = nodes.try_emplace(id, DependencyNode{id, std::move(kind), std::move(label), {}, {}});
        if (!inserted && it->second.label.empty()) it->second.label = std::move(label);
        return it->second;
    };
    auto connect = [&ensure](EntityId from, EntityId to) {
        auto& source = ensure(from, "unknown", from.to_string());
        auto& target = ensure(to, "unknown", to.to_string());
        source.dependencies.push_back(to);
        target.dependents.push_back(from);
    };

    for (const auto& asset : session.media().assets()) {
        ensure(asset.id, "media", asset.logical_name);
    }
    for (const auto& alias : session.media().aliases()) {
        ensure(alias.alias_id, "media-alias", alias.label);
        connect(alias.alias_id, alias.target_id);
    }
    for (const auto& track : session.timeline().tracks()) {
        ensure(track.id, "track", track.name);
    }
    for (const auto& clip : session.timeline().clips()) {
        ensure(clip.id, "clip", "clip " + clip.id.to_string().substr(8));
        connect(clip.id, clip.track_id);
        connect(clip.id, clip.media_id);
    }
    for (const auto& lane : session.timeline().automation_lanes()) {
        ensure(lane.id, "automation", lane.parameter);
        connect(lane.id, lane.track_id);
    }
    for (const auto& take : session.timeline().takes()) {
        ensure(take.id, "take", "take " + take.id.to_string().substr(8));
        connect(take.id, take.track_id);
        connect(take.id, take.media_id);
    }
    for (const auto& plugin : session.routing().plugins()) {
        ensure(plugin.id, "plugin", plugin.vendor + " " + plugin.product);
        connect(plugin.id, plugin.track_id);
    }
    for (const auto& edge : session.routing().edges()) {
        connect(edge.destination, edge.source);
    }
    for (const auto& send : session.routing().sends()) {
        ensure(send.id, "send", "send " + send.id.to_string().substr(8));
        connect(send.id, send.source);
        connect(send.id, send.destination);
    }
    for (const auto& job : session.rendering().jobs()) {
        ensure(job.id, "render-job", "render " + job.id.to_string().substr(8));
        connect(job.id, job.track_id);
    }
    for (const auto& snapshot : session.history().snapshots()) {
        ensure(snapshot.id, "snapshot", snapshot.label);
        for (const auto& track : snapshot.tracks) connect(snapshot.id, track.id);
    }

    std::vector<DependencyNode> result;
    result.reserve(nodes.size());
    for (auto& [id, node] : nodes) {
        std::sort(node.dependencies.begin(), node.dependencies.end());
        node.dependencies.erase(std::unique(node.dependencies.begin(), node.dependencies.end()), node.dependencies.end());
        std::sort(node.dependents.begin(), node.dependents.end());
        node.dependents.erase(std::unique(node.dependents.begin(), node.dependents.end()), node.dependents.end());
        result.push_back(std::move(node));
    }
    std::stable_sort(result.begin(), result.end(), [](const DependencyNode& left, const DependencyNode& right) {
        return std::tie(left.kind, left.label, left.id) < std::tie(right.kind, right.label, right.id);
    });
    return result;
}

std::vector<SessionDifference> diff_sessions(const Session& before,
                                             const Session& after) {
    std::vector<SessionDifference> result;
    append_difference(result, "metadata", "title", before.metadata().title, after.metadata().title);
    append_difference(result, "metadata", "creator", before.metadata().creator, after.metadata().creator);
    append_difference(result, "metadata", "application", before.metadata().application, after.metadata().application);
    append_difference(result, "metadata", "sample-rate", std::to_string(before.metadata().sample_rate), std::to_string(after.metadata().sample_rate));

    std::unordered_map<EntityId, std::string, EntityIdHash> before_media;
    std::unordered_map<EntityId, std::string, EntityIdHash> after_media;
    for (const auto& asset : before.media().assets()) before_media[asset.id] = media_signature(asset);
    for (const auto& asset : after.media().assets()) after_media[asset.id] = media_signature(asset);
    std::set<EntityId> media_ids;
    for (const auto& [id, value] : before_media) media_ids.insert(id);
    for (const auto& [id, value] : after_media) media_ids.insert(id);
    for (const auto id : media_ids) append_difference(result, "media", id.to_string(), before_media[id], after_media[id]);

    std::unordered_map<EntityId, std::string, EntityIdHash> before_tracks;
    std::unordered_map<EntityId, std::string, EntityIdHash> after_tracks;
    for (const auto& track : before.timeline().tracks()) before_tracks[track.id] = track_signature(track);
    for (const auto& track : after.timeline().tracks()) after_tracks[track.id] = track_signature(track);
    std::set<EntityId> track_ids;
    for (const auto& [id, value] : before_tracks) track_ids.insert(id);
    for (const auto& [id, value] : after_tracks) track_ids.insert(id);
    for (const auto id : track_ids) append_difference(result, "track", id.to_string(), before_tracks[id], after_tracks[id]);

    std::unordered_map<EntityId, std::string, EntityIdHash> before_clips;
    std::unordered_map<EntityId, std::string, EntityIdHash> after_clips;
    for (const auto& clip : before.timeline().clips()) before_clips[clip.id] = clip_signature(clip);
    for (const auto& clip : after.timeline().clips()) after_clips[clip.id] = clip_signature(clip);
    std::set<EntityId> clip_ids;
    for (const auto& [id, value] : before_clips) clip_ids.insert(id);
    for (const auto& [id, value] : after_clips) clip_ids.insert(id);
    for (const auto id : clip_ids) append_difference(result, "clip", id.to_string(), before_clips[id], after_clips[id]);

    std::unordered_map<EntityId, std::string, EntityIdHash> before_plugins;
    std::unordered_map<EntityId, std::string, EntityIdHash> after_plugins;
    for (const auto& plugin : before.routing().plugins()) before_plugins[plugin.id] = plugin_signature(plugin);
    for (const auto& plugin : after.routing().plugins()) after_plugins[plugin.id] = plugin_signature(plugin);
    std::set<EntityId> plugin_ids;
    for (const auto& [id, value] : before_plugins) plugin_ids.insert(id);
    for (const auto& [id, value] : after_plugins) plugin_ids.insert(id);
    for (const auto id : plugin_ids) append_difference(result, "plugin", id.to_string(), before_plugins[id], after_plugins[id]);

    append_difference(result,
                      "package",
                      "digest",
                      std::to_string(before.package().content_digest()),
                      std::to_string(after.package().content_digest()));
    std::stable_sort(result.begin(), result.end(), [](const SessionDifference& left, const SessionDifference& right) {
        return std::tie(left.area, left.key) < std::tie(right.area, right.key);
    });
    return result;
}

RenderEstimate estimate_render_cost(const Session& session, double realtime_factor) {
    RenderEstimate result;
    realtime_factor = std::max(0.001, realtime_factor);
    std::uint64_t weighted_plugins = 0;
    for (const auto& plugin : session.routing().plugins()) {
        if (!plugin.bypassed) weighted_plugins += 1 + plugin.parameters.size() / 16 + plugin.latency / 1024;
    }
    for (const auto& job : session.rendering().jobs()) {
        const auto frames = static_cast<std::uint64_t>(std::max<std::int64_t>(0, job.end_sample - job.start_sample));
        result.total_frames += frames;
        const auto channels = std::max<std::uint16_t>(1, job.channels);
        if (frames <= std::numeric_limits<std::uint64_t>::max() / channels) result.total_samples += frames * channels;
        const auto bytes_per_sample = job.dither ? 3U : 4U;
        if (frames <= std::numeric_limits<std::uint64_t>::max() / (channels * bytes_per_sample)) {
            result.estimated_output_bytes += frames * channels * bytes_per_sample;
        }
        result.estimated_work_units += frames * std::max<std::uint64_t>(1, weighted_plugins + channels);
        result.estimated_seconds += static_cast<double>(frames) /
                                    static_cast<double>(std::max<std::uint32_t>(1, job.sample_rate)) /
                                    realtime_factor;
    }
    return result;
}

std::map<std::string, std::size_t> command_category_histogram(const CommandTape& tape) {
    std::map<std::string, std::size_t> result;
    for (const auto& command : tape.commands) {
        const auto category = command_category_name(describe_opcode(command.opcode).category);
        ++result[std::string(category)];
    }
    return result;
}

std::string occupancy_csv(const std::vector<OccupancyBucket>& buckets) {
    std::ostringstream out;
    out << "start_sample,end_sample,active_clips,active_tracks,estimated_gain\n";
    for (const auto& bucket : buckets) {
        out << bucket.start_sample << ',' << bucket.end_sample << ',' << bucket.active_clips << ','
            << bucket.active_tracks << ',' << format_decimal(bucket.estimated_gain, 6) << '\n';
    }
    return out.str();
}

std::string dependency_dot(const std::vector<DependencyNode>& nodes) {
    std::ostringstream out;
    out << "digraph wavecairn {\n  rankdir=LR;\n";
    for (const auto& node : nodes) {
        out << "  n" << node.id.to_string() << " [label=\"" << json_escape(node.kind + "\\n" + node.label) << "\"];\n";
    }
    for (const auto& node : nodes) {
        for (const auto dependency : node.dependencies) {
            out << "  n" << node.id.to_string() << " -> n" << dependency.to_string() << ";\n";
        }
    }
    out << "}\n";
    return out.str();
}

std::string differences_text(const std::vector<SessionDifference>& differences) {
    std::ostringstream out;
    for (const auto& difference : differences) {
        out << difference.area << ' ' << difference.key << '\n'
            << "  - " << (difference.before.empty() ? "<missing>" : difference.before) << '\n'
            << "  + " << (difference.after.empty() ? "<missing>" : difference.after) << '\n';
    }
    return out.str();
}

std::string session_json(const Session& session, const Diagnostics* diagnostics) {
    std::ostringstream out;
    const auto media_stats = session.media().statistics();
    const auto timeline_stats = session.timeline().statistics();
    const auto routing_stats = session.routing().statistics();
    const auto render_stats = session.rendering().statistics();
    const auto history_stats = session.history().statistics();
    out << '{'
        << "\"metadata\":{"
        << "\"id\":\"" << session.metadata().id.to_string() << "\","
        << "\"title\":\"" << json_escape(session.metadata().title) << "\","
        << "\"creator\":\"" << json_escape(session.metadata().creator) << "\","
        << "\"application\":\"" << json_escape(session.metadata().application) << "\","
        << "\"sample_rate\":" << session.metadata().sample_rate << "},"
        << "\"fingerprint\":" << session.fingerprint() << ','
        << "\"counts\":{"
        << "\"media\":" << media_stats.assets << ','
        << "\"aliases\":" << media_stats.aliases << ','
        << "\"tracks\":" << timeline_stats.tracks << ','
        << "\"clips\":" << timeline_stats.clips << ','
        << "\"tempo\":" << timeline_stats.tempo_segments << ','
        << "\"automation\":" << timeline_stats.automation_lanes << ','
        << "\"markers\":" << timeline_stats.markers << ','
        << "\"takes\":" << timeline_stats.takes << ','
        << "\"plugins\":" << routing_stats.plugins << ','
        << "\"edges\":" << routing_stats.edges << ','
        << "\"sends\":" << routing_stats.sends << ','
        << "\"render_jobs\":" << render_stats.jobs << ','
        << "\"sample_blocks\":" << render_stats.sample_blocks << ','
        << "\"snapshots\":" << history_stats.snapshots << ','
        << "\"journal\":" << history_stats.journal_records << ','
        << "\"sections\":" << session.package().document().sections.size() << "},"
        << "\"media\":[";
    for (std::size_t index = 0; index < session.media().assets().size(); ++index) {
        if (index != 0) out << ',';
        const auto& asset = session.media().assets()[index];
        out << "{\"id\":\"" << asset.id.to_string() << "\",\"name\":\"" << json_escape(asset.logical_name)
            << "\",\"path\":\"" << json_escape(asset.relative_path) << "\",\"kind\":\""
            << media_kind_name(asset.kind) << "\",\"offline\":" << bool_text(asset.offline) << '}';
    }
    out << "],\"tracks\":[";
    for (std::size_t index = 0; index < session.timeline().tracks().size(); ++index) {
        if (index != 0) out << ',';
        const auto& track = session.timeline().tracks()[index];
        out << "{\"id\":\"" << track.id.to_string() << "\",\"name\":\"" << json_escape(track.name)
            << "\",\"channels\":" << track.channels << ",\"gain\":" << track.gain
            << ",\"pan\":" << track.pan << '}';
    }
    out << ']';
    if (diagnostics != nullptr) out << ",\"diagnostics\":" << diagnostics->to_json();
    out << '}';
    return out.str();
}

std::string render_estimate_text(const RenderEstimate& estimate) {
    std::ostringstream out;
    out << "total frames: " << estimate.total_frames << '\n'
        << "total samples: " << estimate.total_samples << '\n'
        << "estimated output bytes: " << estimate.estimated_output_bytes << '\n'
        << "estimated work units: " << estimate.estimated_work_units << '\n'
        << "estimated seconds: " << format_decimal(estimate.estimated_seconds, 3) << '\n';
    return out.str();
}

}  // namespace wavecairn
