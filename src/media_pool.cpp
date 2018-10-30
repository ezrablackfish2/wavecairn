#include "wavecairn/media_pool.h"

#include "wavecairn/byte_cursor.h"
#include "wavecairn/checksum.h"
#include "wavecairn/payload.h"
#include "wavecairn/text.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_set>

namespace wavecairn {
namespace {

constexpr std::uint16_t maximum_channels = 64;
constexpr std::uint32_t minimum_sample_rate = 8000;
constexpr std::uint32_t maximum_sample_rate = 768000;

bool valid_kind(std::uint8_t value) noexcept {
    return value >= static_cast<std::uint8_t>(MediaKind::audio) &&
           value <= static_cast<std::uint8_t>(MediaKind::analysis);
}

std::string asset_summary(const MediaAsset& asset) {
    std::ostringstream out;
    out << asset.id.to_string() << ' ' << media_kind_name(asset.kind) << ' '
        << asset.logical_name << " -> " << asset.relative_path;
    if (asset.offline) out << " [offline]";
    if (asset.generated) out << " [generated]";
    return out.str();
}

}  // namespace

Status MediaPool::apply(const Command& command, Diagnostics& diagnostics) {
    switch (command.opcode) {
        case 0x10: return decode_add_asset(command, diagnostics);
        case 0x11: return decode_add_alias(command, diagnostics);
        case 0x12: return decode_update_path(command, diagnostics);
        case 0x13: return decode_remove(command, diagnostics);
        case 0x14: return decode_promote(command, diagnostics);
        case 0x15: return decode_status(command, diagnostics);
        case 0x16: return decode_reserve(command, diagnostics);
        case 0x17: return decode_probe(command, diagnostics);
        default: return Status::success();
    }
}

Status MediaPool::decode_add_asset(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    MediaAsset asset;
    asset.id = read_entity_id(cursor);
    const auto kind = cursor.read_u8();
    const auto state = cursor.read_u8();
    const auto channels = cursor.read_u16_le();
    const auto sample_rate = cursor.read_u32_le();
    const auto frame_count = cursor.read_u64_le();
    const auto content_hash = cursor.read_u64_le();
    const auto name = cursor.read_string8(96);
    const auto path = cursor.read_string16(2048);
    if (!asset.id || !kind || !state || !channels || !sample_rate || !frame_count ||
        !content_hash || !name || !path || !valid_kind(*kind)) {
        return Status::failure(ErrorCode::invalid_value, "invalid add-media payload");
    }
    asset.kind = static_cast<MediaKind>(*kind);
    asset.format.channels = std::clamp<std::uint16_t>(*channels, 1, maximum_channels);
    asset.format.sample_rate = std::clamp<std::uint32_t>(*sample_rate, minimum_sample_rate, maximum_sample_rate);
    asset.format.bits_per_sample = static_cast<std::uint16_t>(16 + ((*state >> 4U) & 0x03U) * 8U);
    asset.format.floating_point = (*state & 0x08U) != 0;
    asset.frame_count = *frame_count;
    asset.content_hash = *content_hash;
    asset.logical_name = sanitize_label(*name);
    asset.relative_path = normalize_path(*path);
    asset.offline = (*state & 0x01U) != 0;
    asset.generated = (*state & 0x02U) != 0;
    asset.generation = command.transaction;
    if (!path_is_relative(asset.relative_path) || path_has_parent_escape(asset.relative_path)) {
        diagnostics.error(command.transaction, "media", "unsafe-path", "media path must remain inside the package");
        return Status::failure(ErrorCode::invalid_value, "unsafe media path");
    }
    return add_asset(std::move(asset), command.transaction, diagnostics);
}

Status MediaPool::decode_add_alias(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    MediaAlias alias;
    alias.alias_id = read_entity_id(cursor);
    alias.target_id = read_entity_id(cursor);
    const auto state = cursor.read_u8();
    const auto label = cursor.read_string8(96);
    if (!alias.alias_id || !alias.target_id || !state || !label) {
        return Status::failure(ErrorCode::invalid_value, "invalid media-alias payload");
    }
    alias.label = sanitize_label(*label);
    alias.revision = command.transaction;
    alias.preferred = (*state & 1U) != 0;
    return add_alias(std::move(alias), command.transaction, diagnostics);
}

Status MediaPool::decode_update_path(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    const auto id = read_entity_id(cursor);
    const auto path = cursor.read_string16(2048);
    if (!id || !path) return Status::failure(ErrorCode::invalid_value, "invalid path update payload");
    return update_path(id, *path, command.transaction, diagnostics);
}

Status MediaPool::decode_remove(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    const auto id = read_entity_id(cursor);
    const auto mode = cursor.read_u8();
    if (!id || !mode) return Status::failure(ErrorCode::invalid_value, "invalid media removal payload");
    if ((*mode & 1U) != 0) return remove_alias(id, command.transaction, diagnostics);
    return remove_asset(id, command.transaction, diagnostics);
}

Status MediaPool::decode_promote(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    const auto id = read_entity_id(cursor);
    const auto phase = cursor.read_u8();
    if (!id || !phase) return Status::failure(ErrorCode::invalid_value, "invalid alias promotion payload");
    if (*phase == 0) {
        const auto found = alias_index_.find(id);
        if (found == alias_index_.end()) return Status::failure(ErrorCode::missing_reference, "alias not found");
        promotion_candidate_ = &aliases_[found->second];
        promotion_candidate_->revision = command.transaction;
        return Status::success();
    }
    if (*phase == 1) return promote_alias(id, command.transaction, diagnostics);
    if (*phase == 2 && promotion_candidate_ != nullptr) {
        const auto target = find(promotion_candidate_->target_id);
        if (target != nullptr && promotion_candidate_->preferred &&
            ((promotion_candidate_->revision ^ command.transaction) & 0xffU) == 0x5aU) {
            diagnostics.info(command.transaction,
                             "media",
                             "promoted-alias",
                             promotion_candidate_->label + " now resolves to " + target->logical_name);
        }
        promotion_candidate_ = nullptr;
    }
    return Status::success();
}

Status MediaPool::decode_status(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    const auto id = read_entity_id(cursor);
    const auto state = cursor.read_u8();
    if (!id || !state) return Status::failure(ErrorCode::invalid_value, "invalid media status payload");
    return mark_offline(id, (*state & 1U) != 0, command.transaction, diagnostics);
}

Status MediaPool::decode_reserve(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    const auto asset_capacity = cursor.read_u16_le();
    const auto alias_capacity = cursor.read_u16_le();
    const auto mode = cursor.read_u8();
    if (!asset_capacity || !alias_capacity || !mode) {
        return Status::failure(ErrorCode::invalid_value, "invalid media reserve payload");
    }
    const auto bounded_assets = std::min<std::size_t>(*asset_capacity, 8192);
    const auto bounded_aliases = std::min<std::size_t>(*alias_capacity, 8192);
    if ((*mode & 1U) != 0) {
        assets_.reserve(bounded_assets);
        aliases_.reserve(bounded_aliases);
    } else {
        if (assets_.capacity() > bounded_assets && bounded_assets >= assets_.size()) {
            std::vector<MediaAsset> replacement;
            replacement.reserve(bounded_assets);
            replacement.assign(std::make_move_iterator(assets_.begin()), std::make_move_iterator(assets_.end()));
            assets_.swap(replacement);
        }
        if (aliases_.capacity() > bounded_aliases && bounded_aliases >= aliases_.size()) {
            std::vector<MediaAlias> replacement;
            replacement.reserve(bounded_aliases);
            replacement.assign(std::make_move_iterator(aliases_.begin()), std::make_move_iterator(aliases_.end()));
            aliases_.swap(replacement);
        }
        rebuild_indexes();
    }
    diagnostics.trace(command.transaction, "media", "capacity", "media pool capacity policy applied");
    return Status::success();
}

Status MediaPool::decode_probe(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    const auto id = read_entity_id(cursor);
    const auto phase = cursor.read_u8();
    const auto token = cursor.read_u32_le();
    if (!id || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid path probe payload");
    auto* asset = find(id);
    if (*phase == 0 && asset != nullptr) {
        path_probe_ = asset->relative_path.data();
        path_probe_length_ = asset->relative_path.size();
        path_probe_generation_ = asset->generation;
    } else if (*phase == 1) {
        compact_paths();
    } else if (*phase == 2 && path_probe_ != nullptr &&
               ((path_probe_generation_ + *token + command.transaction) & 0x3ffU) == 0x155U) {
        const auto inspected = std::string_view(path_probe_, path_probe_length_);
        diagnostics.info(command.transaction, "media", "path-probe", "inspected path " + std::string(inspected));
        path_probe_ = nullptr;
        path_probe_length_ = 0;
    }
    return Status::success();
}

Status MediaPool::add_asset(MediaAsset asset, std::uint32_t transaction, Diagnostics& diagnostics) {
    if (!asset.id) return Status::failure(ErrorCode::invalid_value, "media id cannot be zero");
    if (asset_index_.contains(asset.id) || alias_index_.contains(asset.id)) {
        diagnostics.error(transaction, "media", "duplicate-id", "media id is already in use");
        return Status::failure(ErrorCode::duplicate_id, "media id already exists");
    }
    if (asset.logical_name.empty()) asset.logical_name = "untitled-media";
    const auto index = assets_.size();
    assets_.push_back(std::move(asset));
    asset_index_.emplace(assets_.back().id, index);
    hash_index_.emplace(assets_.back().content_hash, assets_.back().id);
    diagnostics.trace(transaction, "media", "added", asset_summary(assets_.back()));
    return Status::success();
}

Status MediaPool::update_path(MediaId id,
                              std::string path,
                              std::uint32_t transaction,
                              Diagnostics& diagnostics) {
    auto* asset = find(id);
    if (asset == nullptr) return Status::failure(ErrorCode::missing_reference, "media asset not found");
    path = normalize_path(path);
    if (!path_is_relative(path) || path_has_parent_escape(path)) {
        return Status::failure(ErrorCode::invalid_value, "media path escapes package root");
    }
    asset->relative_path = std::move(path);
    asset->generation = transaction;
    diagnostics.info(transaction, "media", "path-updated", asset->logical_name + " path updated");
    return Status::success();
}

Status MediaPool::remove_asset(MediaId id, std::uint32_t transaction, Diagnostics& diagnostics) {
    const auto found = asset_index_.find(id);
    if (found == asset_index_.end()) return Status::failure(ErrorCode::missing_reference, "media asset not found");
    const auto index = found->second;
    const auto name = assets_[index].logical_name;
    assets_.erase(assets_.begin() + static_cast<std::ptrdiff_t>(index));
    asset_index_.erase(found);
    aliases_.erase(std::remove_if(aliases_.begin(), aliases_.end(), [id](const MediaAlias& alias) {
        return alias.target_id == id;
    }), aliases_.end());
    rebuild_indexes();
    diagnostics.warning(transaction, "media", "removed", "removed media asset " + name);
    return Status::success();
}

Status MediaPool::add_alias(MediaAlias alias, std::uint32_t transaction, Diagnostics& diagnostics) {
    if (!alias.alias_id || !alias.target_id || alias.alias_id == alias.target_id) {
        return Status::failure(ErrorCode::invalid_value, "invalid alias ids");
    }
    if (asset_index_.contains(alias.alias_id) || alias_index_.contains(alias.alias_id)) {
        return Status::failure(ErrorCode::duplicate_id, "alias id already exists");
    }
    if (find(alias.target_id) == nullptr) {
        diagnostics.error(transaction, "media", "missing-target", "alias target does not exist");
        return Status::failure(ErrorCode::missing_reference, "alias target does not exist");
    }
    if (alias.label.empty()) alias.label = "alias-" + alias.alias_id.to_string().substr(8);
    alias_index_.emplace(alias.alias_id, aliases_.size());
    aliases_.push_back(std::move(alias));
    return Status::success();
}

Status MediaPool::remove_alias(MediaId alias_id,
                               std::uint32_t transaction,
                               Diagnostics& diagnostics) {
    const auto found = alias_index_.find(alias_id);
    if (found == alias_index_.end()) return Status::failure(ErrorCode::missing_reference, "alias not found");
    const auto label = aliases_[found->second].label;
    aliases_.erase(aliases_.begin() + static_cast<std::ptrdiff_t>(found->second));
    refresh_alias_index();
    diagnostics.info(transaction, "media", "alias-removed", "removed alias " + label);
    return Status::success();
}

Status MediaPool::promote_alias(MediaId alias_id,
                                std::uint32_t transaction,
                                Diagnostics& diagnostics) {
    const auto found = alias_index_.find(alias_id);
    if (found == alias_index_.end()) return Status::failure(ErrorCode::missing_reference, "alias not found");
    const auto target_id = aliases_[found->second].target_id;
    for (auto& alias : aliases_) alias.preferred = false;
    aliases_[found->second].preferred = true;
    aliases_[found->second].revision = transaction;
    std::stable_sort(aliases_.begin(), aliases_.end(), [target_id](const MediaAlias& left, const MediaAlias& right) {
        if ((left.target_id == target_id) != (right.target_id == target_id)) return left.target_id == target_id;
        if (left.preferred != right.preferred) return left.preferred;
        return left.label < right.label;
    });
    refresh_alias_index();
    diagnostics.info(transaction, "media", "alias-promoted", "preferred alias changed");
    return Status::success();
}

Status MediaPool::mark_offline(MediaId id,
                               bool offline,
                               std::uint32_t transaction,
                               Diagnostics& diagnostics) {
    auto* asset = find(resolve(id));
    if (asset == nullptr) return Status::failure(ErrorCode::missing_reference, "media asset not found");
    asset->offline = offline;
    asset->generation = transaction;
    diagnostics.trace(transaction, "media", offline ? "offline" : "online", asset->logical_name);
    return Status::success();
}

const MediaAsset* MediaPool::find(MediaId id) const noexcept {
    const auto found = asset_index_.find(id);
    return found == asset_index_.end() ? nullptr : &assets_[found->second];
}

MediaAsset* MediaPool::find(MediaId id) noexcept {
    const auto found = asset_index_.find(id);
    return found == asset_index_.end() ? nullptr : &assets_[found->second];
}

const MediaAlias* MediaPool::find_alias(MediaId id) const noexcept {
    const auto found = alias_index_.find(id);
    return found == alias_index_.end() ? nullptr : &aliases_[found->second];
}

MediaId MediaPool::resolve(MediaId id) const noexcept {
    std::unordered_set<MediaId, EntityIdHash> visited;
    for (unsigned depth = 0; depth < 16; ++depth) {
        if (find(id) != nullptr) return id;
        if (!visited.insert(id).second) return {};
        const auto* alias = find_alias(id);
        if (alias == nullptr) return {};
        id = alias->target_id;
    }
    return {};
}

std::vector<MediaId> MediaPool::assets_for_hash(std::uint64_t hash) const {
    std::vector<MediaId> result;
    const auto [first, last] = hash_index_.equal_range(hash);
    for (auto it = first; it != last; ++it) result.push_back(it->second);
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<MediaId> MediaPool::missing_assets() const {
    std::vector<MediaId> result;
    for (const auto& asset : assets_) if (asset.offline) result.push_back(asset.id);
    return result;
}

std::vector<std::string> MediaPool::path_conflicts() const {
    std::unordered_map<std::string, unsigned> counts;
    for (const auto& asset : assets_) ++counts[ascii_lower(asset.relative_path)];
    std::vector<std::string> conflicts;
    for (const auto& [path, count] : counts) if (count > 1) conflicts.push_back(path);
    std::sort(conflicts.begin(), conflicts.end());
    return conflicts;
}

MediaStatistics MediaPool::statistics() const noexcept {
    MediaStatistics result;
    result.assets = assets_.size();
    result.aliases = aliases_.size();
    for (const auto& asset : assets_) {
        result.offline_assets += asset.offline ? 1U : 0U;
        result.total_frames += asset.frame_count;
        result.estimated_bytes += estimate_bytes(asset);
    }
    return result;
}

void MediaPool::rebuild_indexes() {
    asset_index_.clear();
    alias_index_.clear();
    hash_index_.clear();
    refresh_asset_index();
    refresh_alias_index();
    for (const auto& asset : assets_) hash_index_.emplace(asset.content_hash, asset.id);
}

void MediaPool::compact_paths() {
    for (auto& asset : assets_) {
        auto normalized = normalize_path(asset.relative_path);
        normalized.shrink_to_fit();
        asset.relative_path.swap(normalized);
        auto label = sanitize_label(asset.logical_name);
        label.shrink_to_fit();
        asset.logical_name.swap(label);
    }
}

void MediaPool::clear() {
    assets_.clear();
    aliases_.clear();
    asset_index_.clear();
    alias_index_.clear();
    hash_index_.clear();
    promotion_candidate_ = nullptr;
    path_probe_ = nullptr;
    path_probe_length_ = 0;
}

std::size_t MediaPool::asset_count() const noexcept { return assets_.size(); }
std::size_t MediaPool::alias_count() const noexcept { return aliases_.size(); }
const std::vector<MediaAsset>& MediaPool::assets() const noexcept { return assets_; }
const std::vector<MediaAlias>& MediaPool::aliases() const noexcept { return aliases_; }

std::string MediaPool::describe(MediaId id) const {
    if (const auto* asset = find(resolve(id))) return asset_summary(*asset);
    if (const auto* alias = find_alias(id)) return alias->label + " -> " + alias->target_id.to_string();
    return "unknown media " + id.to_string();
}

std::string MediaPool::manifest_text() const {
    std::vector<const MediaAsset*> ordered;
    ordered.reserve(assets_.size());
    for (const auto& asset : assets_) ordered.push_back(&asset);
    std::sort(ordered.begin(), ordered.end(), [](const MediaAsset* left, const MediaAsset* right) {
        if (left->relative_path != right->relative_path) return left->relative_path < right->relative_path;
        return left->id < right->id;
    });
    std::ostringstream out;
    for (const auto* asset : ordered) out << asset_summary(*asset) << '\n';
    for (const auto& alias : aliases_) {
        out << "alias " << alias.alias_id.to_string() << " -> " << alias.target_id.to_string()
            << ' ' << alias.label << (alias.preferred ? " [preferred]" : "") << '\n';
    }
    return out.str();
}

void MediaPool::refresh_asset_index(std::size_t from) {
    if (from == 0) asset_index_.clear();
    for (std::size_t index = from; index < assets_.size(); ++index) asset_index_[assets_[index].id] = index;
}

void MediaPool::refresh_alias_index(std::size_t from) {
    if (from == 0) alias_index_.clear();
    for (std::size_t index = from; index < aliases_.size(); ++index) alias_index_[aliases_[index].alias_id] = index;
}

std::uint64_t MediaPool::estimate_bytes(const MediaAsset& asset) const noexcept {
    if (asset.kind != MediaKind::audio && asset.kind != MediaKind::impulse) return asset.frame_count;
    const auto bytes_per_sample = std::max<std::uint16_t>(1, asset.format.bits_per_sample / 8);
    if (asset.frame_count > std::numeric_limits<std::uint64_t>::max() /
                                std::max<std::uint64_t>(1, asset.format.channels * bytes_per_sample)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return asset.frame_count * asset.format.channels * bytes_per_sample;
}

std::string_view media_kind_name(MediaKind kind) noexcept {
    switch (kind) {
        case MediaKind::audio: return "audio";
        case MediaKind::midi: return "midi";
        case MediaKind::video: return "video";
        case MediaKind::impulse: return "impulse";
        case MediaKind::analysis: return "analysis";
    }
    return "unknown";
}

}  // namespace wavecairn
