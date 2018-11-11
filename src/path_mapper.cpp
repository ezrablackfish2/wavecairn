#include "wavecairn/path_mapper.h"

#include "wavecairn/text.h"

#include <algorithm>
#include <sstream>

namespace wavecairn {
namespace {

std::string_view kind_name(PathRuleKind kind) noexcept {
    switch (kind) {
        case PathRuleKind::prefix: return "prefix";
        case PathRuleKind::suffix: return "suffix";
        case PathRuleKind::exact: return "exact";
        case PathRuleKind::extension: return "extension";
        case PathRuleKind::directory: return "directory";
    }
    return "prefix";
}

std::optional<PathRuleKind> parse_kind(std::string_view text) {
    const auto lowered = ascii_lower(trim(text));
    if (lowered == "prefix") return PathRuleKind::prefix;
    if (lowered == "suffix") return PathRuleKind::suffix;
    if (lowered == "exact") return PathRuleKind::exact;
    if (lowered == "extension") return PathRuleKind::extension;
    if (lowered == "directory") return PathRuleKind::directory;
    return std::nullopt;
}

std::string comparable(std::string_view text, bool case_sensitive) {
    return case_sensitive ? std::string(text) : ascii_lower(text);
}

}  // namespace

void PathMapper::add_rule(PathRule rule) {
    rule.match = normalize_path(rule.match);
    rule.replacement = normalize_path(rule.replacement);
    rules_.push_back(std::move(rule));
}

void PathMapper::clear() { rules_.clear(); }
const std::vector<PathRule>& PathMapper::rules() const noexcept { return rules_; }

std::string PathMapper::map(std::string_view path, std::size_t* matched_rules) const {
    std::string current = normalize_path(path);
    std::size_t count = 0;
    for (const auto& rule : rules_) {
        if (!matches(rule, current)) continue;
        current = replace(rule, current);
        current = normalize_path(current);
        ++count;
        if (rule.stop_after_match) break;
    }
    if (matched_rules) *matched_rules = count;
    return current;
}

std::vector<PathMapping> PathMapper::plan(const Session& session) const {
    std::vector<PathMapping> result;
    result.reserve(session.media().assets().size());
    for (const auto& asset : session.media().assets()) {
        PathMapping mapping;
        mapping.media_id = asset.id;
        mapping.original = asset.relative_path;
        mapping.mapped = map(asset.relative_path, &mapping.matched_rules);
        mapping.changed = mapping.original != mapping.mapped;
        mapping.safe = path_is_relative(mapping.mapped) && !path_has_parent_escape(mapping.mapped);
        result.push_back(std::move(mapping));
    }
    std::stable_sort(result.begin(), result.end(), [](const PathMapping& left, const PathMapping& right) {
        if (left.original != right.original) return left.original < right.original;
        return left.media_id < right.media_id;
    });
    return result;
}

std::size_t PathMapper::apply(Session& session, Diagnostics& diagnostics) const {
    std::size_t changed = 0;
    for (const auto& mapping : plan(session)) {
        if (!mapping.changed) continue;
        if (!mapping.safe) {
            diagnostics.error(0,
                              "path-mapper",
                              "unsafe-result",
                              "rule mapping for " + mapping.media_id.to_string() + " escaped the package root");
            continue;
        }
        auto status = session.media().update_path(mapping.media_id, mapping.mapped, 0, diagnostics);
        if (status) ++changed;
        else diagnostics.warning(0, "path-mapper", "update-failed", status.message());
    }
    return changed;
}

std::string PathMapper::describe() const {
    std::ostringstream out;
    for (std::size_t index = 0; index < rules_.size(); ++index) {
        const auto& rule = rules_[index];
        out << index + 1 << ". " << kind_name(rule.kind) << " \"" << rule.match << "\" -> \""
            << rule.replacement << "\" " << (rule.case_sensitive ? "case-sensitive" : "case-insensitive")
            << (rule.stop_after_match ? " stop" : " continue") << '\n';
    }
    return out.str();
}

bool PathMapper::matches(const PathRule& rule, std::string_view path) const {
    const auto candidate = comparable(path, rule.case_sensitive);
    const auto match = comparable(rule.match, rule.case_sensitive);
    switch (rule.kind) {
        case PathRuleKind::prefix:
            return candidate.starts_with(match);
        case PathRuleKind::suffix:
            return candidate.ends_with(match);
        case PathRuleKind::exact:
            return candidate == match;
        case PathRuleKind::extension: {
            const auto slash = candidate.find_last_of('/');
            const auto dot = candidate.find_last_of('.');
            return dot != std::string::npos && (slash == std::string::npos || dot > slash) && candidate.substr(dot) == match;
        }
        case PathRuleKind::directory: {
            const auto parts = split(candidate, '/', false);
            return std::find(parts.begin(), parts.end(), match) != parts.end();
        }
    }
    return false;
}

std::string PathMapper::replace(const PathRule& rule, std::string_view path) const {
    const auto candidate = comparable(path, rule.case_sensitive);
    const auto match = comparable(rule.match, rule.case_sensitive);
    switch (rule.kind) {
        case PathRuleKind::prefix:
            return rule.replacement + std::string(path.substr(match.size()));
        case PathRuleKind::suffix:
            return std::string(path.substr(0, path.size() - match.size())) + rule.replacement;
        case PathRuleKind::exact:
            return rule.replacement;
        case PathRuleKind::extension: {
            const auto dot = candidate.find_last_of('.');
            return std::string(path.substr(0, dot)) + rule.replacement;
        }
        case PathRuleKind::directory: {
            auto parts = split(path, '/', false);
            for (auto& part : parts) {
                if (comparable(part, rule.case_sensitive) == match) part = rule.replacement;
            }
            return join(parts, "/");
        }
    }
    return std::string(path);
}

Result<std::vector<PathRule>> parse_path_rules(std::string_view text) {
    std::vector<PathRule> rules;
    const auto lines = split(text, '\n', false);
    for (std::size_t line_number = 0; line_number < lines.size(); ++line_number) {
        auto line = trim(lines[line_number]);
        if (line.empty() || line.starts_with('#')) continue;
        const auto fields = split(line, '\t', true);
        if (fields.size() < 3 || fields.size() > 5) {
            return Status::failure(ErrorCode::invalid_value,
                                   "path rule line " + std::to_string(line_number + 1) + " has wrong field count");
        }
        const auto kind = parse_kind(fields[0]);
        if (!kind) {
            return Status::failure(ErrorCode::invalid_value,
                                   "path rule line " + std::to_string(line_number + 1) + " has unknown kind");
        }
        PathRule rule;
        rule.kind = *kind;
        rule.match = percent_decode(fields[1]);
        rule.replacement = percent_decode(fields[2]);
        if (fields.size() >= 4) rule.case_sensitive = ascii_lower(fields[3]) == "case";
        if (fields.size() >= 5) rule.stop_after_match = ascii_lower(fields[4]) != "continue";
        rules.push_back(std::move(rule));
    }
    return rules;
}

std::string serialize_path_rules(const std::vector<PathRule>& rules) {
    std::ostringstream out;
    for (const auto& rule : rules) {
        out << kind_name(rule.kind) << '\t'
            << percent_encode(rule.match) << '\t'
            << percent_encode(rule.replacement) << '\t'
            << (rule.case_sensitive ? "case" : "nocase") << '\t'
            << (rule.stop_after_match ? "stop" : "continue") << '\n';
    }
    return out.str();
}

}  // namespace wavecairn
