#include "wavecairn/manifest.h"

#include "wavecairn/checksum.h"
#include "wavecairn/text.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace wavecairn {
namespace {

std::string escape_field(std::string_view text) {
    return percent_encode(text);
}

std::string unescape_field(std::string_view text) {
    return percent_decode(text);
}

std::string attributes_text(const std::map<std::string, std::string>& attributes) {
    std::vector<std::string> parts;
    parts.reserve(attributes.size());
    for (const auto& [key, value] : attributes) {
        parts.push_back(escape_field(key) + "=" + escape_field(value));
    }
    return join(parts, ";");
}

std::map<std::string, std::string> parse_attributes(std::string_view text) {
    std::map<std::string, std::string> result;
    for (const auto& part : split(text, ';', false)) {
        const auto equal = part.find('=');
        if (equal == std::string::npos) continue;
        result[unescape_field(std::string_view(part).substr(0, equal))] =
            unescape_field(std::string_view(part).substr(equal + 1));
    }
    return result;
}

std::optional<std::uint64_t> parse_unsigned(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::uint64_t media_size(const MediaAsset& asset) noexcept {
    const auto bytes_per_sample = std::max<std::uint16_t>(1, asset.format.bits_per_sample / 8);
    return asset.frame_count * asset.format.channels * bytes_per_sample;
}

}  // namespace

SessionManifest build_manifest(const Session& session) {
    SessionManifest manifest;
    manifest.session_fingerprint = session.fingerprint();
    manifest.entries.reserve(session.media().assets().size() +
                             session.timeline().tracks().size() +
                             session.timeline().clips().size() +
                             session.routing().plugins().size() +
                             session.rendering().jobs().size() +
                             session.package().document().sections.size());

    for (const auto& asset : session.media().assets()) {
        ManifestEntry entry;
        entry.type = "media";
        entry.id = asset.id;
        entry.path = asset.relative_path;
        entry.label = asset.logical_name;
        entry.size = media_size(asset);
        entry.digest = asset.content_hash;
        entry.attributes["kind"] = std::string(media_kind_name(asset.kind));
        entry.attributes["sample_rate"] = std::to_string(asset.format.sample_rate);
        entry.attributes["channels"] = std::to_string(asset.format.channels);
        entry.attributes["offline"] = asset.offline ? "true" : "false";
        entry.attributes["generated"] = asset.generated ? "true" : "false";
        manifest.entries.push_back(std::move(entry));
    }

    for (const auto& track : session.timeline().tracks()) {
        ManifestEntry entry;
        entry.type = "track";
        entry.id = track.id;
        entry.label = track.name;
        entry.digest = combine_hash(track.id.value(), track.revision);
        entry.attributes["channels"] = std::to_string(track.channels);
        entry.attributes["gain"] = format_decimal(track.gain, 6);
        entry.attributes["pan"] = format_decimal(track.pan, 6);
        entry.attributes["muted"] = track.muted ? "true" : "false";
        entry.attributes["solo"] = track.solo ? "true" : "false";
        manifest.entries.push_back(std::move(entry));
    }

    for (const auto& clip : session.timeline().clips()) {
        ManifestEntry entry;
        entry.type = "clip";
        entry.id = clip.id;
        entry.label = "clip-" + clip.id.to_string().substr(8);
        entry.size = static_cast<std::uint64_t>(std::max<std::int64_t>(0, clip.length));
        entry.digest = combine_hash(clip.media_id.value(), static_cast<std::uint64_t>(clip.timeline_start));
        entry.attributes["track"] = clip.track_id.to_string();
        entry.attributes["media"] = clip.media_id.to_string();
        entry.attributes["timeline_start"] = std::to_string(clip.timeline_start);
        entry.attributes["source_start"] = std::to_string(clip.source_start);
        entry.attributes["length"] = std::to_string(clip.length);
        entry.attributes["group"] = std::to_string(clip.group);
        manifest.entries.push_back(std::move(entry));
    }

    for (const auto& plugin : session.routing().plugins()) {
        ManifestEntry entry;
        entry.type = "plugin";
        entry.id = plugin.id;
        entry.label = trim(plugin.vendor + " " + plugin.product);
        entry.digest = combine_hash(plugin.id.value(), plugin.generation);
        entry.attributes["track"] = plugin.track_id.to_string();
        entry.attributes["latency"] = std::to_string(plugin.latency);
        entry.attributes["parameters"] = std::to_string(plugin.parameters.size());
        entry.attributes["bypassed"] = plugin.bypassed ? "true" : "false";
        entry.attributes["instrument"] = plugin.instrument ? "true" : "false";
        manifest.entries.push_back(std::move(entry));
    }

    for (const auto& job : session.rendering().jobs()) {
        ManifestEntry entry;
        entry.type = "render";
        entry.id = job.id;
        entry.label = "render-" + job.id.to_string().substr(8);
        entry.size = static_cast<std::uint64_t>(std::max<std::int64_t>(0, job.end_sample - job.start_sample));
        entry.digest = combine_hash(job.id.value(), job.revision);
        entry.attributes["track"] = job.track_id.to_string();
        entry.attributes["sample_rate"] = std::to_string(job.sample_rate);
        entry.attributes["channels"] = std::to_string(job.channels);
        entry.attributes["priority"] = std::to_string(job.priority);
        manifest.entries.push_back(std::move(entry));
    }

    for (std::size_t index = 0; index < session.package().document().sections.size(); ++index) {
        const auto& section = session.package().document().sections[index];
        ManifestEntry entry;
        entry.type = "section";
        entry.id = EntityId(combine_hash(index + 1, fnv1a64(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(section.tag.data()), section.tag.size()))));
        entry.label = section_tag_string(section.tag);
        entry.size = section.payload.size();
        entry.digest = fnv1a64(section.payload);
        entry.attributes["flags"] = std::to_string(section.flags);
        entry.attributes["revision"] = std::to_string(section.revision);
        manifest.entries.push_back(std::move(entry));
    }

    std::stable_sort(manifest.entries.begin(), manifest.entries.end(), [](const ManifestEntry& left, const ManifestEntry& right) {
        return std::tie(left.type, left.path, left.label, left.id) <
               std::tie(right.type, right.path, right.label, right.id);
    });
    return manifest;
}

std::string serialize_manifest(const SessionManifest& manifest) {
    std::ostringstream out;
    out << "WCMF\t" << manifest.version << '\t' << manifest.session_fingerprint << '\n';
    for (const auto& entry : manifest.entries) {
        out << escape_field(entry.type) << '\t'
            << entry.id.to_string() << '\t'
            << escape_field(entry.path) << '\t'
            << escape_field(entry.label) << '\t'
            << entry.size << '\t'
            << entry.digest << '\t'
            << attributes_text(entry.attributes) << '\n';
    }
    return out.str();
}

Result<SessionManifest> parse_manifest(std::string_view text) {
    const auto lines = split(text, '\n', false);
    if (lines.empty()) return Status::failure(ErrorCode::truncated, "manifest is empty");
    const auto header = split(lines.front(), '\t', true);
    if (header.size() != 3 || header[0] != "WCMF") {
        return Status::failure(ErrorCode::invalid_magic, "manifest header is invalid");
    }
    SessionManifest manifest;
    const auto version = parse_integer(header[1]);
    const auto fingerprint = parse_unsigned(header[2]);
    if (!version || !fingerprint || *version < 1 || *version > 100) {
        return Status::failure(ErrorCode::unsupported_version, "manifest version is unsupported");
    }
    manifest.version = static_cast<std::uint32_t>(*version);
    manifest.session_fingerprint = static_cast<std::uint64_t>(*fingerprint);

    for (std::size_t line_number = 1; line_number < lines.size(); ++line_number) {
        const auto fields = split(lines[line_number], '\t', true);
        if (fields.size() != 7) {
            return Status::failure(ErrorCode::invalid_value,
                                   "manifest line " + std::to_string(line_number + 1) + " has wrong field count");
        }
        const auto id = EntityId::parse(fields[1]);
        const auto size = parse_unsigned(fields[4]);
        const auto digest = parse_unsigned(fields[5]);
        if (!id || !size || !digest) {
            return Status::failure(ErrorCode::invalid_value,
                                   "manifest line " + std::to_string(line_number + 1) + " contains invalid numbers");
        }
        ManifestEntry entry;
        entry.type = unescape_field(fields[0]);
        entry.id = *id;
        entry.path = unescape_field(fields[2]);
        entry.label = unescape_field(fields[3]);
        entry.size = static_cast<std::uint64_t>(*size);
        entry.digest = static_cast<std::uint64_t>(*digest);
        entry.attributes = parse_attributes(fields[6]);
        manifest.entries.push_back(std::move(entry));
    }
    return manifest;
}

std::vector<std::string> manifest_conflicts(const SessionManifest& manifest) {
    std::vector<std::string> result;
    std::map<std::pair<std::string, EntityId>, std::size_t> ids;
    std::map<std::string, EntityId> paths;
    for (const auto& entry : manifest.entries) {
        const auto key = std::make_pair(entry.type, entry.id);
        if (++ids[key] > 1) result.push_back("duplicate " + entry.type + " id " + entry.id.to_string());
        if (!entry.path.empty()) {
            const auto normalized = ascii_lower(normalize_path(entry.path));
            const auto found = paths.find(normalized);
            if (found != paths.end() && found->second != entry.id) {
                result.push_back("path conflict " + normalized + " between " + found->second.to_string() + " and " + entry.id.to_string());
            } else {
                paths[normalized] = entry.id;
            }
        }
        if (!path_is_relative(entry.path) || path_has_parent_escape(entry.path)) {
            result.push_back("unsafe path on " + entry.id.to_string() + ": " + entry.path);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<ManifestEntry> manifest_entries_by_type(const SessionManifest& manifest,
                                                    std::string_view type) {
    std::vector<ManifestEntry> result;
    const auto lowered = ascii_lower(type);
    for (const auto& entry : manifest.entries) if (ascii_lower(entry.type) == lowered) result.push_back(entry);
    return result;
}

std::optional<ManifestEntry> find_manifest_entry(const SessionManifest& manifest, EntityId id) {
    const auto found = std::find_if(manifest.entries.begin(), manifest.entries.end(), [id](const ManifestEntry& entry) {
        return entry.id == id;
    });
    return found == manifest.entries.end() ? std::nullopt : std::optional<ManifestEntry>(*found);
}

std::uint64_t manifest_digest(const SessionManifest& manifest) noexcept {
    auto digest = combine_hash(manifest.version, manifest.session_fingerprint);
    for (const auto& entry : manifest.entries) {
        digest = combine_hash(digest, fnv1a64(entry.type));
        digest = combine_hash(digest, entry.id.value());
        digest = combine_hash(digest, fnv1a64(entry.path));
        digest = combine_hash(digest, fnv1a64(entry.label));
        digest = combine_hash(digest, entry.size);
        digest = combine_hash(digest, entry.digest);
        for (const auto& [key, value] : entry.attributes) {
            digest = combine_hash(digest, fnv1a64(key));
            digest = combine_hash(digest, fnv1a64(value));
        }
    }
    return digest;
}

std::string manifest_summary(const SessionManifest& manifest) {
    std::map<std::string, std::size_t> counts;
    std::uint64_t total_size = 0;
    for (const auto& entry : manifest.entries) {
        ++counts[entry.type];
        total_size += entry.size;
    }
    std::ostringstream out;
    out << "manifest version: " << manifest.version << '\n'
        << "session fingerprint: " << manifest.session_fingerprint << '\n'
        << "manifest digest: " << manifest_digest(manifest) << '\n'
        << "entries: " << manifest.entries.size() << '\n'
        << "total logical size: " << total_size << '\n';
    for (const auto& [type, count] : counts) out << type << ": " << count << '\n';
    for (const auto& conflict : manifest_conflicts(manifest)) out << "conflict: " << conflict << '\n';
    return out.str();
}

}  // namespace wavecairn
