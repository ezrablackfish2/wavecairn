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

enum class MediaKind : std::uint8_t {
    audio = 1,
    midi = 2,
    video = 3,
    impulse = 4,
    analysis = 5,
};

struct AudioFormat {
    std::uint32_t sample_rate{48000};
    std::uint16_t channels{2};
    std::uint16_t bits_per_sample{24};
    bool floating_point{false};
};

struct MediaAsset {
    MediaId id;
    MediaKind kind{MediaKind::audio};
    std::string logical_name;
    std::string relative_path;
    AudioFormat format;
    std::uint64_t frame_count{0};
    std::uint64_t content_hash{0};
    std::uint32_t generation{0};
    bool offline{false};
    bool generated{false};
};

struct MediaAlias {
    MediaId alias_id;
    MediaId target_id;
    std::string label;
    std::uint32_t revision{0};
    bool preferred{false};
};

struct MediaStatistics {
    std::size_t assets{0};
    std::size_t aliases{0};
    std::size_t offline_assets{0};
    std::uint64_t total_frames{0};
    std::uint64_t estimated_bytes{0};
};

class MediaPool {
public:
    Status apply(const Command& command, Diagnostics& diagnostics);

    Status add_asset(MediaAsset asset, std::uint32_t transaction, Diagnostics& diagnostics);
    Status update_path(MediaId id,
                       std::string path,
                       std::uint32_t transaction,
                       Diagnostics& diagnostics);
    Status remove_asset(MediaId id, std::uint32_t transaction, Diagnostics& diagnostics);
    Status add_alias(MediaAlias alias, std::uint32_t transaction, Diagnostics& diagnostics);
    Status remove_alias(MediaId alias_id, std::uint32_t transaction, Diagnostics& diagnostics);
    Status promote_alias(MediaId alias_id, std::uint32_t transaction, Diagnostics& diagnostics);
    Status mark_offline(MediaId id,
                        bool offline,
                        std::uint32_t transaction,
                        Diagnostics& diagnostics);

    const MediaAsset* find(MediaId id) const noexcept;
    MediaAsset* find(MediaId id) noexcept;
    const MediaAlias* find_alias(MediaId id) const noexcept;
    MediaId resolve(MediaId id) const noexcept;
    std::vector<MediaId> assets_for_hash(std::uint64_t hash) const;
    std::vector<MediaId> missing_assets() const;
    std::vector<std::string> path_conflicts() const;
    MediaStatistics statistics() const noexcept;

    void rebuild_indexes();
    void compact_paths();
    void clear();

    std::size_t asset_count() const noexcept;
    std::size_t alias_count() const noexcept;
    const std::vector<MediaAsset>& assets() const noexcept;
    const std::vector<MediaAlias>& aliases() const noexcept;

    std::string describe(MediaId id) const;
    std::string manifest_text() const;

private:
    Status decode_add_asset(const Command& command, Diagnostics& diagnostics);
    Status decode_add_alias(const Command& command, Diagnostics& diagnostics);
    Status decode_update_path(const Command& command, Diagnostics& diagnostics);
    Status decode_remove(const Command& command, Diagnostics& diagnostics);
    Status decode_promote(const Command& command, Diagnostics& diagnostics);
    Status decode_status(const Command& command, Diagnostics& diagnostics);
    Status decode_reserve(const Command& command, Diagnostics& diagnostics);
    Status decode_probe(const Command& command, Diagnostics& diagnostics);

    void refresh_asset_index(std::size_t from = 0);
    void refresh_alias_index(std::size_t from = 0);
    std::uint64_t estimate_bytes(const MediaAsset& asset) const noexcept;

    std::vector<MediaAsset> assets_;
    std::vector<MediaAlias> aliases_;
    std::unordered_map<MediaId, std::size_t, EntityIdHash> asset_index_;
    std::unordered_map<MediaId, std::size_t, EntityIdHash> alias_index_;
    std::unordered_multimap<std::uint64_t, MediaId> hash_index_;
    MediaAlias* promotion_candidate_{nullptr};
    const char* path_probe_{nullptr};
    std::size_t path_probe_length_{0};
    std::uint32_t path_probe_generation_{0};
};

std::string_view media_kind_name(MediaKind kind) noexcept;

}  // namespace wavecairn
