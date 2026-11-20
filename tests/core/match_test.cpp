#include "core/graph.hpp"
#include "core/match.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace pwcap::core {
namespace {

// The four Match fields paired with the NodeInfo member of the same name.
struct FieldPair {
    Pattern Match::* pattern;
    std::string NodeInfo::* field;
};

constexpr std::array<FieldPair, 4> kFields{{
    {&Match::app_name, &NodeInfo::app_name},
    {&Match::binary, &NodeInfo::binary},
    {&Match::node_name, &NodeInfo::node_name},
    {&Match::media_name, &NodeInfo::media_name},
}};

NodeInfo node_with(const FieldPair& pair, std::string value) {
    NodeInfo node;
    node.*pair.field = std::move(value);
    return node;
}

Match match_with(const FieldPair& pair, Pattern pattern) {
    Match match;
    match.*pair.pattern = std::move(pattern);
    return match;
}

NodeInfo firefox_tab(std::string media_name) {
    NodeInfo node;
    node.app_name = "Firefox";
    node.media_name = std::move(media_name);
    node.media_class = std::string{kStreamOutputAudio};
    return node;
}

TEST(MatchRules, ExactIsCaseSensitiveOnEveryField) {
    for (const auto& pair : kFields) {  // loop over a fixed table of member pointers
        const auto match = match_with(pair, Pattern{"Firefox", MatchKind::Exact});
        EXPECT_TRUE(matches(match, node_with(pair, "Firefox")));
        EXPECT_FALSE(matches(match, node_with(pair, "firefox")));
        EXPECT_FALSE(matches(match, node_with(pair, "Firefox2")));
    }
}

TEST(MatchRules, PrefixMatchesOnlyTheStart) {
    const Match match{.app_name = Pattern{"Fire", MatchKind::Prefix}};
    EXPECT_TRUE(matches(match, firefox_tab("x")));

    NodeInfo other;
    other.app_name = "MyFirefox";
    EXPECT_FALSE(matches(match, other));
}

TEST(MatchRules, SubstringTreatsGlobCharactersLiterally) {
    const Match inner{.app_name = Pattern{"efo", MatchKind::Substring}};
    EXPECT_TRUE(matches(inner, firefox_tab("x")));

    const Match glob{.app_name = Pattern{"Fire*", MatchKind::Substring}};
    EXPECT_FALSE(matches(glob, firefox_tab("x")));
    NodeInfo literal;
    literal.app_name = "Fire*fox";
    EXPECT_TRUE(matches(glob, literal));

    const Match question{.app_name = Pattern{"Fire?ox", MatchKind::Substring}};
    EXPECT_FALSE(matches(question, firefox_tab("x")));
}

TEST(MatchRules, EmptyTextIsWildcard) {
    // Loop over the three kinds: the wildcard rule must not depend on the kind.
    for (const auto kind : {MatchKind::Exact, MatchKind::Prefix, MatchKind::Substring}) {
        const Match match{.app_name = Pattern{"", kind}};
        EXPECT_TRUE(matches(match, firefox_tab("x")));
        EXPECT_TRUE(matches(match, NodeInfo{}));
    }

    const Match only_node_name{.node_name = Pattern{"x", MatchKind::Exact}};
    NodeInfo node;
    node.node_name = "x";
    node.app_name = "anything";
    EXPECT_TRUE(matches(only_node_name, node));
}

TEST(MatchRules, AllNonEmptyFieldsMustHit) {
    const Match match{.app_name = Pattern{"Firefox", MatchKind::Exact},
                      .media_name = Pattern{"pwspike-tab-440", MatchKind::Exact}};
    EXPECT_TRUE(matches(match, firefox_tab("pwspike-tab-440")));
    EXPECT_FALSE(matches(match, firefox_tab("pwspike-tab-880")));

    NodeInfo wrong_app = firefox_tab("pwspike-tab-440");
    wrong_app.app_name = "Brave";
    EXPECT_FALSE(matches(match, wrong_app));
}

TEST(MatchRules, NonEmptyPatternNeverHitsEmptyField) {
    // s0-item2: application.process.binary is null for pw-play, so the field is empty.
    NodeInfo pw_play;
    pw_play.app_name = "pw-play";
    pw_play.node_name = "pwspike-tone440";

    const Match binary{.binary = Pattern{"pw-play", MatchKind::Exact}};
    EXPECT_FALSE(matches(binary, pw_play));

    const Match prefix{.binary = Pattern{"pw", MatchKind::Prefix}};
    EXPECT_FALSE(matches(prefix, pw_play));
    const Match substring{.binary = Pattern{"play", MatchKind::Substring}};
    EXPECT_FALSE(matches(substring, pw_play));
}

TEST(Selection, AllWildcardIncludeOrExcludeIsInvalid) {
    const Match valid{.app_name = Pattern{"Firefox", MatchKind::Exact}};

    const auto wildcard_include = Selection::make(Match{});
    ASSERT_FALSE(wildcard_include.has_value());
    EXPECT_EQ(wildcard_include.error(), SelectionError::InvalidMatch);

    // Empty texts of any kind are still all-wildcard.
    const auto empty_text_include = Selection::make(Match{.app_name = Pattern{"", MatchKind::Prefix}});
    ASSERT_FALSE(empty_text_include.has_value());
    EXPECT_EQ(empty_text_include.error(), SelectionError::InvalidMatch);

    const auto wildcard_exclude = Selection::make(valid, std::vector<Match>{Match{}});
    ASSERT_FALSE(wildcard_exclude.has_value());
    EXPECT_EQ(wildcard_exclude.error(), SelectionError::InvalidMatch);

    const Match exclude{.media_name = Pattern{"x", MatchKind::Exact}};
    const auto ok = Selection::make(valid, std::vector<Match>{exclude});
    EXPECT_TRUE(ok.has_value());
}

TEST(Selection, ExcludeKeepsOnlyTab440) {
    const Match include{.app_name = Pattern{"Firefox", MatchKind::Exact}};
    const Match exclude{.media_name = Pattern{"pwspike-tab-880", MatchKind::Exact}};
    const auto selection = Selection::make(include, std::vector<Match>{exclude});
    ASSERT_TRUE(selection.has_value());

    EXPECT_TRUE(selection->selects(firefox_tab("pwspike-tab-440")));
    EXPECT_FALSE(selection->selects(firefox_tab("pwspike-tab-880")));

    NodeInfo other_app = firefox_tab("pwspike-tab-440");
    other_app.app_name = "Brave";
    EXPECT_FALSE(selection->selects(other_app));

    const auto no_exclude = Selection::make(include);
    ASSERT_TRUE(no_exclude.has_value());
    EXPECT_TRUE(no_exclude->selects(firefox_tab("pwspike-tab-880")));
    EXPECT_TRUE(no_exclude->exclude().empty());
}

}  // namespace
}  // namespace pwcap::core
