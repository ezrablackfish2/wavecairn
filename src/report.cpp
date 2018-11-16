#include "wavecairn/report.h"

#include "wavecairn/text.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace wavecairn {
namespace {

std::string json_escape_local(std::string_view text) {
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

std::string csv_escape_local(std::string_view value) {
    bool quote = false;
    for (char ch : value) {
        if (ch == ',' || ch == '"' || ch == '\n' || ch == '\r') {
            quote = true;
            break;
        }
    }
    if (!quote) return std::string(value);
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') out += "\"\"";
        else out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

std::string yes_no(bool value) {
    return value ? "yes" : "no";
}

std::string number(double value, int precision = 3) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string integer(std::int64_t value) {
    return std::to_string(value);
}

std::string unsigned_integer(std::uint64_t value) {
    return std::to_string(value);
}

std::string markdown_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (char character : value) {
        if (character == '|') {
            result += "\\|";
        } else if (character == '\n' || character == '\r') {
            result += " ";
        } else {
            result.push_back(character);
        }
    }
    return result;
}

void append_row(ReportSection& section,
                std::initializer_list<std::string> values,
                std::size_t maximum_rows) {
    if (section.rows.size() >= maximum_rows) {
        return;
    }
    section.rows.emplace_back(values);
}

void add_overview(SessionReport& report, const Session& session) {
    ReportSection section;
    section.title = "Session overview";
    section.columns = {"Field", "Value"};
    append_row(section, {"Session ID", session.metadata().id.to_string()}, 100);
    append_row(section, {"Title", session.metadata().title}, 100);
    append_row(section, {"Creator", session.metadata().creator}, 100);
    append_row(section, {"Application", session.metadata().application}, 100);
    append_row(section, {"Sample rate", std::to_string(session.metadata().sample_rate)}, 100);
    append_row(section, {"Revision", std::to_string(session.metadata().revision)}, 100);
    append_row(section, {"Fingerprint", unsigned_integer(session.fingerprint())}, 100);
    append_row(section, {"Timeline end", integer(session.timeline().end_sample())}, 100);
    report.sections.push_back(std::move(section));
}

void add_media(SessionReport& report,
               const Session& session,
               std::size_t maximum_rows) {
    ReportSection section;
    section.title = "Media inventory";
    section.columns = {
        "ID", "Kind", "Name", "Path", "Rate", "Channels",
        "Frames", "Hash", "Offline", "Generated"
    };
    for (const auto& asset : session.media().assets()) {
        append_row(section,
                   {asset.id.to_string(),
                    std::string(media_kind_name(asset.kind)),
                    asset.logical_name,
                    asset.relative_path,
                    std::to_string(asset.format.sample_rate),
                    std::to_string(asset.format.channels),
                    unsigned_integer(asset.frame_count),
                    unsigned_integer(asset.content_hash),
                    yes_no(asset.offline),
                    yes_no(asset.generated)},
                   maximum_rows);
    }
    const auto stats = session.media().statistics();
    section.notes.push_back("Assets: " + std::to_string(stats.assets));
    section.notes.push_back("Aliases: " + std::to_string(stats.aliases));
    section.notes.push_back("Offline assets: " + std::to_string(stats.offline_assets));
    section.notes.push_back("Estimated bytes: " + unsigned_integer(stats.estimated_bytes));
    if (session.media().assets().size() > maximum_rows) {
        section.notes.push_back("Rows were limited by the report options.");
    }
    report.sections.push_back(std::move(section));

    ReportSection aliases;
    aliases.title = "Media aliases";
    aliases.columns = {"Alias ID", "Target ID", "Label", "Preferred", "Revision"};
    for (const auto& alias : session.media().aliases()) {
        append_row(aliases,
                   {alias.alias_id.to_string(),
                    alias.target_id.to_string(),
                    alias.label,
                    yes_no(alias.preferred),
                    std::to_string(alias.revision)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(aliases));
}

void add_tracks(SessionReport& report,
                const Session& session,
                std::size_t maximum_rows) {
    ReportSection tracks;
    tracks.title = "Tracks";
    tracks.columns = {
        "ID", "Name", "Channels", "Gain", "Pan", "Muted", "Solo", "Armed", "Revision"
    };
    for (const auto& track : session.timeline().tracks()) {
        append_row(tracks,
                   {track.id.to_string(),
                    track.name,
                    std::to_string(track.channels),
                    number(track.gain),
                    number(track.pan),
                    yes_no(track.muted),
                    yes_no(track.solo),
                    yes_no(track.armed),
                    std::to_string(track.revision)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(tracks));

    ReportSection clips;
    clips.title = "Clips";
    clips.columns = {
        "ID", "Track", "Media", "Timeline start", "Source start", "Length",
        "Gain", "Fade in", "Fade out", "Group", "Muted", "Locked"
    };
    for (const auto& clip : session.timeline().clips()) {
        append_row(clips,
                   {clip.id.to_string(),
                    clip.track_id.to_string(),
                    clip.media_id.to_string(),
                    integer(clip.timeline_start),
                    integer(clip.source_start),
                    integer(clip.length),
                    number(clip.gain),
                    number(clip.fade_in),
                    number(clip.fade_out),
                    std::to_string(clip.group),
                    yes_no(clip.muted),
                    yes_no(clip.locked)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(clips));
}

void add_tempo_and_markers(SessionReport& report,
                           const Session& session,
                           std::size_t maximum_rows) {
    ReportSection tempo;
    tempo.title = "Tempo map";
    tempo.columns = {
        "ID", "Start sample", "Start BPM", "End BPM", "Meter", "Ramp", "Revision"
    };
    for (const auto& segment : session.timeline().tempo_segments()) {
        append_row(tempo,
                   {segment.id.to_string(),
                    integer(segment.start_sample),
                    number(segment.start_bpm),
                    number(segment.end_bpm),
                    std::to_string(segment.numerator) + "/" +
                        std::to_string(segment.denominator),
                    yes_no(segment.ramp),
                    std::to_string(segment.revision)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(tempo));

    ReportSection markers;
    markers.title = "Markers";
    markers.columns = {
        "ID", "Position", "Duration", "Name", "Note", "Color", "Revision"
    };
    for (const auto& marker : session.timeline().markers()) {
        append_row(markers,
                   {marker.id.to_string(),
                    integer(marker.position),
                    integer(marker.duration),
                    marker.name,
                    marker.note,
                    std::to_string(marker.color),
                    std::to_string(marker.revision)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(markers));
}

void add_automation(SessionReport& report,
                    const Session& session,
                    std::size_t maximum_rows) {
    ReportSection lanes;
    lanes.title = "Automation lanes";
    lanes.columns = {
        "ID", "Track", "Parameter", "Default", "Enabled", "Points", "Revision"
    };
    for (const auto& lane : session.timeline().automation_lanes()) {
        append_row(lanes,
                   {lane.id.to_string(),
                    lane.track_id.to_string(),
                    lane.parameter,
                    number(lane.default_value),
                    yes_no(lane.enabled),
                    std::to_string(lane.points.size()),
                    std::to_string(lane.revision)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(lanes));

    ReportSection takes;
    takes.title = "Takes";
    takes.columns = {
        "ID", "Track", "Media", "Source start", "Length", "Lane", "Rating",
        "Active", "Rejected", "Revision"
    };
    for (const auto& take : session.timeline().takes()) {
        append_row(takes,
                   {take.id.to_string(),
                    take.track_id.to_string(),
                    take.media_id.to_string(),
                    integer(take.source_start),
                    integer(take.length),
                    std::to_string(take.lane),
                    number(take.rating),
                    yes_no(take.active),
                    yes_no(take.rejected),
                    std::to_string(take.revision)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(takes));
}

void add_routing(SessionReport& report,
                 const Session& session,
                 std::size_t maximum_rows) {
    ReportSection plugins;
    plugins.title = "Plugin graph";
    plugins.columns = {
        "ID", "Track", "Vendor", "Product", "Parameters", "Latency",
        "Bypassed", "Instrument", "Generation"
    };
    for (const auto& plugin : session.routing().plugins()) {
        append_row(plugins,
                   {plugin.id.to_string(),
                    plugin.track_id.to_string(),
                    plugin.vendor,
                    plugin.product,
                    std::to_string(plugin.parameters.size()),
                    std::to_string(plugin.latency),
                    yes_no(plugin.bypassed),
                    yes_no(plugin.instrument),
                    std::to_string(plugin.generation)},
                   maximum_rows);
    }
    plugins.notes.push_back("Cycle detected: " + yes_no(session.routing().has_cycle()));
    report.sections.push_back(std::move(plugins));

    ReportSection edges;
    edges.title = "Plugin connections";
    edges.columns = {
        "Source", "Destination", "Source bus", "Destination bus", "Gain", "Sidechain"
    };
    for (const auto& edge : session.routing().edges()) {
        append_row(edges,
                   {edge.source.to_string(),
                    edge.destination.to_string(),
                    std::to_string(edge.source_bus),
                    std::to_string(edge.destination_bus),
                    number(edge.gain),
                    yes_no(edge.sidechain)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(edges));

    ReportSection sends;
    sends.title = "Track sends";
    sends.columns = {
        "ID", "Source", "Destination", "Level", "Pan", "Pre-fader", "Muted", "Revision"
    };
    for (const auto& send : session.routing().sends()) {
        append_row(sends,
                   {send.id.to_string(),
                    send.source.to_string(),
                    send.destination.to_string(),
                    number(send.level),
                    number(send.pan),
                    yes_no(send.pre_fader),
                    yes_no(send.muted),
                    std::to_string(send.revision)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(sends));
}

void add_rendering(SessionReport& report,
                   const Session& session,
                   std::size_t maximum_rows) {
    ReportSection jobs;
    jobs.title = "Render jobs";
    jobs.columns = {
        "ID", "Track", "Start", "End", "Rate", "Channels", "Priority",
        "Normalize", "Dither", "Revision"
    };
    for (const auto& job : session.rendering().jobs()) {
        append_row(jobs,
                   {job.id.to_string(),
                    job.track_id.to_string(),
                    integer(job.start_sample),
                    integer(job.end_sample),
                    std::to_string(job.sample_rate),
                    std::to_string(job.channels),
                    std::to_string(job.priority),
                    yes_no(job.normalize),
                    yes_no(job.dither),
                    std::to_string(job.revision)},
                   maximum_rows);
    }
    const auto estimate = estimate_render_cost(session);
    jobs.notes.push_back("Estimated output bytes: " + unsigned_integer(estimate.estimated_output_bytes));
    jobs.notes.push_back("Estimated work units: " + unsigned_integer(estimate.estimated_work_units));
    jobs.notes.push_back("Estimated seconds: " + number(estimate.estimated_seconds));
    report.sections.push_back(std::move(jobs));

    ReportSection slices;
    slices.title = "Render slices";
    slices.columns = {
        "Job", "Start", "End", "Worker", "Sequence"
    };
    for (const auto& slice : session.rendering().slices()) {
        append_row(slices,
                   {slice.job_id.to_string(),
                    integer(slice.start_sample),
                    integer(slice.end_sample),
                    std::to_string(slice.worker),
                    std::to_string(slice.sequence)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(slices));
}

void add_history(SessionReport& report,
                 const Session& session,
                 std::size_t maximum_rows) {
    ReportSection snapshots;
    snapshots.title = "Snapshots";
    snapshots.columns = {"ID", "Label", "Tracks", "Digest", "Revision"};
    for (const auto& snapshot : session.history().snapshots()) {
        append_row(snapshots,
                   {snapshot.id.to_string(),
                    snapshot.label,
                    std::to_string(snapshot.tracks.size()),
                    unsigned_integer(snapshot.digest),
                    std::to_string(snapshot.revision)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(snapshots));

    ReportSection journal;
    journal.title = "Journal";
    journal.columns = {
        "Sequence", "Transaction", "Opcode", "Bytes", "Digest", "Committed"
    };
    for (const auto& record : session.history().journal()) {
        append_row(journal,
                   {std::to_string(record.sequence),
                    std::to_string(record.transaction),
                    std::to_string(record.opcode),
                    std::to_string(record.payload.size()),
                    unsigned_integer(record.digest),
                    yes_no(record.committed)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(journal));

    ReportSection records;
    records.title = "Project records";
    records.columns = {"ID", "Key", "Value", "Revision", "Tombstone"};
    for (const auto& record : session.history().records()) {
        append_row(records,
                   {record.id.to_string(),
                    record.key,
                    record.value,
                    std::to_string(record.revision),
                    yes_no(record.tombstone)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(records));
}

void add_package(SessionReport& report,
                 const Session& session,
                 std::size_t maximum_rows) {
    ReportSection package;
    package.title = "Package sections";
    package.columns = {"Tag", "Flags", "Revision", "Bytes"};
    for (const auto& section : session.package().document().sections) {
        append_row(package,
                   {section_tag_string(section.tag),
                    std::to_string(section.flags),
                    std::to_string(section.revision),
                    std::to_string(section.payload.size())},
                   maximum_rows);
    }
    package.notes.push_back("Package ID: " +
                            unsigned_integer(session.package().document().package_id));
    package.notes.push_back("Content digest: " +
                            unsigned_integer(session.package().content_digest()));
    report.sections.push_back(std::move(package));
}

void add_diagnostics(SessionReport& report,
                     const Diagnostics& diagnostics,
                     std::size_t maximum_rows) {
    ReportSection section;
    section.title = "Diagnostics";
    section.columns = {
        "Severity", "Transaction", "Component", "Code", "Message"
    };
    for (const auto& item : diagnostics.entries()) {
        append_row(section,
                   {std::string(severity_name(item.severity)),
                    std::to_string(item.transaction),
                    item.component,
                    item.code,
                    item.message},
                   maximum_rows);
    }
    section.notes.push_back("Warnings: " +
                            std::to_string(diagnostics.count(Severity::warning)));
    section.notes.push_back("Errors: " +
                            std::to_string(diagnostics.count(Severity::error)));
    report.sections.push_back(std::move(section));
}

void add_manifest(SessionReport& report,
                  const Session& session,
                  std::size_t maximum_rows) {
    const auto manifest = build_manifest(session);
    ReportSection section;
    section.title = "Manifest";
    section.columns = {"Type", "ID", "Path", "Label", "Size", "Digest"};
    for (const auto& entry : manifest.entries) {
        append_row(section,
                   {entry.type,
                    entry.id.to_string(),
                    entry.path,
                    entry.label,
                    unsigned_integer(entry.size),
                    unsigned_integer(entry.digest)},
                   maximum_rows);
    }
    section.notes.push_back("Manifest digest: " +
                            unsigned_integer(manifest_digest(manifest)));
    for (const auto& conflict : manifest_conflicts(manifest)) {
        section.notes.push_back("Conflict: " + conflict);
    }
    report.sections.push_back(std::move(section));
}

void add_dependencies(SessionReport& report,
                      const Session& session,
                      std::size_t maximum_rows) {
    const auto graph = build_dependency_graph(session);
    ReportSection section;
    section.title = "Dependency graph";
    section.columns = {"ID", "Kind", "Label", "Dependencies", "Dependents"};
    for (const auto& node : graph) {
        std::ostringstream dependencies;
        for (std::size_t index = 0; index < node.dependencies.size(); ++index) {
            if (index != 0) {
                dependencies << ' ';
            }
            dependencies << node.dependencies[index].to_string();
        }
        std::ostringstream dependents;
        for (std::size_t index = 0; index < node.dependents.size(); ++index) {
            if (index != 0) {
                dependents << ' ';
            }
            dependents << node.dependents[index].to_string();
        }
        append_row(section,
                   {node.id.to_string(),
                    node.kind,
                    node.label,
                    dependencies.str(),
                    dependents.str()},
                   maximum_rows);
    }
    report.sections.push_back(std::move(section));
}

void add_occupancy(SessionReport& report,
                   const Session& session,
                   std::int64_t bucket_size,
                   std::size_t maximum_rows) {
    ReportSection section;
    section.title = "Timeline occupancy";
    section.columns = {
        "Start", "End", "Active clips", "Active tracks", "Estimated gain"
    };
    for (const auto& bucket : build_occupancy_map(session, bucket_size)) {
        append_row(section,
                   {integer(bucket.start_sample),
                    integer(bucket.end_sample),
                    std::to_string(bucket.active_clips),
                    std::to_string(bucket.active_tracks),
                    number(bucket.estimated_gain)},
                   maximum_rows);
    }
    report.sections.push_back(std::move(section));
}

std::size_t column_width(const ReportSection& section, std::size_t column) {
    std::size_t width = column < section.columns.size() ? section.columns[column].size() : 0;
    for (const auto& row : section.rows) {
        if (column < row.size()) {
            width = std::max(width, std::min<std::size_t>(60, row[column].size()));
        }
    }
    return width;
}

std::string clipped(std::string_view value, std::size_t width) {
    if (value.size() <= width) {
        return std::string(value);
    }
    if (width <= 3) {
        return std::string(value.substr(0, width));
    }
    return std::string(value.substr(0, width - 3)) + "...";
}

}  // namespace

SessionReport build_report(const Session& session,
                           const Diagnostics* diagnostics,
                           const ReportOptions& options) {
    SessionReport report;
    report.title = session.metadata().title.empty()
                       ? "WaveCairn session report"
                       : session.metadata().title + " — session report";
    report.generated_by = "WaveCairn";

    add_overview(report, session);
    if (options.include_media) {
        add_media(report, session, options.maximum_rows_per_section);
    }
    if (options.include_timeline) {
        add_tracks(report, session, options.maximum_rows_per_section);
        add_tempo_and_markers(report, session, options.maximum_rows_per_section);
        add_automation(report, session, options.maximum_rows_per_section);
    }
    if (options.include_routing) {
        add_routing(report, session, options.maximum_rows_per_section);
    }
    if (options.include_rendering) {
        add_rendering(report, session, options.maximum_rows_per_section);
    }
    if (options.include_history) {
        add_history(report, session, options.maximum_rows_per_section);
    }
    if (options.include_package) {
        add_package(report, session, options.maximum_rows_per_section);
    }
    if (options.include_diagnostics && diagnostics != nullptr) {
        add_diagnostics(report, *diagnostics, options.maximum_rows_per_section);
    }
    if (options.include_manifest) {
        add_manifest(report, session, options.maximum_rows_per_section);
    }
    if (options.include_dependency_graph) {
        add_dependencies(report, session, options.maximum_rows_per_section);
    }
    if (options.include_occupancy) {
        add_occupancy(report,
                      session,
                      std::max<std::int64_t>(1, options.occupancy_bucket_size),
                      options.maximum_rows_per_section);
    }

    const SessionValidator validator;
    Diagnostics validation_diagnostics;
    const auto validation = validator.validate(session, validation_diagnostics);
    if (!validation.usable || validation.errors != 0) {
        report.notices.push_back("Validation found " +
                                 std::to_string(validation.errors) +
                                 " errors and " +
                                 std::to_string(validation.warnings) +
                                 " warnings.");
    }
    return report;
}

std::string report_as_text(const SessionReport& report) {
    std::ostringstream out;
    out << report.title << '\n';
    out << std::string(report.title.size(), '=') << "\n\n";
    for (const auto& notice : report.notices) {
        out << "NOTICE: " << notice << '\n';
    }
    if (!report.notices.empty()) {
        out << '\n';
    }

    for (const auto& section : report.sections) {
        out << section.title << '\n';
        out << std::string(section.title.size(), '-') << '\n';
        if (!section.columns.empty()) {
            std::vector<std::size_t> widths;
            widths.reserve(section.columns.size());
            for (std::size_t column = 0; column < section.columns.size(); ++column) {
                widths.push_back(column_width(section, column));
            }
            for (std::size_t column = 0; column < section.columns.size(); ++column) {
                if (column != 0) {
                    out << " | ";
                }
                out << std::left << std::setw(static_cast<int>(widths[column]))
                    << clipped(section.columns[column], widths[column]);
            }
            out << '\n';
            for (std::size_t column = 0; column < widths.size(); ++column) {
                if (column != 0) {
                    out << "-+-";
                }
                out << std::string(widths[column], '-');
            }
            out << '\n';
            for (const auto& row : section.rows) {
                for (std::size_t column = 0; column < section.columns.size(); ++column) {
                    if (column != 0) {
                        out << " | ";
                    }
                    const auto value = column < row.size() ? row[column] : std::string{};
                    out << std::left << std::setw(static_cast<int>(widths[column]))
                        << clipped(value, widths[column]);
                }
                out << '\n';
            }
        }
        for (const auto& note : section.notes) {
            out << "* " << note << '\n';
        }
        out << '\n';
    }
    return out.str();
}

std::string report_as_markdown(const SessionReport& report) {
    std::ostringstream out;
    out << "# " << markdown_escape(report.title) << "\n\n";
    for (const auto& notice : report.notices) {
        out << "> " << markdown_escape(notice) << "\n\n";
    }
    for (const auto& section : report.sections) {
        out << "## " << markdown_escape(section.title) << "\n\n";
        if (!section.columns.empty()) {
            out << '|';
            for (const auto& column : section.columns) {
                out << ' ' << markdown_escape(column) << " |";
            }
            out << "\n|";
            for (std::size_t index = 0; index < section.columns.size(); ++index) {
                out << " --- |";
            }
            out << '\n';
            for (const auto& row : section.rows) {
                out << '|';
                for (std::size_t column = 0; column < section.columns.size(); ++column) {
                    const auto value = column < row.size() ? row[column] : std::string{};
                    out << ' ' << markdown_escape(value) << " |";
                }
                out << '\n';
            }
            out << '\n';
        }
        for (const auto& note : section.notes) {
            out << "- " << markdown_escape(note) << '\n';
        }
        if (!section.notes.empty()) {
            out << '\n';
        }
    }
    return out.str();
}

std::string report_as_json(const SessionReport& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"title\": \"" << json_escape_local(report.title) << "\",\n";
    out << "  \"generated_by\": \"" << json_escape_local(report.generated_by) << "\",\n";
    out << "  \"notices\": [";
    for (std::size_t index = 0; index < report.notices.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << "\"" << json_escape_local(report.notices[index]) << "\"";
    }
    out << "],\n";
    out << "  \"sections\": [\n";
    for (std::size_t section_index = 0;
         section_index < report.sections.size();
         ++section_index) {
        const auto& section = report.sections[section_index];
        out << "    {\n";
        out << "      \"title\": \"" << json_escape_local(section.title) << "\",\n";
        out << "      \"columns\": [";
        for (std::size_t index = 0; index < section.columns.size(); ++index) {
            if (index != 0) {
                out << ',';
            }
            out << "\"" << json_escape_local(section.columns[index]) << "\"";
        }
        out << "],\n";
        out << "      \"rows\": [\n";
        for (std::size_t row_index = 0; row_index < section.rows.size(); ++row_index) {
            out << "        [";
            for (std::size_t column = 0;
                 column < section.rows[row_index].size();
                 ++column) {
                if (column != 0) {
                    out << ',';
                }
                out << "\"" << json_escape_local(section.rows[row_index][column]) << "\"";
            }
            out << ']';
            if (row_index + 1 != section.rows.size()) {
                out << ',';
            }
            out << '\n';
        }
        out << "      ],\n";
        out << "      \"notes\": [";
        for (std::size_t index = 0; index < section.notes.size(); ++index) {
            if (index != 0) {
                out << ',';
            }
            out << "\"" << json_escape_local(section.notes[index]) << "\"";
        }
        out << "]\n";
        out << "    }";
        if (section_index + 1 != report.sections.size()) {
            out << ',';
        }
        out << '\n';
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

std::string report_as_csv(const SessionReport& report) {
    std::ostringstream out;
    for (const auto& section : report.sections) {
        out << "# " << section.title << '\n';
        for (std::size_t index = 0; index < section.columns.size(); ++index) {
            if (index != 0) {
                out << ',';
            }
            out << csv_escape_local(section.columns[index]);
        }
        out << '\n';
        for (const auto& row : section.rows) {
            for (std::size_t index = 0; index < section.columns.size(); ++index) {
                if (index != 0) {
                    out << ',';
                }
                out << csv_escape_local(index < row.size() ? row[index] : std::string{});
            }
            out << '\n';
        }
        for (const auto& note : section.notes) {
            out << "# note," << csv_escape_local(note) << '\n';
        }
        out << '\n';
    }
    return out.str();
}

std::string render_report(const SessionReport& report, ReportFormat format) {
    switch (format) {
        case ReportFormat::text:
            return report_as_text(report);
        case ReportFormat::markdown:
            return report_as_markdown(report);
        case ReportFormat::json:
            return report_as_json(report);
        case ReportFormat::csv:
            return report_as_csv(report);
    }
    return report_as_text(report);
}

}  // namespace wavecairn
