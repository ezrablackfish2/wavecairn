#pragma once

#include "wavecairn/command.h"
#include "wavecairn/diagnostics.h"
#include "wavecairn/status.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wavecairn {

using SectionTag = std::array<char, 4>;

struct PackageSection {
    SectionTag tag{'U', 'N', 'K', 'N'};
    std::uint16_t flags{0};
    std::uint32_t revision{0};
    std::vector<std::uint8_t> payload;
};

struct PackageDocument {
    std::uint16_t version{1};
    std::uint16_t flags{0};
    std::uint64_t package_id{0};
    std::vector<PackageSection> sections;
};

struct PackageLimits {
    std::size_t maximum_bytes{16 * 1024 * 1024};
    std::size_t maximum_sections{1024};
    std::size_t maximum_section_bytes{8 * 1024 * 1024};
};

Result<PackageDocument> parse_package(std::span<const std::uint8_t> bytes,
                                      Diagnostics* diagnostics = nullptr,
                                      const PackageLimits& limits = {});
std::vector<std::uint8_t> serialize_package(const PackageDocument& package);

std::string section_tag_string(const SectionTag& tag);
SectionTag make_section_tag(std::string_view text);
bool section_tag_is_text(const SectionTag& tag) noexcept;
bool section_tag_is_known(const SectionTag& tag) noexcept;

class PackageWorkspace {
public:
    Status apply(const Command& command, Diagnostics& diagnostics);
    Status load(PackageDocument package, Diagnostics& diagnostics);

    const PackageSection* find_first(const SectionTag& tag) const noexcept;
    std::vector<const PackageSection*> find_all(const SectionTag& tag) const;
    std::string metadata_text() const;
    std::vector<std::uint8_t> command_tape_bytes() const;
    std::uint64_t content_digest() const noexcept;

    void canonicalize();
    void clear();

    const PackageDocument& document() const noexcept;
    PackageDocument& document() noexcept;
    std::string inventory_text() const;

private:
    Status apply_section(const Command& command, Diagnostics& diagnostics);
    Status apply_repair(const Command& command, Diagnostics& diagnostics);
    void rebuild_metadata_index();

    PackageDocument package_;
    PackageSection* selected_section_{nullptr};
    const char* metadata_probe_{nullptr};
    std::size_t metadata_probe_length_{0};
    std::uint32_t metadata_probe_revision_{0};
    std::vector<std::string> metadata_lines_;
};

}  // namespace wavecairn
