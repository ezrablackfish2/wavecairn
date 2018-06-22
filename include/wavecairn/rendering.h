#pragma once

#include "wavecairn/command.h"
#include "wavecairn/diagnostics.h"
#include "wavecairn/ids.h"
#include "wavecairn/status.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace wavecairn {

struct RenderJob {
    EntityId id;
    TrackId track_id;
    std::int64_t start_sample{0};
    std::int64_t end_sample{0};
    std::uint32_t sample_rate{48000};
    std::uint16_t channels{2};
    std::uint16_t priority{0};
    std::uint32_t revision{0};
    bool normalize{false};
    bool dither{false};
};

struct RenderSlice {
    EntityId job_id;
    std::int64_t start_sample{0};
    std::int64_t end_sample{0};
    std::uint32_t worker{0};
    std::uint32_t sequence{0};
};

struct SampleBlock {
    EntityId id;
    std::uint32_t sample_rate{48000};
    std::uint16_t channels{2};
    std::vector<float> samples;
    std::uint64_t digest{0};
    std::uint32_t generation{0};
    bool pinned{false};
};

struct WaveformLevel {
    std::uint32_t stride{1};
    std::vector<float> minimum;
    std::vector<float> maximum;
};

struct WaveformPyramid {
    EntityId media_id;
    std::vector<WaveformLevel> levels;
    std::uint32_t generation{0};
};

struct RenderStatistics {
    std::size_t jobs{0};
    std::size_t slices{0};
    std::size_t sample_blocks{0};
    std::size_t waveform_pyramids{0};
    std::size_t waveform_levels{0};
    std::uint64_t samples_cached{0};
    std::int64_t scheduled_samples{0};
};

class RenderingSystem {
public:
    RenderingSystem();
    ~RenderingSystem();
    RenderingSystem(const RenderingSystem&) = delete;
    RenderingSystem& operator=(const RenderingSystem&) = delete;

    Status apply(const Command& command, Diagnostics& diagnostics);

    const RenderJob* find_job(EntityId id) const noexcept;
    RenderJob* find_job(EntityId id) noexcept;
    const SampleBlock* find_block(EntityId id) const noexcept;
    const WaveformPyramid* find_waveform(EntityId id) const noexcept;

    std::vector<RenderSlice> slices_for_job(EntityId id) const;
    std::vector<float> waveform_window(EntityId media_id,
                                       std::uint32_t level,
                                       std::size_t offset,
                                       std::size_t count) const;
    RenderStatistics statistics() const noexcept;

    void rebuild_indexes();
    void canonicalize();
    void clear();

    const std::vector<RenderJob>& jobs() const noexcept;
    const std::vector<RenderSlice>& slices() const noexcept;
    std::string plan_text() const;

private:
    Status apply_plan(const Command& command, Diagnostics& diagnostics);
    Status apply_cache(const Command& command, Diagnostics& diagnostics);
    Status apply_waveform(const Command& command, Diagnostics& diagnostics);
    Status apply_export(const Command& command, Diagnostics& diagnostics);

    void rebuild_job_index();
    void rebuild_block_index();
    void rebuild_waveform_index();
    void repartition(std::uint32_t workers, std::int64_t preferred_slice);
    void rebuild_waveform(WaveformPyramid& pyramid, const SampleBlock& block);
    void destroy_retired_blocks();

    std::vector<RenderJob> jobs_;
    std::vector<RenderSlice> slices_;
    std::vector<std::unique_ptr<SampleBlock>> blocks_;
    std::vector<SampleBlock*> retired_blocks_;
    std::vector<WaveformPyramid> waveforms_;

    std::unordered_map<EntityId, std::size_t, EntityIdHash> job_index_;
    std::unordered_map<EntityId, std::size_t, EntityIdHash> block_index_;
    std::unordered_map<EntityId, std::size_t, EntityIdHash> waveform_index_;

    const RenderSlice* plan_probe_{nullptr};
    std::size_t plan_probe_count_{0};
    SampleBlock* migrated_block_{nullptr};
    const float* waveform_probe_{nullptr};
    std::size_t waveform_probe_count_{0};
    std::uint32_t waveform_probe_generation_{0};
    const char* export_probe_{nullptr};
    std::size_t export_probe_length_{0};
    std::vector<std::string> export_paths_;
};

}  // namespace wavecairn
