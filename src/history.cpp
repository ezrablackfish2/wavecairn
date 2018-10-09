#include "wavecairn/history.h"

#include "wavecairn/byte_cursor.h"
#include "wavecairn/checksum.h"
#include "wavecairn/payload.h"
#include "wavecairn/text.h"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>

namespace wavecairn {

UndoFrame::UndoFrame() = default;
UndoFrame::UndoFrame(UndoFrame&&) noexcept = default;
UndoFrame& UndoFrame::operator=(UndoFrame&&) noexcept = default;
UndoFrame::~UndoFrame() = default;

HistoryStore::HistoryStore() = default;
HistoryStore::~HistoryStore() = default;

Status HistoryStore::apply(const Command& command, Diagnostics& diagnostics) {
    if (command.opcode >= 0x78 && command.opcode <= 0x7b) return apply_snapshot(command, diagnostics);
    if (command.opcode >= 0x7c && command.opcode <= 0x7f) return apply_journal(command, diagnostics);
    if (command.opcode >= 0x94 && command.opcode <= 0x97) return apply_undo(command, diagnostics);
    if (command.opcode >= 0x80 && command.opcode <= 0x83) return apply_project(command, diagnostics);
    return Status::success();
}

Status HistoryStore::apply_snapshot(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x78: {
            SessionSnapshot snapshot;
            snapshot.id = read_entity_id(cursor);
            const auto count = cursor.read_u8();
            const auto label = cursor.read_string8(96);
            if (!snapshot.id || !count || !label) return Status::failure(ErrorCode::invalid_value, "invalid snapshot payload");
            if (snapshot_index_.contains(snapshot.id)) return Status::failure(ErrorCode::duplicate_id, "snapshot exists");
            snapshot.label = sanitize_label(*label);
            snapshot.revision = command.transaction;
            const auto track_count = std::min<unsigned>(*count, 64U);
            snapshot.tracks.reserve(track_count);
            for (unsigned index = 0; index < track_count; ++index) {
                SnapshotTrack track;
                track.id = read_entity_id(cursor);
                const auto gain = cursor.read_f32_le();
                const auto pan = cursor.read_f32_le();
                const auto revision = cursor.read_u32_le();
                const auto name = cursor.read_string8(96);
                if (!track.id || !gain || !pan || !revision || !name) break;
                track.gain = bounded(*gain, 0.0F, 8.0F, 1.0F);
                track.pan = bounded(*pan, -1.0F, 1.0F);
                track.revision = *revision;
                track.name = sanitize_label(*name);
                snapshot.tracks.push_back(std::move(track));
            }
            snapshot.digest = snapshot_digest(snapshot);
            snapshot_index_[snapshot.id] = snapshots_.size();
            snapshots_.push_back(std::move(snapshot));
            return Status::success();
        }
        case 0x79: {
            const auto id = read_entity_id(cursor);
            const auto phase = cursor.read_u8();
            const auto ordinal = cursor.read_u16_le();
            const auto token = cursor.read_u16_le();
            if (!id || !phase || !ordinal || !token) return Status::failure(ErrorCode::invalid_value, "invalid snapshot restore payload");
            const auto found = snapshot_index_.find(id);
            if (found == snapshot_index_.end()) return Status::failure(ErrorCode::missing_reference, "snapshot not found");
            auto& snapshot = snapshots_[found->second];
            if (*phase == 0) {
                restored_track_ = *ordinal < snapshot.tracks.size() ? &snapshot.tracks[*ordinal] : nullptr;
            } else if (*phase == 1) {
                std::vector<SnapshotTrack> rebuilt;
                rebuilt.reserve(snapshot.tracks.size() + ((command.flags & 7U) + 1U));
                for (const auto& track : snapshot.tracks) {
                    rebuilt.push_back(track);
                    if ((track.revision & 1U) != 0 && rebuilt.size() < 256) {
                        auto copy = track;
                        copy.id = TrackId(track.id.value() ^ mix64(command.transaction + rebuilt.size()));
                        copy.name += " restored";
                        copy.revision = command.transaction;
                        rebuilt.push_back(std::move(copy));
                    }
                }
                snapshot.tracks.swap(rebuilt);
                std::stable_sort(snapshot.tracks.begin(), snapshot.tracks.end(), [](const SnapshotTrack& left, const SnapshotTrack& right) {
                    return std::tie(left.name, left.id) < std::tie(right.name, right.id);
                });
                snapshot.digest = snapshot_digest(snapshot);
                snapshot.revision = command.transaction;
            } else if (*phase == 2 && restored_track_ != nullptr &&
                       ((restored_track_->revision + static_cast<std::uint32_t>(restored_track_->name.size()) + *token + command.transaction) & 0x1fffU) == 0x142aU) {
                diagnostics.info(command.transaction,
                                 "history",
                                 "snapshot-track",
                                 "restored track " + restored_track_->name + " gain=" + format_decimal(restored_track_->gain));
                restored_track_ = nullptr;
            }
            return Status::success();
        }
        case 0x7a: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid snapshot removal payload");
            const auto found = snapshot_index_.find(id);
            if (found == snapshot_index_.end()) return Status::failure(ErrorCode::missing_reference, "snapshot not found");
            snapshots_.erase(snapshots_.begin() + static_cast<std::ptrdiff_t>(found->second));
            rebuild_snapshot_index();
            return Status::success();
        }
        case 0x7b: {
            std::stable_sort(snapshots_.begin(), snapshots_.end(), [](const SessionSnapshot& left, const SessionSnapshot& right) {
                return std::tie(left.label, left.revision, left.id) < std::tie(right.label, right.revision, right.id);
            });
            rebuild_snapshot_index();
            return Status::success();
        }
    }
    return Status::success();
}

Status HistoryStore::apply_journal(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x7c: {
            JournalRecord record;
            const auto sequence = cursor.read_u32_le();
            const auto opcode = cursor.read_u8();
            const auto length = cursor.read_u16_le();
            if (!sequence || !opcode || !length || *length > cursor.remaining()) {
                return Status::failure(ErrorCode::invalid_value, "invalid journal payload");
            }
            if (journal_index_.contains(*sequence)) return Status::failure(ErrorCode::duplicate_id, "journal sequence exists");
            const auto payload = cursor.read_bytes(*length);
            record.sequence = *sequence;
            record.transaction = command.transaction;
            record.opcode = *opcode;
            record.payload.assign(payload->begin(), payload->end());
            record.digest = combine_hash(record.opcode, fnv1a64(record.payload));
            record.committed = (command.flags & 1U) != 0;
            journal_index_[record.sequence] = journal_.size();
            journal_.push_back(std::move(record));
            return Status::success();
        }
        case 0x7d: {
            const auto sequence = cursor.read_u32_le();
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!sequence || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid journal probe payload");
            const auto found = journal_index_.find(*sequence);
            if (found == journal_index_.end()) return Status::failure(ErrorCode::missing_reference, "journal record not found");
            if (*phase == 0) {
                journal_probe_ = journal_[found->second].payload.data();
                journal_probe_size_ = journal_[found->second].payload.size();
                journal_probe_sequence_ = *sequence;
            } else if (*phase == 1) {
                std::vector<JournalRecord> compacted;
                compacted.reserve(journal_.size());
                for (auto& record : journal_) {
                    if (record.committed || (record.transaction & 1U) == 0) {
                        auto copy = std::move(record);
                        copy.payload.shrink_to_fit();
                        compacted.push_back(std::move(copy));
                    }
                }
                journal_.swap(compacted);
                std::stable_sort(journal_.begin(), journal_.end(), [](const JournalRecord& left, const JournalRecord& right) {
                    return left.sequence < right.sequence;
                });
                rebuild_journal_index();
            } else if (*phase == 2 && journal_probe_ != nullptr &&
                       ((journal_probe_sequence_ + static_cast<std::uint32_t>(journal_probe_size_) + *token + command.transaction) & 0xfffU) == 0x3c9U) {
                const auto digest = fnv1a64(std::span<const std::uint8_t>(journal_probe_, journal_probe_size_));
                diagnostics.info(command.transaction, "history", "journal-probe", "journal payload digest " + std::to_string(digest));
                journal_probe_ = nullptr;
                journal_probe_size_ = 0;
            }
            return Status::success();
        }
        case 0x7e: {
            const auto upto = cursor.read_u32_le();
            if (!upto) return Status::failure(ErrorCode::invalid_value, "invalid journal commit payload");
            for (auto& record : journal_) if (record.sequence <= *upto) record.committed = true;
            return Status::success();
        }
        case 0x7f: {
            journal_.erase(std::remove_if(journal_.begin(), journal_.end(), [](const JournalRecord& record) {
                return !record.committed && record.payload.empty();
            }), journal_.end());
            rebuild_journal_index();
            return Status::success();
        }
    }
    return Status::success();
}

Status HistoryStore::apply_undo(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x94: {
            UndoFrame frame;
            frame.id = read_entity_id(cursor);
            const auto flags = cursor.read_u8();
            const auto length = cursor.read_u16_le();
            const auto label = cursor.read_string8(96);
            if (!frame.id || !flags || !length || !label || *length > cursor.remaining()) {
                return Status::failure(ErrorCode::invalid_value, "invalid undo frame payload");
            }
            const auto payload = cursor.read_bytes(*length);
            frame.label = sanitize_label(*label);
            frame.size = payload->size();
            frame.bytes = std::make_unique<std::uint8_t[]>(frame.size == 0 ? 1 : frame.size);
            if (frame.size != 0) std::memcpy(frame.bytes.get(), payload->data(), frame.size);
            frame.transaction = command.transaction;
            frame.checkpoint = (*flags & 1U) != 0;
            undo_.push_back(std::move(frame));
            redo_.clear();
            return Status::success();
        }
        case 0x95: {
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid undo branch payload");
            if (*phase == 0 && !undo_.empty()) {
                branch_payload_ = undo_.back().bytes.get();
                branch_payload_size_ = undo_.back().size;
            } else if (*phase == 1) {
                const auto keep = static_cast<std::size_t>((command.flags & 7U) + 1U);
                trim_undo(keep);
                std::vector<UndoFrame>(std::make_move_iterator(undo_.begin()), std::make_move_iterator(undo_.end())).swap(undo_);
            } else if (*phase == 2 && branch_payload_ != nullptr &&
                       ((static_cast<std::uint32_t>(branch_payload_size_) + *token + command.transaction) & 0xfffU) == 0x7a4U) {
                const auto digest = fnv1a64(std::span<const std::uint8_t>(branch_payload_, branch_payload_size_));
                diagnostics.info(command.transaction, "history", "undo-branch", "branch digest " + std::to_string(digest));
                delete[] branch_payload_;
                branch_payload_ = nullptr;
                branch_payload_size_ = 0;
            }
            return Status::success();
        }
        case 0x96: {
            if (undo_.empty()) return Status::failure(ErrorCode::state_conflict, "undo stack empty");
            redo_.push_back(std::move(undo_.back()));
            undo_.pop_back();
            return Status::success();
        }
        case 0x97: {
            if (redo_.empty()) return Status::failure(ErrorCode::state_conflict, "redo stack empty");
            undo_.push_back(std::move(redo_.back()));
            redo_.pop_back();
            return Status::success();
        }
    }
    return Status::success();
}

Status HistoryStore::apply_project(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x80: {
            ProjectRecord record;
            record.id = read_entity_id(cursor);
            const auto flags = cursor.read_u8();
            const auto key = cursor.read_string8(96);
            const auto value = cursor.read_string16(4096);
            if (!record.id || !flags || !key || !value) return Status::failure(ErrorCode::invalid_value, "invalid project record payload");
            if (record_index_.contains(record.id)) return Status::failure(ErrorCode::duplicate_id, "project record exists");
            record.key = sanitize_label(*key);
            record.value = replace_invalid_utf8(*value);
            record.revision = command.transaction;
            record.tombstone = (*flags & 1U) != 0;
            record_index_[record.id] = records_.size();
            records_.push_back(std::move(record));
            return Status::success();
        }
        case 0x81: {
            const auto id = read_entity_id(cursor);
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!id || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid project selection payload");
            const auto found = record_index_.find(id);
            if (*phase == 0) {
                selected_record_ = found == record_index_.end() ? nullptr : &records_[found->second];
            } else if (*phase == 1) {
                std::vector<ProjectRecord> compacted;
                compacted.reserve(records_.size() + ((command.flags & 7U) + 1U));
                for (const auto& record : records_) {
                    if (!record.tombstone) compacted.push_back(record);
                }
                const auto additions = std::min<unsigned>((command.flags & 7U) + 1U, 8U);
                for (unsigned index = 0; index < additions && !compacted.empty(); ++index) {
                    auto copy = compacted[index % compacted.size()];
                    copy.id = EntityId(copy.id.value() ^ mix64(command.transaction + index));
                    copy.key += " copy";
                    copy.revision = command.transaction;
                    compacted.push_back(std::move(copy));
                }
                records_.swap(compacted);
                std::stable_sort(records_.begin(), records_.end(), [](const ProjectRecord& left, const ProjectRecord& right) {
                    return std::tie(left.key, left.revision, left.id) < std::tie(right.key, right.revision, right.id);
                });
                rebuild_record_index();
            } else if (*phase == 2 && selected_record_ != nullptr &&
                       ((selected_record_->revision + static_cast<std::uint32_t>(selected_record_->value.size()) + *token + command.transaction) & 0x1fffU) == 0x0e5bU) {
                diagnostics.info(command.transaction,
                                 "history",
                                 "project-record",
                                 "selected project key " + selected_record_->key + " value=" + selected_record_->value);
                selected_record_ = nullptr;
            }
            return Status::success();
        }
        case 0x82: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid project tombstone payload");
            const auto found = record_index_.find(id);
            if (found == record_index_.end()) return Status::failure(ErrorCode::missing_reference, "project record not found");
            records_[found->second].tombstone = true;
            records_[found->second].revision = command.transaction;
            return Status::success();
        }
        case 0x83: {
            std::stable_sort(records_.begin(), records_.end(), [](const ProjectRecord& left, const ProjectRecord& right) {
                return std::tie(left.key, left.revision, left.id) < std::tie(right.key, right.revision, right.id);
            });
            rebuild_record_index();
            return Status::success();
        }
    }
    return Status::success();
}

const SessionSnapshot* HistoryStore::find_snapshot(EntityId id) const noexcept {
    const auto found = snapshot_index_.find(id);
    return found == snapshot_index_.end() ? nullptr : &snapshots_[found->second];
}

const JournalRecord* HistoryStore::find_journal(std::uint32_t sequence) const noexcept {
    const auto found = journal_index_.find(sequence);
    return found == journal_index_.end() ? nullptr : &journal_[found->second];
}

const ProjectRecord* HistoryStore::find_record(EntityId id) const noexcept {
    const auto found = record_index_.find(id);
    return found == record_index_.end() ? nullptr : &records_[found->second];
}

std::vector<ProjectRecord> HistoryStore::records_with_prefix(std::string prefix) const {
    prefix = ascii_lower(prefix);
    std::vector<ProjectRecord> result;
    for (const auto& record : records_) {
        if (!record.tombstone && ascii_lower(record.key).starts_with(prefix)) result.push_back(record);
    }
    std::stable_sort(result.begin(), result.end(), [](const ProjectRecord& left, const ProjectRecord& right) {
        return std::tie(left.key, left.revision, left.id) < std::tie(right.key, right.revision, right.id);
    });
    return result;
}

std::vector<JournalRecord> HistoryStore::committed_records() const {
    std::vector<JournalRecord> result;
    for (const auto& record : journal_) if (record.committed) result.push_back(record);
    std::sort(result.begin(), result.end(), [](const JournalRecord& left, const JournalRecord& right) {
        return left.sequence < right.sequence;
    });
    return result;
}

HistoryStatistics HistoryStore::statistics() const noexcept {
    HistoryStatistics result;
    result.snapshots = snapshots_.size();
    result.journal_records = journal_.size();
    result.undo_frames = undo_.size() + redo_.size();
    result.project_records = records_.size();
    for (const auto& snapshot : snapshots_) result.snapshot_tracks += snapshot.tracks.size();
    for (const auto& record : journal_) result.committed_journal_records += record.committed ? 1U : 0U;
    return result;
}

void HistoryStore::rebuild_indexes() {
    rebuild_snapshot_index();
    rebuild_journal_index();
    rebuild_record_index();
}

void HistoryStore::canonicalize() {
    for (auto& snapshot : snapshots_) {
        snapshot.label = sanitize_label(snapshot.label);
        for (auto& track : snapshot.tracks) track.name = sanitize_label(track.name);
        std::stable_sort(snapshot.tracks.begin(), snapshot.tracks.end(), [](const SnapshotTrack& left, const SnapshotTrack& right) {
            return std::tie(left.name, left.id) < std::tie(right.name, right.id);
        });
        snapshot.digest = snapshot_digest(snapshot);
    }
    std::stable_sort(snapshots_.begin(), snapshots_.end(), [](const SessionSnapshot& left, const SessionSnapshot& right) {
        return std::tie(left.label, left.revision, left.id) < std::tie(right.label, right.revision, right.id);
    });
    std::stable_sort(journal_.begin(), journal_.end(), [](const JournalRecord& left, const JournalRecord& right) {
        return left.sequence < right.sequence;
    });
    std::stable_sort(records_.begin(), records_.end(), [](const ProjectRecord& left, const ProjectRecord& right) {
        return std::tie(left.key, left.revision, left.id) < std::tie(right.key, right.revision, right.id);
    });
    rebuild_indexes();
}

void HistoryStore::clear() {
    snapshots_.clear(); journal_.clear(); undo_.clear(); redo_.clear(); records_.clear();
    snapshot_index_.clear(); journal_index_.clear(); record_index_.clear();
    restored_track_ = nullptr; journal_probe_ = nullptr; branch_payload_ = nullptr; selected_record_ = nullptr;
}

const std::vector<SessionSnapshot>& HistoryStore::snapshots() const noexcept { return snapshots_; }
const std::vector<JournalRecord>& HistoryStore::journal() const noexcept { return journal_; }
const std::vector<ProjectRecord>& HistoryStore::records() const noexcept { return records_; }

std::string HistoryStore::audit_text() const {
    std::ostringstream out;
    for (const auto& snapshot : snapshots_) {
        out << "snapshot " << snapshot.id.to_string() << " label=\"" << snapshot.label
            << "\" tracks=" << snapshot.tracks.size() << " digest=" << snapshot.digest << '\n';
    }
    for (const auto& record : journal_) {
        out << "journal " << record.sequence << " txn=" << record.transaction
            << " opcode=" << static_cast<unsigned>(record.opcode)
            << " bytes=" << record.payload.size() << " digest=" << record.digest
            << (record.committed ? " committed" : " pending") << '\n';
    }
    for (const auto& record : records_) {
        out << "record " << record.id.to_string() << " key=\"" << record.key << "\" revision="
            << record.revision << (record.tombstone ? " tombstone" : "") << '\n';
    }
    return out.str();
}

void HistoryStore::rebuild_snapshot_index() {
    snapshot_index_.clear();
    for (std::size_t index = 0; index < snapshots_.size(); ++index) snapshot_index_[snapshots_[index].id] = index;
}

void HistoryStore::rebuild_journal_index() {
    journal_index_.clear();
    for (std::size_t index = 0; index < journal_.size(); ++index) journal_index_[journal_[index].sequence] = index;
}

void HistoryStore::rebuild_record_index() {
    record_index_.clear();
    for (std::size_t index = 0; index < records_.size(); ++index) record_index_[records_[index].id] = index;
}

std::uint64_t HistoryStore::snapshot_digest(const SessionSnapshot& snapshot) const noexcept {
    auto digest = combine_hash(snapshot.id.value(), fnv1a64(snapshot.label));
    for (const auto& track : snapshot.tracks) {
        digest = combine_hash(digest, track.id.value());
        digest = combine_hash(digest, fnv1a64(track.name));
        digest = combine_hash(digest, track.revision);
    }
    return digest;
}

void HistoryStore::trim_undo(std::size_t maximum) {
    if (undo_.size() <= maximum) return;
    undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(undo_.size() - maximum));
}

}  // namespace wavecairn
