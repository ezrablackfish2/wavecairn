#pragma once

#include "wavecairn/session.h"
#include "wavecairn/status.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wavecairn {

struct ManifestEntry {
    std::string type;
    EntityId id;
    std::string path;
    std::string label;
    std::uint64_t size{0};
    std::uint64_t digest{0};
    std::map<std::string, std::string> attributes;
};

struct SessionManifest {
    std::uint32_t version{1};
    std::uint64_t session_fingerprint{0};
    std::vector<ManifestEntry> entries;
};

SessionManifest build_manifest(const Session& session);
std::string serialize_manifest(const SessionManifest& manifest);
Result<SessionManifest> parse_manifest(std::string_view text);

std::vector<std::string> manifest_conflicts(const SessionManifest& manifest);
std::vector<ManifestEntry> manifest_entries_by_type(const SessionManifest& manifest,
                                                    std::string_view type);
std::optional<ManifestEntry> find_manifest_entry(const SessionManifest& manifest,
                                                 EntityId id);
std::uint64_t manifest_digest(const SessionManifest& manifest) noexcept;
std::string manifest_summary(const SessionManifest& manifest);

}  // namespace wavecairn
