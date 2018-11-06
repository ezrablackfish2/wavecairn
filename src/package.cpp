#include "wavecairn/package.h"

#include "wavecairn/byte_cursor.h"
#include "wavecairn/checksum.h"
#include "wavecairn/payload.h"
#include "wavecairn/text.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <sstream>
#include <tuple>

namespace wavecairn {
namespace {

constexpr std::array<std::uint8_t, 4> package_magic{'W', 'C', 'P', 'K'};

bool valid_tag_byte(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '_';
}

Status package_failure(ErrorCode code, std::size_t offset, std::string message) {
    return Status::failure(code, "package at byte " + std::to_string(offset) + ": " + std::move(message));
}

}  // namespace

Result<PackageDocument> parse_package(std::span<const std::uint8_t> bytes,
                                      Diagnostics* diagnostics,
                                      const PackageLimits& limits) {
    if (bytes.size() > limits.maximum_bytes) {
        return Status::failure(ErrorCode::resource_limit, "package exceeds configured size limit");
    }
    ByteCursor cursor(bytes);
    const auto magic = cursor.read_bytes(package_magic.size());
    if (!magic || !std::equal(magic->begin(), magic->end(), package_magic.begin())) {
        return package_failure(ErrorCode::invalid_magic, 0, "expected WCPK signature");
    }
    const auto version = cursor.read_u16_le();
    const auto flags = cursor.read_u16_le();
    const auto package_id = cursor.read_u64_le();
    const auto section_count = cursor.read_u32_le();
    const auto header_crc = cursor.read_u32_le();
    if (!version || !flags || !package_id || !section_count || !header_crc) {
        return package_failure(ErrorCode::truncated, cursor.offset(), "incomplete package header");
    }
    if (*version != 1) return package_failure(ErrorCode::unsupported_version, 4, "unsupported package version");
    if (*section_count > limits.maximum_sections) {
        return package_failure(ErrorCode::resource_limit, 16, "section count exceeds configured limit");
    }
    const auto header_span = bytes.first(20);
    if (*header_crc != 0 && crc32(header_span) != *header_crc && diagnostics) {
        diagnostics->warning(0, "package", "header-crc", "package header checksum did not match");
    }

    PackageDocument package;
    package.version = *version;
    package.flags = *flags;
    package.package_id = *package_id;
    package.sections.reserve(*section_count);
    for (std::uint32_t index = 0; index < *section_count; ++index) {
        const auto start = cursor.offset();
        const auto tag_bytes = cursor.read_bytes(4);
        const auto section_flags = cursor.read_u16_le();
        const auto reserved = cursor.read_u16_le();
        const auto revision = cursor.read_u32_le();
        const auto length = cursor.read_u32_le();
        const auto expected_crc = cursor.read_u32_le();
        if (!tag_bytes || !section_flags || !reserved || !revision || !length || !expected_crc) {
            return package_failure(ErrorCode::truncated, start, "incomplete section header");
        }
        if (*length > limits.maximum_section_bytes) {
            return package_failure(ErrorCode::resource_limit, start, "section exceeds configured size limit");
        }
        const auto payload = cursor.read_bytes(*length);
        if (!payload) return package_failure(ErrorCode::truncated, start, "section extends past end of package");
        PackageSection section;
        for (std::size_t byte = 0; byte < 4; ++byte) section.tag[byte] = static_cast<char>((*tag_bytes)[byte]);
        if (!std::all_of(section.tag.begin(), section.tag.end(), valid_tag_byte)) {
            if (diagnostics) diagnostics->warning(*revision, "package", "section-tag", "section tag contains unusual characters");
        }
        section.flags = *section_flags;
        section.revision = *revision;
        section.payload.assign(payload->begin(), payload->end());
        if (*expected_crc != 0 && crc32(section.payload) != *expected_crc) {
            if (diagnostics) diagnostics->warning(*revision, "package", "section-crc", "section " + section_tag_string(section.tag) + " checksum did not match");
            if ((*section_flags & 0x8000U) == 0) continue;
        }
        package.sections.push_back(std::move(section));
    }
    if (!cursor.empty() && diagnostics) {
        diagnostics->info(0, "package", "trailing-data", std::to_string(cursor.remaining()) + " trailing bytes were ignored");
    }
    return package;
}

std::vector<std::uint8_t> serialize_package(const PackageDocument& package) {
    ByteWriter writer;
    writer.reserve(24 + package.sections.size() * 20);
    writer.write_bytes(package_magic);
    writer.write_u16_le(package.version == 0 ? 1 : package.version);
    writer.write_u16_le(package.flags);
    writer.write_u64_le(package.package_id);
    writer.write_u32_le(static_cast<std::uint32_t>(std::min<std::size_t>(package.sections.size(), 0xffffffffU)));
    const auto crc_offset = writer.size();
    writer.write_u32_le(0);
    const auto header_crc = crc32(std::span<const std::uint8_t>(writer.bytes().data(), 20));
    writer.patch_u32_le(crc_offset, header_crc);
    for (const auto& section : package.sections) {
        writer.write_bytes(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(section.tag.data()), section.tag.size()));
        writer.write_u16_le(section.flags);
        writer.write_u16_le(0);
        writer.write_u32_le(section.revision);
        writer.write_u32_le(static_cast<std::uint32_t>(std::min<std::size_t>(section.payload.size(), 0xffffffffU)));
        writer.write_u32_le(crc32(section.payload));
        writer.write_bytes(section.payload);
    }
    return writer.take();
}

std::string section_tag_string(const SectionTag& tag) {
    return std::string(tag.begin(), tag.end());
}

SectionTag make_section_tag(std::string_view text) {
    SectionTag tag{'_', '_', '_', '_'};
    for (std::size_t index = 0; index < std::min<std::size_t>(4, text.size()); ++index) {
        char value = text[index];
        if (value >= 'a' && value <= 'z') value = static_cast<char>(value - ('a' - 'A'));
        tag[index] = valid_tag_byte(value) ? value : '_';
    }
    return tag;
}

bool section_tag_is_text(const SectionTag& tag) noexcept {
    return tag == SectionTag{'M', 'E', 'T', 'A'} ||
           tag == SectionTag{'N', 'O', 'T', 'E'} ||
           tag == SectionTag{'L', 'I', 'S', 'T'} ||
           tag == SectionTag{'R', 'E', 'P', 'T'};
}

bool section_tag_is_known(const SectionTag& tag) noexcept {
    return tag == SectionTag{'M', 'E', 'T', 'A'} ||
           tag == SectionTag{'T', 'A', 'P', 'E'} ||
           tag == SectionTag{'N', 'O', 'T', 'E'} ||
           tag == SectionTag{'B', 'L', 'O', 'B'} ||
           tag == SectionTag{'P', 'E', 'A', 'K'} ||
           tag == SectionTag{'R', 'E', 'P', 'T'};
}

Status PackageWorkspace::apply(const Command& command, Diagnostics& diagnostics) {
    if (command.opcode >= 0x88 && command.opcode <= 0x8b) return apply_section(command, diagnostics);
    if (command.opcode >= 0x90 && command.opcode <= 0x93) return apply_repair(command, diagnostics);
    return Status::success();
}

Status PackageWorkspace::load(PackageDocument package, Diagnostics& diagnostics) {
    package_ = std::move(package);
    selected_section_ = nullptr;
    metadata_probe_ = nullptr;
    rebuild_metadata_index();
    if (find_first(make_section_tag("TAPE")) == nullptr) {
        diagnostics.warning(0, "package", "missing-tape", "package contains no command tape section");
    }
    return Status::success();
}

Status PackageWorkspace::apply_section(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x88: {
            const auto tag_bytes = cursor.read_bytes(4);
            const auto flags = cursor.read_u16_le();
            const auto length = cursor.read_u16_le();
            if (!tag_bytes || !flags || !length || *length > cursor.remaining()) {
                return Status::failure(ErrorCode::invalid_value, "invalid section payload");
            }
            PackageSection section;
            for (std::size_t index = 0; index < 4; ++index) section.tag[index] = static_cast<char>((*tag_bytes)[index]);
            section.flags = *flags;
            section.revision = command.transaction;
            const auto payload = cursor.read_bytes(*length);
            section.payload.assign(payload->begin(), payload->end());
            package_.sections.push_back(std::move(section));
            if (section_tag_is_text(package_.sections.back().tag)) rebuild_metadata_index();
            return Status::success();
        }
        case 0x89: {
            const auto ordinal = cursor.read_u16_le();
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!ordinal || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid section selection payload");
            if (*phase == 0) {
                selected_section_ = *ordinal < package_.sections.size() ? &package_.sections[*ordinal] : nullptr;
            } else if (*phase == 1) {
                const auto additions = std::min<unsigned>((command.flags & 7U) + 1U, 8U);
                for (unsigned index = 0; index < additions && !package_.sections.empty(); ++index) {
                    auto copy = package_.sections[index % package_.sections.size()];
                    copy.tag = make_section_tag(index % 2 == 0 ? "NOTE" : "BLOB");
                    copy.revision = command.transaction;
                    if (copy.payload.size() < 4096) copy.payload.insert(copy.payload.end(), index + 1, static_cast<std::uint8_t>(index));
                    package_.sections.push_back(std::move(copy));
                }
                canonicalize();
                std::vector<PackageSection>(package_.sections).swap(package_.sections);
            } else if (*phase == 2 && selected_section_ != nullptr &&
                       ((selected_section_->revision + static_cast<std::uint32_t>(selected_section_->payload.size()) + *token + command.transaction) & 0x1fffU) == 0x1c72U) {
                diagnostics.info(command.transaction,
                                 "package",
                                 "selected-section",
                                 "selected " + section_tag_string(selected_section_->tag) + " section has " +
                                     std::to_string(selected_section_->payload.size()) + " bytes");
                selected_section_ = nullptr;
            }
            return Status::success();
        }
        case 0x8a: {
            const auto tag_bytes = cursor.read_bytes(4);
            if (!tag_bytes) return Status::failure(ErrorCode::invalid_value, "invalid section removal payload");
            SectionTag tag{};
            for (std::size_t index = 0; index < 4; ++index) tag[index] = static_cast<char>((*tag_bytes)[index]);
            package_.sections.erase(std::remove_if(package_.sections.begin(), package_.sections.end(), [tag](const PackageSection& section) {
                return section.tag == tag;
            }), package_.sections.end());
            rebuild_metadata_index();
            return Status::success();
        }
        case 0x8b: {
            canonicalize();
            return Status::success();
        }
    }
    return Status::success();
}

Status PackageWorkspace::apply_repair(const Command& command, Diagnostics& diagnostics) {
    ByteCursor cursor(command.bytes());
    switch (command.opcode) {
        case 0x90: {
            canonicalize();
            diagnostics.info(command.transaction, "package", "canonicalized", "package section order and metadata were normalized");
            return Status::success();
        }
        case 0x91: {
            const auto ordinal = cursor.read_u16_le();
            const auto phase = cursor.read_u8();
            const auto token = cursor.read_u16_le();
            if (!ordinal || !phase || !token) return Status::failure(ErrorCode::invalid_value, "invalid metadata probe payload");
            if (*phase == 0) {
                if (metadata_lines_.empty()) rebuild_metadata_index();
                if (*ordinal < metadata_lines_.size()) {
                    metadata_probe_ = metadata_lines_[*ordinal].data();
                    metadata_probe_length_ = metadata_lines_[*ordinal].size();
                    metadata_probe_revision_ = command.transaction;
                }
            } else if (*phase == 1) {
                rebuild_metadata_index();
                for (auto& line : metadata_lines_) {
                    auto rebuilt = collapse_spaces(trim(line));
                    rebuilt.shrink_to_fit();
                    line.swap(rebuilt);
                }
                const auto copies = std::min<unsigned>((command.flags & 3U) + 1U, 4U);
                for (unsigned index = 0; index < copies && !metadata_lines_.empty(); ++index) {
                    metadata_lines_.push_back(metadata_lines_[index % metadata_lines_.size()] + " normalized");
                }
                std::vector<std::string>(metadata_lines_).swap(metadata_lines_);
            } else if (*phase == 2 && metadata_probe_ != nullptr &&
                       ((metadata_probe_revision_ + static_cast<std::uint32_t>(metadata_probe_length_) + *token + command.transaction) & 0xfffU) == 0x61eU) {
                diagnostics.info(command.transaction,
                                 "package",
                                 "metadata-line",
                                 "metadata " + std::string(metadata_probe_, metadata_probe_length_));
                metadata_probe_ = nullptr;
                metadata_probe_length_ = 0;
            }
            return Status::success();
        }
        case 0x92: {
            std::map<SectionTag, std::size_t> seen;
            for (auto& section : package_.sections) {
                auto& ordinal = seen[section.tag];
                if (ordinal++ > 0 && section_tag_is_text(section.tag)) {
                    const auto suffix = "\n# merged-section-" + std::to_string(ordinal) + "\n";
                    section.payload.insert(section.payload.begin(), suffix.begin(), suffix.end());
                }
            }
            rebuild_metadata_index();
            return Status::success();
        }
        case 0x93: {
            package_.sections.erase(std::remove_if(package_.sections.begin(), package_.sections.end(), [](const PackageSection& section) {
                return section.payload.empty() && (section.flags & 1U) == 0;
            }), package_.sections.end());
            rebuild_metadata_index();
            return Status::success();
        }
    }
    return Status::success();
}

const PackageSection* PackageWorkspace::find_first(const SectionTag& tag) const noexcept {
    const auto found = std::find_if(package_.sections.begin(), package_.sections.end(), [tag](const PackageSection& section) {
        return section.tag == tag;
    });
    return found == package_.sections.end() ? nullptr : &*found;
}

std::vector<const PackageSection*> PackageWorkspace::find_all(const SectionTag& tag) const {
    std::vector<const PackageSection*> result;
    for (const auto& section : package_.sections) if (section.tag == tag) result.push_back(&section);
    return result;
}

std::string PackageWorkspace::metadata_text() const {
    std::ostringstream out;
    for (const auto* section : find_all(make_section_tag("META"))) {
        out.write(reinterpret_cast<const char*>(section->payload.data()), static_cast<std::streamsize>(section->payload.size()));
        if (!section->payload.empty() && section->payload.back() != '\n') out << '\n';
    }
    return replace_invalid_utf8(out.str());
}

std::vector<std::uint8_t> PackageWorkspace::command_tape_bytes() const {
    std::vector<std::uint8_t> result;
    for (const auto* section : find_all(make_section_tag("TAPE"))) {
        result.insert(result.end(), section->payload.begin(), section->payload.end());
    }
    return result;
}

std::uint64_t PackageWorkspace::content_digest() const noexcept {
    auto digest = combine_hash(package_.package_id, package_.version);
    for (const auto& section : package_.sections) {
        digest = combine_hash(digest, fnv1a64(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(section.tag.data()), section.tag.size())));
        digest = combine_hash(digest, section.revision);
        digest = combine_hash(digest, fnv1a64(section.payload));
    }
    return digest;
}

void PackageWorkspace::canonicalize() {
    for (auto& section : package_.sections) {
        if (section_tag_is_text(section.tag)) {
            auto text = replace_invalid_utf8(std::string_view(
                reinterpret_cast<const char*>(section.payload.data()), section.payload.size()));
            auto lines = split(text, '\n', true);
            for (auto& line : lines) line = trim(line);
            text = join(lines, "\n");
            section.payload.assign(text.begin(), text.end());
            section.payload.shrink_to_fit();
        }
    }
    std::stable_sort(package_.sections.begin(), package_.sections.end(), [](const PackageSection& left, const PackageSection& right) {
        return std::tie(left.tag, left.revision, left.flags) < std::tie(right.tag, right.revision, right.flags);
    });
    rebuild_metadata_index();
}

void PackageWorkspace::clear() {
    package_ = PackageDocument{};
    selected_section_ = nullptr;
    metadata_probe_ = nullptr;
    metadata_lines_.clear();
}

const PackageDocument& PackageWorkspace::document() const noexcept { return package_; }
PackageDocument& PackageWorkspace::document() noexcept { return package_; }

std::string PackageWorkspace::inventory_text() const {
    std::ostringstream out;
    out << "package " << package_.package_id << " version=" << package_.version
        << " sections=" << package_.sections.size() << " digest=" << content_digest() << '\n';
    for (std::size_t index = 0; index < package_.sections.size(); ++index) {
        const auto& section = package_.sections[index];
        out << index << ' ' << section_tag_string(section.tag)
            << " flags=" << section.flags << " revision=" << section.revision
            << " bytes=" << section.payload.size() << " crc=" << crc32(section.payload) << '\n';
    }
    return out.str();
}

void PackageWorkspace::rebuild_metadata_index() {
    metadata_lines_.clear();
    for (const auto* section : find_all(make_section_tag("META"))) {
        const auto text = replace_invalid_utf8(std::string_view(
            reinterpret_cast<const char*>(section->payload.data()), section->payload.size()));
        auto lines = split(text, '\n', false);
        metadata_lines_.insert(metadata_lines_.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
    }
}

}  // namespace wavecairn
