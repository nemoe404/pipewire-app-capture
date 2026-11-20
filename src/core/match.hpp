#pragma once

// Match rules and the validated Selection built from them. Pure functions over NodeInfo.

#include "core/graph.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pwcap::core {

enum class MatchKind : std::uint8_t { Exact, Prefix, Substring };

// Empty text is a wildcard. Comparison is a case-sensitive byte comparison with no glob syntax.
struct Pattern {
    std::string text{};
    MatchKind kind{MatchKind::Exact};
};

// Each field pairs with the NodeInfo member of the same name. All non-empty fields must hit.
struct Match {
    Pattern app_name{};
    Pattern binary{};
    Pattern node_name{};
    Pattern media_name{};
};

[[nodiscard]] bool matches(const Match& match, const NodeInfo& node) noexcept;

// True when every pattern text is empty, so the Match would hit any node.
[[nodiscard]] bool is_wildcard(const Match& match) noexcept;

enum class SelectionError : std::uint8_t { InvalidMatch };

// A validated include rule plus exclude rules. The constructor is private, so a Selection with
// an all-wildcard include cannot exist.
class Selection {
public:
    [[nodiscard]] static std::expected<Selection, SelectionError> make(
        Match include, std::vector<Match> exclude = {});

    [[nodiscard]] const Match& include() const noexcept { return include_; }
    [[nodiscard]] std::span<const Match> exclude() const noexcept { return exclude_; }

    // True when the include hits and no exclude hits.
    [[nodiscard]] bool selects(const NodeInfo& node) const noexcept;

private:
    Selection(Match include, std::vector<Match> exclude)
        : include_{std::move(include)}, exclude_{std::move(exclude)} {}

    Match include_;
    std::vector<Match> exclude_;
};

}  // namespace pwcap::core
