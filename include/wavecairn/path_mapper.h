#pragma once

#include "wavecairn/session.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wavecairn {

enum class PathRuleKind {
    prefix,
    suffix,
    exact,
    extension,
    directory,
};

struct PathRule {
    PathRuleKind kind{PathRuleKind::prefix};
    std::string match;
    std::string replacement;
    bool case_sensitive{false};
    bool stop_after_match{true};
};

struct PathMapping {
    MediaId media_id;
    std::string original;
    std::string mapped;
    std::size_t matched_rules{0};
    bool changed{false};
    bool safe{true};
};

class PathMapper {
public:
    void add_rule(PathRule rule);
    void clear();
    const std::vector<PathRule>& rules() const noexcept;

    std::string map(std::string_view path, std::size_t* matched_rules = nullptr) const;
    std::vector<PathMapping> plan(const Session& session) const;
    std::size_t apply(Session& session, Diagnostics& diagnostics) const;
    std::string describe() const;

private:
    bool matches(const PathRule& rule, std::string_view path) const;
    std::string replace(const PathRule& rule, std::string_view path) const;

    std::vector<PathRule> rules_;
};

Result<std::vector<PathRule>> parse_path_rules(std::string_view text);
std::string serialize_path_rules(const std::vector<PathRule>& rules);

}  // namespace wavecairn
