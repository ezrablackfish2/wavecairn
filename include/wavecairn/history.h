#pragma once

#include "wavecairn/command.h"
#include "wavecairn/diagnostics.h"
#include "wavecairn/ids.h"
#include "wavecairn/status.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wavecairn {

struct SnapshotTrack {
    TrackId id;
    std::string name;
    float gain{1.0F};
    float pan{0.0F};
    std::uint32_t revision{0};
};

struct SessionSnapshot {
    EntityId id;
    std::string label;
    std::vector<SnapshotTrack> tracks;
    std::uint64_t digest{0};
    std::uint32_t revision{0};
};

struct JournalRecord {
    std::uint32_t sequence{0};
    std::uint32_t transaction{0};
    std::uint8_t opcode{0};
    std::vector<std::uint8_t> payload;
    std::uint64_t digest{0};
    bool committed{false};
};

struct UndoFrame {
    EntityId id;
    std::string label;
    std::unique_ptr<std::uint8_t[]> bytes;
    std::size_t size{0};
    std::uint32_t transaction{0};
    bool checkpoint{false};

    UndoFrame();
    UndoFrame(UndoFrame&&) noexcept;
    UndoFrame& operator=(UndoFrame&&) noexcept;
    UndoFrame(const UndoFrame&) = delete;
    UndoFrame& operator=(const UndoFrame&) = delete;
    ~UndoFrame();
};

struct ProjectRecord {
    EntityId id;
    std::string key;
    std::string value;
    std::uint32_t revision{0};
    bool tombstone{false};
};

struct HistoryStatistics {
    std::size_t snapshots{0};
    std::size_t snapshot_tracks{0};
    std::size_t journal_records{0};
    std::size_t committed_journal_records{0};
    std::size_t undo_frames{0};
    std::size_t project_records{0};
};

class HistoryStore {
public:
    HistoryStore();
    ~HistoryStore();
    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;

    Status apply(const Command& command, Diagnostics& diagnostics);

    const SessionSnapshot* find_snapshot(EntityId id) const noexcept;
    const JournalRecord* find_journal(std::uint32_t sequence) const noexcept;
    const ProjectRecord* find_record(EntityId id) const noexcept;

    std::vector<ProjectRecord> records_with_prefix(std::string prefix) const;
    std::vector<JournalRecord> committed_records() const;
    HistoryStatistics statistics() const noexcept;

    void rebuild_indexes();
    void canonicalize();
    void clear();

    const std::vector<SessionSnapshot>& snapshots() const noexcept;
    const std::vector<JournalRecord>& journal() const noexcept;
    const std::vector<ProjectRecord>& records() const noexcept;
    std::string audit_text() const;

private:
    Status apply_snapshot(const Command& command, Diagnostics& diagnostics);
    Status apply_journal(const Command& command, Diagnostics& diagnostics);
    Status apply_undo(const Command& command, Diagnostics& diagnostics);
    Status apply_project(const Command& command, Diagnostics& diagnostics);

    void rebuild_snapshot_index();
    void rebuild_journal_index();
    void rebuild_record_index();
    std::uint64_t snapshot_digest(const SessionSnapshot& snapshot) const noexcept;
    void trim_undo(std::size_t maximum);

    std::vector<SessionSnapshot> snapshots_;
    std::vector<JournalRecord> journal_;
    std::vector<UndoFrame> undo_;
    std::vector<UndoFrame> redo_;
    std::vector<ProjectRecord> records_;

    std::unordered_map<EntityId, std::size_t, EntityIdHash> snapshot_index_;
    std::unordered_map<std::uint32_t, std::size_t> journal_index_;
    std::unordered_map<EntityId, std::size_t, EntityIdHash> record_index_;

    SnapshotTrack* restored_track_{nullptr};
    const std::uint8_t* journal_probe_{nullptr};
    std::size_t journal_probe_size_{0};
    std::uint32_t journal_probe_sequence_{0};
    std::uint8_t* branch_payload_{nullptr};
    std::size_t branch_payload_size_{0};
    ProjectRecord* selected_record_{nullptr};
};

}  // namespace wavecairn
