#pragma once

#include "wavecairn/session.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wavecairn {

struct OccupancyBucket {
    std::int64_t start_sample{0};
    std::int64_t end_sample{0};
    std::size_t active_clips{0};
    std::size_t active_tracks{0};
    double estimated_gain{0.0};
};

struct DependencyNode {
    EntityId id;
    std::string kind;
    std::string label;
    std::vector<EntityId> dependencies;
    std::vector<EntityId> dependents;
};

struct SessionDifference {
    std::string area;
    std::string key;
    std::string before;
    std::string after;
};

struct RenderEstimate {
    std::uint64_t total_frames{0};
    std::uint64_t total_samples{0};
    std::uint64_t estimated_output_bytes{0};
    std::uint64_t estimated_work_units{0};
    double estimated_seconds{0.0};
};

std::vector<OccupancyBucket> build_occupancy_map(const Session& session,
                                                 std::int64_t bucket_size);
std::vector<DependencyNode> build_dependency_graph(const Session& session);
std::vector<SessionDifference> diff_sessions(const Session& before,
                                             const Session& after);
RenderEstimate estimate_render_cost(const Session& session,
                                    double realtime_factor = 1.0);
std::map<std::string, std::size_t> command_category_histogram(const CommandTape& tape);

std::string occupancy_csv(const std::vector<OccupancyBucket>& buckets);
std::string dependency_dot(const std::vector<DependencyNode>& nodes);
std::string differences_text(const std::vector<SessionDifference>& differences);
std::string session_json(const Session& session, const Diagnostics* diagnostics = nullptr);
std::string render_estimate_text(const RenderEstimate& estimate);

}  // namespace wavecairn
