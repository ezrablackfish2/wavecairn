#pragma once

#include "wavecairn/diagnostics.h"
#include "wavecairn/session.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wavecairn {

struct ValidationPolicy {
    bool require_media_for_clips{true};
    bool require_tracks_for_routing{true};
    bool reject_absolute_paths{true};
    bool warn_overlapping_clips{true};
    bool warn_offline_media{true};
    bool validate_package_sections{true};
    bool validate_history_digests{true};
    std::size_t maximum_tracks{4096};
    std::size_t maximum_clips{65536};
    std::size_t maximum_plugins{8192};
    std::size_t maximum_sections{1024};
};

struct ValidationSummary {
    std::size_t checks{0};
    std::size_t infos{0};
    std::size_t warnings{0};
    std::size_t errors{0};
    bool usable{true};
};

class SessionValidator {
public:
    explicit SessionValidator(ValidationPolicy policy = {});

    ValidationSummary validate(const Session& session, Diagnostics& diagnostics) const;

private:
    void validate_metadata(const Session& session, Diagnostics& diagnostics, ValidationSummary& summary) const;
    void validate_media(const Session& session, Diagnostics& diagnostics, ValidationSummary& summary) const;
    void validate_timeline(const Session& session, Diagnostics& diagnostics, ValidationSummary& summary) const;
    void validate_routing(const Session& session, Diagnostics& diagnostics, ValidationSummary& summary) const;
    void validate_rendering(const Session& session, Diagnostics& diagnostics, ValidationSummary& summary) const;
    void validate_history(const Session& session, Diagnostics& diagnostics, ValidationSummary& summary) const;
    void validate_package_data(const Session& session, Diagnostics& diagnostics, ValidationSummary& summary) const;

    ValidationPolicy policy_;
};

struct RepairSummary {
    std::size_t labels_normalized{0};
    std::size_t paths_normalized{0};
    std::size_t duplicate_markers_removed{0};
    std::size_t dangling_edges_removed{0};
    std::size_t empty_sections_removed{0};
};

class SessionRepairer {
public:
    RepairSummary repair(Session& session, Diagnostics& diagnostics) const;
};

}  // namespace wavecairn
