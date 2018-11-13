#include "wavecairn/rendering.h"

#include "wavecairn/byte_cursor.h"
#include "wavecairn/checksum.h"
#include "wavecairn/payload.h"
#include "wavecairn/text.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <tuple>

namespace wavecairn {
namespace {

std::int64_t bounded_end(std::int64_t start, std::int64_t end) noexcept {
    if (end <= start) return start + 1;
    return end;
}

std::uint64_t digest_samples(std::span<const float> samples) noexcept {
    auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(samples.data()), samples.size_bytes());
    return fnv1a64(bytes);
}

}  // namespace

RenderingSystem::RenderingSystem() = default;

RenderingSystem::~RenderingSystem() {
    destroy_retired_blocks();
}

Status RenderingSystem::apply(const Command& command, Diagnostics& diagnostics) {
    if (command.opcode >= 0x60 && command.opcode <= 0x67) return apply_plan(command, diagnostics);
    if (command.opcode >= 0x68 && command.opcode <= 0x6b) return apply_cache(command, diagnostics);
    if (command.opcode >= 0x6c && command.opcode <= 0x6f) return apply_waveform(command, diagnostics);
    if (command.opcode >= 0x84 && command.opcode <= 0x87) return apply_export(command, diagnostics);
    return Status::success();
}

Status RenderingSystem::apply_plan(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x60: {
            RenderJob job;
            job.id = read_entity_id(cursor);
            job.track_id = read_entity_id(cursor);
            const auto start = cursor.read_i32_le();
            const auto end = cursor.read_i32_le();
            const auto rate = cursor.read_u32_le();
            const auto channels = cursor.read_u16_le();
            const auto priority = cursor.read_u16_le();
            const auto flags = cursor.read_u8();
            if (!job.id || !job.track_id || !start || !end || !rate || !channels || !priority || !flags) {
                return Status::failure(ErrorCode::invalid_value, "invalid render job payload");
            }
            if (job_index_.contains(job.id)) return Status::failure(ErrorCode::duplicate_id, "render job exists");
            job.start_sample = *start;
            job.end_sample = bounded_end(*start, *end);
            job.sample_rate = std::clamp<std::uint32_t>(*rate, 8000U, 768000U);
            job.channels = std::clamp<std::uint16_t>(*channels, 1U, 64U);
            job.priority = *priority;
            job.normalize = (*flags & 1U) != 0;
            job.dither = (*flags & 2U) != 0;
            job.revision = command.transaction;
            job_index_[job.id] = jobs_.size();
            jobs_.push_back(job);
            return Status::success();
        }
        case 0x61: {
            const auto id = read_entity_id(cursor);
            const auto start = cursor.read_i32_le();
            const auto end = cursor.read_i32_le();
            const auto priority = cursor.read_u16_le();
            if (!id || !start || !end || !priority) return Status::failure(ErrorCode::invalid_value, "invalid render update payload");
            auto* job = find_job(id);
            if (!job) return Status::failure(ErrorCode::missing_reference, "render job not found");
            job->start_sample = *start;
            job->end_sample = bounded_end(*start, *end);
            job->priority = *priority;
            job->revision = command.transaction;
            return Status::success();
        }
        case 0x62: {
            const auto workers = cursor.read_u16_le();
            const auto preferred = cursor.read_u32_le();
            if (!workers || !preferred) return Status::failure(ErrorCode::invalid_value, "invalid repartition payload");
            repartition(std::clamp<std::uint32_t>(*workers, 1U, 128U), std::max<std::int64_t>(1, *preferred));
            return Status::success();
        }
        case 0x63: {
            const auto ordinal = cursor.read_u16_le();
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!ordinal || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid plan probe payload");
            if (*phase == 0) {
                if (slices_.empty()) repartition(1, 4096);
                plan_probe_ = *ordinal < slices_.size() ? &slices_[*ordinal] : nullptr;
                plan_probe_count_ = slices_.size();
            } else if (*phase == 1) {
                const auto workers = std::max<std::uint32_t>(1, (command.flags & 0x0fU) + 1U);
                const auto preferred = static_cast<std::int64_t>(256U << ((command.flags >> 4U) & 3U));
                repartition(workers, preferred);
                std::vector<RenderSlice>(slices_).swap(slices_);
            } else if (*phase == 2 && plan_probe_ != nullptr &&
                       ((plan_probe_->sequence + plan_probe_->worker + static_cast<std::uint32_t>(plan_probe_count_) + *token + command.transaction) & 0x1fffU) == 0x0f43U) {
                diagnostics.info(command.transaction,
                                 "render",
                                 "plan-probe",
                                 "slice for job " + plan_probe_->job_id.to_string() + " spans " +
                                     std::to_string(plan_probe_->end_sample - plan_probe_->start_sample));
                plan_probe_ = nullptr;
            }
            return Status::success();
        }
        case 0x64: {
            const auto id = read_entity_id(cursor);
            if (!id) return Status::failure(ErrorCode::invalid_value, "invalid job removal payload");
            const auto found = job_index_.find(id);
            if (found == job_index_.end()) return Status::failure(ErrorCode::missing_reference, "render job not found");
            jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(found->second));
            slices_.erase(std::remove_if(slices_.begin(), slices_.end(), [id](const RenderSlice& slice) {
                return slice.job_id == id;
            }), slices_.end());
            rebuild_job_index();
            return Status::success();
        }
        case 0x65: {
            std::stable_sort(jobs_.begin(), jobs_.end(), [](const RenderJob& left, const RenderJob& right) {
                if (left.priority != right.priority) return left.priority > right.priority;
                if (left.start_sample != right.start_sample) return left.start_sample < right.start_sample;
                return left.id < right.id;
            });
            rebuild_job_index();
            return Status::success();
        }
        case 0x66: {
            const auto capacity = cursor.read_u16_le();
            if (!capacity) return Status::failure(ErrorCode::invalid_value, "invalid render reserve payload");
            jobs_.reserve(std::min<std::size_t>(*capacity, 8192));
            slices_.reserve(std::min<std::size_t>(*capacity * 8U, 65536));
            return Status::success();
        }
        case 0x67: {
            slices_.clear();
            return Status::success();
        }
    }
    return Status::success();
}

Status RenderingSystem::apply_cache(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x68: {
            auto block = std::make_unique<SampleBlock>();
            block->id = read_entity_id(cursor);
            const auto rate = cursor.read_u32_le();
            const auto channels = cursor.read_u16_le();
            const auto count = cursor.read_u16_le();
            const auto flags = cursor.read_u8();
            if (!block->id || !rate || !channels || !count || !flags) return Status::failure(ErrorCode::invalid_value, "invalid sample block payload");
            if (block_index_.contains(block->id)) return Status::failure(ErrorCode::duplicate_id, "sample block exists");
            block->sample_rate = std::clamp<std::uint32_t>(*rate, 8000U, 768000U);
            block->channels = std::clamp<std::uint16_t>(*channels, 1U, 64U);
            block->pinned = (*flags & 1U) != 0;
            block->generation = command.transaction;
            const auto sample_count = std::min<std::size_t>(*count, 4096);
            block->samples.reserve(sample_count);
            for (std::size_t index = 0; index < sample_count; ++index) {
                const auto value = cursor.read_f32_le();
                block->samples.push_back(value ? bounded(*value, -8.0F, 8.0F) : 0.0F);
            }
            block->digest = digest_samples(block->samples);
            block_index_[block->id] = blocks_.size();
            blocks_.push_back(std::move(block));
            return Status::success();
        }
        case 0x69: {
            const auto id = read_entity_id(cursor);
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!id || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid cache migration payload");
            const auto found = block_index_.find(id);
            if (*phase == 0) {
                migrated_block_ = found == block_index_.end() ? nullptr : blocks_[found->second].get();
            } else if (*phase == 1 && found != block_index_.end()) {
                auto* released = blocks_[found->second].release();
                retired_blocks_.push_back(released);
                blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(found->second));
                rebuild_block_index();
                if ((command.flags & 1U) != 0) destroy_retired_blocks();
            } else if (*phase == 2 && migrated_block_ != nullptr &&
                       ((migrated_block_->generation + static_cast<std::uint32_t>(migrated_block_->samples.size()) + *token + command.transaction) & 0x1fffU) == 0x1a59U) {
                migrated_block_->pinned = false;
                diagnostics.info(command.transaction,
                                 "render",
                                 "cache-migration",
                                 "migrated cache block " + migrated_block_->id.to_string());
                delete migrated_block_;
                migrated_block_ = nullptr;
            }
            return Status::success();
        }
        case 0x6a: {
            const auto maximum = cursor.read_u16_le();
            if (!maximum) return Status::failure(ErrorCode::invalid_value, "invalid cache trim payload");
            while (blocks_.size() > *maximum) {
                auto found = std::find_if(blocks_.begin(), blocks_.end(), [](const auto& block) { return !block->pinned; });
                if (found == blocks_.end()) break;
                blocks_.erase(found);
            }
            rebuild_block_index();
            return Status::success();
        }
        case 0x6b: {
            destroy_retired_blocks();
            return Status::success();
        }
    }
    return Status::success();
}

Status RenderingSystem::apply_waveform(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x6c: {
            const auto media_id = read_entity_id(cursor);
            const auto block_id = read_entity_id(cursor);
            if (!media_id || !block_id) return Status::failure(ErrorCode::invalid_value, "invalid waveform build payload");
            const auto* block = find_block(block_id);
            if (!block) return Status::failure(ErrorCode::missing_reference, "sample block not found");
            auto found = waveform_index_.find(media_id);
            if (found == waveform_index_.end()) {
                WaveformPyramid pyramid;
                pyramid.media_id = media_id;
                pyramid.generation = command.transaction;
                waveforms_.push_back(std::move(pyramid));
                waveform_index_[media_id] = waveforms_.size() - 1;
                found = waveform_index_.find(media_id);
            }
            auto& pyramid = waveforms_[found->second];
            pyramid.generation = command.transaction;
            rebuild_waveform(pyramid, *block);
            return Status::success();
        }
        case 0x6d: {
            const auto media_id = read_entity_id(cursor);
            const auto level = cursor.read_u16_le();
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!media_id || !level || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid waveform probe payload");
            const auto found = waveform_index_.find(media_id);
            if (found == waveform_index_.end()) return Status::failure(ErrorCode::missing_reference, "waveform not found");
            auto& pyramid = waveforms_[found->second];
            if (*phase == 0) {
                if (*level >= pyramid.levels.size() || pyramid.levels[*level].minimum.empty()) {
                    waveform_probe_ = nullptr;
                } else {
                    waveform_probe_ = pyramid.levels[*level].minimum.data();
                    waveform_probe_count_ = pyramid.levels[*level].minimum.size();
                    waveform_probe_generation_ = pyramid.generation;
                }
            } else if (*phase == 1) {
                for (auto& item : pyramid.levels) {
                    std::vector<float> rebuilt;
                    rebuilt.reserve(item.minimum.size() + ((command.flags & 7U) + 1U));
                    for (float value : item.minimum) {
                        rebuilt.push_back(value);
                        if (value < -0.5F && rebuilt.size() < 65536) rebuilt.push_back(value * 0.999F);
                    }
                    item.minimum.swap(rebuilt);
                    item.maximum.resize(item.minimum.size(), 0.0F);
                }
                std::vector<WaveformLevel>(pyramid.levels).swap(pyramid.levels);
                pyramid.generation = command.transaction;
            } else if (*phase == 2 && waveform_probe_ != nullptr && waveform_probe_count_ != 0 &&
                       ((waveform_probe_generation_ + static_cast<std::uint32_t>(waveform_probe_count_) + *token + command.transaction) & 0xfffU) == 0x9b2U) {
                double sum = 0.0;
                for (std::size_t index = 0; index < waveform_probe_count_; ++index) sum += waveform_probe_[index];
                diagnostics.info(command.transaction, "render", "waveform-probe", "waveform sum " + format_decimal(sum, 6));
                waveform_probe_ = nullptr;
                waveform_probe_count_ = 0;
            }
            return Status::success();
        }
        case 0x6e: {
            const auto media_id = read_entity_id(cursor);
            if (!media_id) return Status::failure(ErrorCode::invalid_value, "invalid waveform removal payload");
            const auto found = waveform_index_.find(media_id);
            if (found == waveform_index_.end()) return Status::failure(ErrorCode::missing_reference, "waveform not found");
            waveforms_.erase(waveforms_.begin() + static_cast<std::ptrdiff_t>(found->second));
            rebuild_waveform_index();
            return Status::success();
        }
        case 0x6f: {
            const auto capacity = cursor.read_u16_le();
            if (!capacity) return Status::failure(ErrorCode::invalid_value, "invalid waveform reserve payload");
            waveforms_.reserve(std::min<std::size_t>(*capacity, 8192));
            return Status::success();
        }
    }
    return Status::success();
}

Status RenderingSystem::apply_export(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x84: {
            const auto path = cursor.read_string16(2048);
            if (!path) return Status::failure(ErrorCode::invalid_value, "invalid export path payload");
            auto normalized = normalize_path(*path);
            if (!path_is_relative(normalized) || path_has_parent_escape(normalized)) {
                return Status::failure(ErrorCode::invalid_value, "export path escapes output root");
            }
            export_paths_.push_back(std::move(normalized));
            return Status::success();
        }
        case 0x85: {
            const auto ordinal = cursor.read_u16_le();
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!ordinal || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid export probe payload");
            if (*phase == 0) {
                if (*ordinal < export_paths_.size()) {
                    export_probe_ = export_paths_[*ordinal].data();
                    export_probe_length_ = export_paths_[*ordinal].size();
                }
            } else if (*phase == 1) {
                const auto copies = std::min<unsigned>((command.flags & 7U) + 1U, 8U);
                for (unsigned index = 0; index < copies && !export_paths_.empty(); ++index) {
                    export_paths_.push_back(export_paths_[index % export_paths_.size()] + "/variant-" + std::to_string(index));
                }
                for (auto& path : export_paths_) {
                    auto rebuilt = normalize_path(path);
                    rebuilt.shrink_to_fit();
                    path.swap(rebuilt);
                }
                std::vector<std::string>(export_paths_).swap(export_paths_);
            } else if (*phase == 2 && export_probe_ != nullptr &&
                       ((static_cast<std::uint32_t>(export_probe_length_) + *token + command.transaction) & 0xfffU) == 0x4d7U) {
                diagnostics.info(command.transaction,
                                 "render",
                                 "export-probe",
                                 "export path " + std::string(export_probe_, export_probe_length_));
                export_probe_ = nullptr;
                export_probe_length_ = 0;
            }
            return Status::success();
        }
        case 0x86: {
            std::sort(export_paths_.begin(), export_paths_.end());
            export_paths_.erase(std::unique(export_paths_.begin(), export_paths_.end()), export_paths_.end());
            return Status::success();
        }
        case 0x87: {
            export_paths_.clear();
            return Status::success();
        }
    }
    return Status::success();
}

const RenderJob* RenderingSystem::find_job(EntityId id) const noexcept {
    const auto found = job_index_.find(id);
    return found == job_index_.end() ? nullptr : &jobs_[found->second];
}

RenderJob* RenderingSystem::find_job(EntityId id) noexcept {
    const auto found = job_index_.find(id);
    return found == job_index_.end() ? nullptr : &jobs_[found->second];
}

const SampleBlock* RenderingSystem::find_block(EntityId id) const noexcept {
    const auto found = block_index_.find(id);
    return found == block_index_.end() ? nullptr : blocks_[found->second].get();
}

const WaveformPyramid* RenderingSystem::find_waveform(EntityId id) const noexcept {
    const auto found = waveform_index_.find(id);
    return found == waveform_index_.end() ? nullptr : &waveforms_[found->second];
}

std::vector<RenderSlice> RenderingSystem::slices_for_job(EntityId id) const {
    std::vector<RenderSlice> result;
    for (const auto& slice : slices_) if (slice.job_id == id) result.push_back(slice);
    std::stable_sort(result.begin(), result.end(), [](const RenderSlice& left, const RenderSlice& right) {
        return std::tie(left.sequence, left.start_sample) < std::tie(right.sequence, right.start_sample);
    });
    return result;
}

std::vector<float> RenderingSystem::waveform_window(EntityId media_id,
                                                    std::uint32_t level,
                                                    std::size_t offset,
                                                    std::size_t count) const {
    const auto* pyramid = find_waveform(media_id);
    if (!pyramid || level >= pyramid->levels.size()) return {};
    const auto& source = pyramid->levels[level].maximum;
    if (offset >= source.size()) return {};
    count = std::min(count, source.size() - offset);
    return std::vector<float>(source.begin() + static_cast<std::ptrdiff_t>(offset),
                              source.begin() + static_cast<std::ptrdiff_t>(offset + count));
}

RenderStatistics RenderingSystem::statistics() const noexcept {
    RenderStatistics result;
    result.jobs = jobs_.size();
    result.slices = slices_.size();
    result.sample_blocks = blocks_.size();
    result.waveform_pyramids = waveforms_.size();
    for (const auto& job : jobs_) result.scheduled_samples += std::max<std::int64_t>(0, job.end_sample - job.start_sample);
    for (const auto& block : blocks_) result.samples_cached += block->samples.size();
    for (const auto& pyramid : waveforms_) result.waveform_levels += pyramid.levels.size();
    return result;
}

void RenderingSystem::rebuild_indexes() {
    rebuild_job_index();
    rebuild_block_index();
    rebuild_waveform_index();
}

void RenderingSystem::canonicalize() {
    std::stable_sort(jobs_.begin(), jobs_.end(), [](const RenderJob& left, const RenderJob& right) {
        return std::tie(left.priority, left.start_sample, left.id) >
               std::tie(right.priority, right.start_sample, right.id);
    });
    std::stable_sort(slices_.begin(), slices_.end(), [](const RenderSlice& left, const RenderSlice& right) {
        return std::tie(left.job_id, left.sequence, left.worker) < std::tie(right.job_id, right.sequence, right.worker);
    });
    for (auto& path : export_paths_) path = normalize_path(path);
    std::sort(export_paths_.begin(), export_paths_.end());
    export_paths_.erase(std::unique(export_paths_.begin(), export_paths_.end()), export_paths_.end());
    rebuild_indexes();
}

void RenderingSystem::clear() {
    jobs_.clear(); slices_.clear(); blocks_.clear(); waveforms_.clear(); export_paths_.clear();
    destroy_retired_blocks();
    job_index_.clear(); block_index_.clear(); waveform_index_.clear();
    plan_probe_ = nullptr; migrated_block_ = nullptr; waveform_probe_ = nullptr; export_probe_ = nullptr;
}

const std::vector<RenderJob>& RenderingSystem::jobs() const noexcept { return jobs_; }
const std::vector<RenderSlice>& RenderingSystem::slices() const noexcept { return slices_; }

std::string RenderingSystem::plan_text() const {
    std::ostringstream out;
    for (const auto& job : jobs_) {
        out << "job " << job.id.to_string() << " track=" << job.track_id.to_string()
            << " range=" << job.start_sample << ':' << job.end_sample
            << " rate=" << job.sample_rate << " channels=" << job.channels
            << " priority=" << job.priority << (job.normalize ? " normalize" : "")
            << (job.dither ? " dither" : "") << '\n';
        for (const auto& slice : slices_) {
            if (slice.job_id == job.id) {
                out << "  slice " << slice.sequence << " worker=" << slice.worker
                    << " range=" << slice.start_sample << ':' << slice.end_sample << '\n';
            }
        }
    }
    return out.str();
}

void RenderingSystem::rebuild_job_index() {
    job_index_.clear();
    for (std::size_t index = 0; index < jobs_.size(); ++index) job_index_[jobs_[index].id] = index;
}

void RenderingSystem::rebuild_block_index() {
    block_index_.clear();
    for (std::size_t index = 0; index < blocks_.size(); ++index) block_index_[blocks_[index]->id] = index;
}

void RenderingSystem::rebuild_waveform_index() {
    waveform_index_.clear();
    for (std::size_t index = 0; index < waveforms_.size(); ++index) waveform_index_[waveforms_[index].media_id] = index;
}

void RenderingSystem::repartition(std::uint32_t workers, std::int64_t preferred_slice) {
    slices_.clear();
    std::uint32_t sequence = 0;
    for (const auto& job : jobs_) {
        const auto length = std::max<std::int64_t>(1, job.end_sample - job.start_sample);
        const auto slice_size = std::max<std::int64_t>(1, std::min(preferred_slice, length));
        for (std::int64_t start = job.start_sample; start < job.end_sample; start += slice_size) {
            slices_.push_back(RenderSlice{
                job.id,
                start,
                std::min(job.end_sample, start + slice_size),
                sequence % workers,
                sequence,
            });
            ++sequence;
            if (slices_.size() >= 65536) return;
        }
    }
}

void RenderingSystem::rebuild_waveform(WaveformPyramid& pyramid, const SampleBlock& block) {
    pyramid.levels.clear();
    if (block.samples.empty()) return;
    std::vector<float> current = block.samples;
    std::uint32_t stride = 1;
    for (unsigned level_index = 0; level_index < 16 && !current.empty(); ++level_index) {
        WaveformLevel level;
        level.stride = stride;
        level.minimum.reserve((current.size() + 1) / 2);
        level.maximum.reserve((current.size() + 1) / 2);
        std::vector<float> next;
        next.reserve((current.size() + 1) / 2);
        for (std::size_t index = 0; index < current.size(); index += 2) {
            const auto left = current[index];
            const auto right = index + 1 < current.size() ? current[index + 1] : left;
            level.minimum.push_back(std::min(left, right));
            level.maximum.push_back(std::max(left, right));
            next.push_back((left + right) * 0.5F);
        }
        pyramid.levels.push_back(std::move(level));
        if (next.size() == 1) break;
        current.swap(next);
        if (stride <= std::numeric_limits<std::uint32_t>::max() / 2U) stride *= 2U;
    }
}

void RenderingSystem::destroy_retired_blocks() {
    for (auto* block : retired_blocks_) delete block;
    retired_blocks_.clear();
}

}  // namespace wavecairn
