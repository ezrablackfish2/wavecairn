#pragma once

#include "wavecairn/command.h"
#include "wavecairn/diagnostics.h"
#include "wavecairn/session.h"
#include "wavecairn/status.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wavecairn {

enum class MergePreference : std::uint8_t {
    keep_base,
    prefer_incoming,
    prefer_newer_revision,
};

enum class MergeDisposition : std::uint8_t {
    added,
    retained,
    replaced,
    skipped,
    conflict,
};

struct MergePolicy {
    MergePreference preference{MergePreference::prefer_newer_revision};
    bool include_media{true};
    bool include_timeline{true};
    bool include_routing{true};
    bool include_rendering{true};
    bool include_history{true};
    bool include_package_sections{true};
    bool preserve_offline_state{true};
    bool preserve_track_mix{true};
    bool preserve_clip_envelopes{true};
};

struct MergeDecision {
    std::string area;
    EntityId id;
    MergeDisposition disposition{MergeDisposition::retained};
    std::string reason;
};

struct MergePlan {
    CommandTape commands;
    std::vector<MergeDecision> decisions;
    std::size_t additions{0};
    std::size_t replacements{0};
    std::size_t retained{0};
    std::size_t conflicts{0};

    bool clean() const noexcept;
    std::string summary_text() const;
};

MergePlan plan_merge(const Session& base,
                     const Session& incoming,
                     const MergePolicy& policy = {});

Result<ProcessingReport> apply_merge(Session& base,
                                     const MergePlan& plan,
                                     Diagnostics& diagnostics,
                                     bool canonicalize = true);

std::string merge_decisions_csv(const MergePlan& plan);
std::string merge_disposition_name(MergeDisposition disposition);

}  // namespace wavecairn
