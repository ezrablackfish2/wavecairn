#include "wavecairn/session.h"

#include "wavecairn/checksum.h"
#include "wavecairn/format_registry.h"
#include "wavecairn/tape.h"
#include "wavecairn/text.h"

#include <algorithm>
#include <array>
#include <sstream>

namespace wavecairn {
namespace {

bool profile_accepts(ProcessingProfile profile, CommandCategory category) noexcept {
    if (profile == ProcessingProfile::complete) return true;
    switch (profile) {
        case ProcessingProfile::import:
            return category == CommandCategory::media ||
                   category == CommandCategory::track ||
                   category == CommandCategory::clip ||
                   category == CommandCategory::marker ||
                   category == CommandCategory::snapshot ||
                   category == CommandCategory::package ||
                   category == CommandCategory::repair;
        case ProcessingProfile::editing:
            return category == CommandCategory::track ||
                   category == CommandCategory::clip ||
                   category == CommandCategory::tempo ||
                   category == CommandCategory::automation ||
                   category == CommandCategory::marker ||
                   category == CommandCategory::take ||
                   category == CommandCategory::journal ||
                   category == CommandCategory::undo;
        case ProcessingProfile::routing:
            return category == CommandCategory::plugin ||
                   category == CommandCategory::send ||
                   category == CommandCategory::event ||
                   category == CommandCategory::query ||
                   category == CommandCategory::track;
        case ProcessingProfile::rendering:
            return category == CommandCategory::render ||
                   category == CommandCategory::cache ||
                   category == CommandCategory::waveform ||
                   category == CommandCategory::export_data ||
                   category == CommandCategory::track;
        case ProcessingProfile::package:
            return category == CommandCategory::project ||
                   category == CommandCategory::package ||
                   category == CommandCategory::repair ||
                   category == CommandCategory::query ||
                   category == CommandCategory::snapshot ||
                   category == CommandCategory::journal;
        case ProcessingProfile::complete:
            return true;
    }
    return false;
}

void barrier_checks(const Session& session, std::uint32_t transaction, Diagnostics& diagnostics) {
    for (const auto& clip : session.timeline().clips()) {
        if (session.timeline().find_track(clip.track_id) == nullptr) {
            diagnostics.error(transaction, "session", "clip-track", "clip references a missing track");
        }
        if (session.media().resolve(clip.media_id).valid() == false) {
            diagnostics.warning(transaction, "session", "clip-media", "clip references unresolved media");
        }
    }
    for (const auto& take : session.timeline().takes()) {
        if (session.timeline().find_track(take.track_id) == nullptr) {
            diagnostics.error(transaction, "session", "take-track", "take references a missing track");
        }
        if (!session.media().resolve(take.media_id)) {
            diagnostics.warning(transaction, "session", "take-media", "take references unresolved media");
        }
    }
    if (session.routing().has_cycle()) {
        diagnostics.error(transaction, "session", "plugin-cycle", "plug-in graph contains a cycle");
    }
    for (const auto& job : session.rendering().jobs()) {
        if (session.timeline().find_track(job.track_id) == nullptr) {
            diagnostics.warning(transaction, "session", "render-track", "render job references a missing track");
        }
    }
    for (const auto& conflict : session.media().path_conflicts()) {
        diagnostics.warning(transaction, "session", "media-path-conflict", "multiple assets use " + conflict);
    }
}

}  // namespace

Session::Session() {
    metadata_.id = EntityId(1);
    metadata_.title = "Untitled Session";
    metadata_.application = "WaveCairn";
}

Status Session::apply(const Command& command, Diagnostics& diagnostics) {
    const auto category = describe_opcode(command.opcode).category;
    switch (category) {
        case CommandCategory::media:
            return media_.apply(command, diagnostics);
        case CommandCategory::track:
        case CommandCategory::clip:
        case CommandCategory::tempo:
        case CommandCategory::automation:
        case CommandCategory::marker:
        case CommandCategory::take:
            return timeline_.apply(command, diagnostics);
        case CommandCategory::plugin:
        case CommandCategory::send:
        case CommandCategory::event:
        case CommandCategory::query:
            return routing_.apply(command, diagnostics);
        case CommandCategory::render:
        case CommandCategory::cache:
        case CommandCategory::waveform:
        case CommandCategory::export_data:
            return rendering_.apply(command, diagnostics);
        case CommandCategory::snapshot:
        case CommandCategory::journal:
        case CommandCategory::project:
        case CommandCategory::undo:
            return history_.apply(command, diagnostics);
        case CommandCategory::package:
        case CommandCategory::repair:
            return package_.apply(command, diagnostics);
        case CommandCategory::control:
        case CommandCategory::unknown:
            return Status::success();
    }
    return Status::success();
}

void Session::canonicalize() {
    metadata_.title = sanitize_label(metadata_.title, 160);
    metadata_.creator = sanitize_label(metadata_.creator, 160);
    metadata_.application = sanitize_label(metadata_.application, 160);
    media_.compact_paths();
    media_.rebuild_indexes();
    timeline_.canonicalize();
    routing_.canonicalize();
    rendering_.canonicalize();
    history_.canonicalize();
    package_.canonicalize();
}

void Session::clear() {
    metadata_ = SessionMetadata{};
    metadata_.id = EntityId(1);
    metadata_.title = "Untitled Session";
    metadata_.application = "WaveCairn";
    media_.clear();
    timeline_.clear();
    routing_.clear();
    rendering_.clear();
    history_.clear();
    package_.clear();
}

SessionMetadata& Session::metadata() noexcept { return metadata_; }
const SessionMetadata& Session::metadata() const noexcept { return metadata_; }
MediaPool& Session::media() noexcept { return media_; }
const MediaPool& Session::media() const noexcept { return media_; }
Timeline& Session::timeline() noexcept { return timeline_; }
const Timeline& Session::timeline() const noexcept { return timeline_; }
RoutingGraph& Session::routing() noexcept { return routing_; }
const RoutingGraph& Session::routing() const noexcept { return routing_; }
RenderingSystem& Session::rendering() noexcept { return rendering_; }
const RenderingSystem& Session::rendering() const noexcept { return rendering_; }
HistoryStore& Session::history() noexcept { return history_; }
const HistoryStore& Session::history() const noexcept { return history_; }
PackageWorkspace& Session::package() noexcept { return package_; }
const PackageWorkspace& Session::package() const noexcept { return package_; }

std::uint64_t Session::fingerprint() const noexcept {
    auto digest = combine_hash(metadata_.id.value(), fnv1a64(metadata_.title));
    digest = combine_hash(digest, fnv1a64(metadata_.creator));
    digest = combine_hash(digest, metadata_.sample_rate);
    for (const auto& asset : media_.assets()) {
        digest = combine_hash(digest, asset.id.value());
        digest = combine_hash(digest, asset.content_hash);
        digest = combine_hash(digest, fnv1a64(asset.relative_path));
    }
    for (const auto& track : timeline_.tracks()) {
        digest = combine_hash(digest, track.id.value());
        digest = combine_hash(digest, fnv1a64(track.name));
        digest = combine_hash(digest, track.revision);
    }
    for (const auto& clip : timeline_.clips()) {
        digest = combine_hash(digest, clip.id.value());
        digest = combine_hash(digest, static_cast<std::uint64_t>(clip.timeline_start));
        digest = combine_hash(digest, static_cast<std::uint64_t>(clip.length));
    }
    for (const auto& plugin : routing_.plugins()) {
        digest = combine_hash(digest, plugin.id.value());
        digest = combine_hash(digest, plugin.generation);
    }
    digest = combine_hash(digest, package_.content_digest());
    return digest;
}

std::string Session::summary_text() const {
    const auto media_stats = media_.statistics();
    const auto timeline_stats = timeline_.statistics();
    const auto routing_stats = routing_.statistics();
    const auto render_stats = rendering_.statistics();
    const auto history_stats = history_.statistics();
    std::ostringstream out;
    out << "WaveCairn session\n"
        << "title: " << metadata_.title << '\n'
        << "creator: " << metadata_.creator << '\n'
        << "application: " << metadata_.application << '\n'
        << "sample rate: " << metadata_.sample_rate << '\n'
        << "fingerprint: " << fingerprint() << '\n'
        << "media assets: " << media_stats.assets << '\n'
        << "media aliases: " << media_stats.aliases << '\n'
        << "offline assets: " << media_stats.offline_assets << '\n'
        << "tracks: " << timeline_stats.tracks << '\n'
        << "clips: " << timeline_stats.clips << '\n'
        << "tempo segments: " << timeline_stats.tempo_segments << '\n'
        << "automation lanes: " << timeline_stats.automation_lanes << '\n'
        << "markers: " << timeline_stats.markers << '\n'
        << "takes: " << timeline_stats.takes << '\n'
        << "plug-ins: " << routing_stats.plugins << '\n'
        << "plug-in edges: " << routing_stats.edges << '\n'
        << "sends: " << routing_stats.sends << '\n'
        << "render jobs: " << render_stats.jobs << '\n'
        << "render slices: " << render_stats.slices << '\n'
        << "sample blocks: " << render_stats.sample_blocks << '\n'
        << "snapshots: " << history_stats.snapshots << '\n'
        << "journal records: " << history_stats.journal_records << '\n'
        << "package sections: " << package_.document().sections.size() << '\n';
    return out.str();
}

Result<ProcessingReport> process_command_tape(Session& session,
                                              std::span<const std::uint8_t> bytes,
                                              Diagnostics& diagnostics,
                                              const ProcessingOptions& options) {
    TapeLimits limits;
    limits.maximum_commands = options.maximum_commands;
    auto parsed = parse_command_tape(bytes, &diagnostics, limits);
    if (!parsed) return parsed.status();

    ProcessingReport report;
    for (const auto& command : parsed.value().commands) {
        ++report.commands_seen;
        report.last_transaction = command.transaction;
        const auto& descriptor = describe_opcode(command.opcode);
        if (!profile_accepts(options.profile, descriptor.category)) continue;
        const auto status = session.apply(command, diagnostics);
        if (status) {
            ++report.commands_applied;
        } else {
            ++report.commands_rejected;
            diagnostics.warning(command.transaction,
                                "session",
                                "command-rejected",
                                std::string(descriptor.name) + ": " + status.message());
            if (options.stop_on_error) break;
        }
        if (command_is_barrier(command)) {
            ++report.barriers;
            if (options.validate_barriers) barrier_checks(session, command.transaction, diagnostics);
        }
    }
    if (options.canonicalize_at_end) session.canonicalize();
    report.warnings = diagnostics.count(Severity::warning);
    report.errors = diagnostics.count(Severity::error);
    return report;
}

Result<ProcessingReport> process_package(Session& session,
                                         std::span<const std::uint8_t> bytes,
                                         Diagnostics& diagnostics,
                                         const ProcessingOptions& options) {
    auto parsed = parse_package(bytes, &diagnostics);
    if (!parsed) return parsed.status();
    session.package().load(std::move(parsed.value()), diagnostics);
    const auto tape = session.package().command_tape_bytes();
    if (tape.empty()) {
        ProcessingReport report;
        report.warnings = diagnostics.count(Severity::warning);
        report.errors = diagnostics.count(Severity::error);
        return report;
    }
    return process_command_tape(session, tape, diagnostics, options);
}

Result<ProcessingReport> process_input(Session& session,
                                       std::span<const std::uint8_t> bytes,
                                       Diagnostics& diagnostics,
                                       const ProcessingOptions& options) {
    if (bytes.size() >= 4 && bytes[0] == 'W' && bytes[1] == 'C' && bytes[2] == 'P' && bytes[3] == 'K') {
        return process_package(session, bytes, diagnostics, options);
    }
    return process_command_tape(session, bytes, diagnostics, options);
}

void fuzz_process(std::span<const std::uint8_t> bytes, ProcessingProfile profile) {
    Session session;
    Diagnostics diagnostics;
    ProcessingOptions options;
    options.profile = profile;
    options.stop_on_error = false;
    options.validate_barriers = true;
    options.canonicalize_at_end = (bytes.size() & 1U) != 0;
    options.maximum_commands = 2048;
    auto report = process_input(session, bytes, diagnostics, options);
    if (report && (bytes.size() % 17U) == 0) {
        session.fingerprint();
        session.summary_text();
    }
}

}  // namespace wavecairn
