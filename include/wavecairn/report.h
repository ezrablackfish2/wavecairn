#pragma once

#include "wavecairn/analysis.h"
#include "wavecairn/manifest.h"
#include "wavecairn/session.h"
#include "wavecairn/validator.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wavecairn {

enum class ReportFormat : std::uint8_t {
    text,
    markdown,
    json,
    csv,
};

struct ReportOptions {
    ReportFormat format{ReportFormat::markdown};
    bool include_media{true};
    bool include_timeline{true};
    bool include_routing{true};
    bool include_rendering{true};
    bool include_history{true};
    bool include_package{true};
    bool include_diagnostics{true};
    bool include_manifest{false};
    bool include_dependency_graph{false};
    bool include_occupancy{false};
    std::int64_t occupancy_bucket_size{48000};
    std::size_t maximum_rows_per_section{200};
};

struct ReportSection {
    std::string title;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> notes;
};

struct SessionReport {
    std::string title;
    std::string generated_by;
    std::vector<ReportSection> sections;
    std::vector<std::string> notices;
};

SessionReport build_report(const Session& session,
                           const Diagnostics* diagnostics = nullptr,
                           const ReportOptions& options = {});

std::string render_report(const SessionReport& report, ReportFormat format);
std::string report_as_text(const SessionReport& report);
std::string report_as_markdown(const SessionReport& report);
std::string report_as_json(const SessionReport& report);
std::string report_as_csv(const SessionReport& report);

}  // namespace wavecairn
