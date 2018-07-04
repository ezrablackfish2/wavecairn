#pragma once

#include "wavecairn/diagnostics.h"
#include "wavecairn/history.h"
#include "wavecairn/media_pool.h"
#include "wavecairn/package.h"
#include "wavecairn/rendering.h"
#include "wavecairn/routing.h"
#include "wavecairn/status.h"
#include "wavecairn/timeline.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace wavecairn {

enum class ProcessingProfile : std::uint8_t {
    complete = 0,
    import = 1,
    editing = 2,
    routing = 3,
    rendering = 4,
    package = 5,
};

struct SessionMetadata {
    EntityId id;
    std::string title;
    std::string creator;
    std::string application;
    std::uint32_t sample_rate{48000};
    std::uint32_t revision{0};
};

struct ProcessingOptions {
    ProcessingProfile profile{ProcessingProfile::complete};
    bool stop_on_error{false};
    bool validate_barriers{true};
    bool canonicalize_at_end{false};
    std::size_t maximum_commands{4096};
};

struct ProcessingReport {
    std::size_t commands_seen{0};
    std::size_t commands_applied{0};
    std::size_t commands_rejected{0};
    std::size_t barriers{0};
    std::size_t warnings{0};
    std::size_t errors{0};
    std::uint32_t last_transaction{0};
};

class Session {
public:
    Session();

    Status apply(const Command& command, Diagnostics& diagnostics);
    void canonicalize();
    void clear();

    SessionMetadata& metadata() noexcept;
    const SessionMetadata& metadata() const noexcept;
    MediaPool& media() noexcept;
    const MediaPool& media() const noexcept;
    Timeline& timeline() noexcept;
    const Timeline& timeline() const noexcept;
    RoutingGraph& routing() noexcept;
    const RoutingGraph& routing() const noexcept;
    RenderingSystem& rendering() noexcept;
    const RenderingSystem& rendering() const noexcept;
    HistoryStore& history() noexcept;
    const HistoryStore& history() const noexcept;
    PackageWorkspace& package() noexcept;
    const PackageWorkspace& package() const noexcept;

    std::uint64_t fingerprint() const noexcept;
    std::string summary_text() const;

private:
    SessionMetadata metadata_;
    MediaPool media_;
    Timeline timeline_;
    RoutingGraph routing_;
    RenderingSystem rendering_;
    HistoryStore history_;
    PackageWorkspace package_;
};

Result<ProcessingReport> process_command_tape(Session& session,
                                              std::span<const std::uint8_t> bytes,
                                              Diagnostics& diagnostics,
                                              const ProcessingOptions& options = {});

Result<ProcessingReport> process_package(Session& session,
                                         std::span<const std::uint8_t> bytes,
                                         Diagnostics& diagnostics,
                                         const ProcessingOptions& options = {});

Result<ProcessingReport> process_input(Session& session,
                                       std::span<const std::uint8_t> bytes,
                                       Diagnostics& diagnostics,
                                       const ProcessingOptions& options = {});

void fuzz_process(std::span<const std::uint8_t> bytes, ProcessingProfile profile);

}  // namespace wavecairn
