#include "core/match.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace pwcap::core {

namespace {

bool hits(const Pattern& pattern, std::string_view field) noexcept {
    if (pattern.text.empty()) {
        return true;
    }
    switch (pattern.kind) {
        case MatchKind::Exact:
            return field == pattern.text;
        case MatchKind::Prefix:
            return field.starts_with(pattern.text);
        case MatchKind::Substring:
            return field.contains(pattern.text);
    }
    std::unreachable();
}

}  // namespace

bool matches(const Match& match, const NodeInfo& node) noexcept {
    return hits(match.app_name, node.app_name) && hits(match.binary, node.binary) &&
           hits(match.node_name, node.node_name) && hits(match.media_name, node.media_name);
}

bool is_wildcard(const Match& match) noexcept {
    const std::array<const Pattern*, 4> patterns{&match.app_name, &match.binary, &match.node_name,
                                                 &match.media_name};
    return std::ranges::all_of(patterns, [](const Pattern* p) { return p->text.empty(); });
}

std::expected<Selection, SelectionError> Selection::make(Match include,
                                                         std::vector<Match> exclude) {
    // An all-wildcard include would select every application, and an all-wildcard exclude would
    // silently exclude every node; both are rejected here, at open time (CORE-03).
    if (is_wildcard(include) || std::ranges::any_of(exclude, is_wildcard)) {
        return std::unexpected{SelectionError::InvalidMatch};
    }
    return Selection{std::move(include), std::move(exclude)};
}

bool Selection::selects(const NodeInfo& node) const noexcept {
    return matches(include_, node) &&
           std::ranges::none_of(exclude_, [&node](const Match& m) { return matches(m, node); });
}

}  // namespace pwcap::core
