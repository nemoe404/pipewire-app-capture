#include "cli/render.hpp"

#include "pwcap/list.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using pwcap::AppStream;
using pwcap::cli::display_safe;
using pwcap::cli::json_escape;
using pwcap::cli::render_json;
using pwcap::cli::render_text;

// U+FFFD in UTF-8.
constexpr std::string_view kReplacement = "\xEF\xBF\xBD";

// Two streams: one with a pid, one without and with an empty media name.
[[nodiscard]] std::vector<AppStream> two_streams() {
    const AppStream with_pid{
        .app_name = "a",
        .binary = "b",
        .node_name = "n",
        .media_name = "m",
        .serial = 7,
        .pid = 42,
    };
    const AppStream without_pid{
        .app_name = "x",
        .binary = "y",
        .node_name = "z",
        .media_name = "",
        .serial = 9,
        .pid = std::nullopt,
    };
    return {with_pid, without_pid};
}

} // namespace

TEST(Render, JsonEscapesQuoteBackslashAndControls) {
    EXPECT_EQ(json_escape("\""), "\\\"");
    EXPECT_EQ(json_escape("\\"), "\\\\");
    EXPECT_EQ(json_escape("\n"), "\\n");
    EXPECT_EQ(json_escape("\t"), "\\t");
    EXPECT_EQ(json_escape("\x01"), "\\u0001");
    EXPECT_EQ(json_escape("\x1f"), "\\u001f");
    // DEL and C1 are not C0 controls, so JSON passes them through.
    EXPECT_EQ(json_escape("\x7f"), "\x7f");
}

TEST(Render, JsonReplacesInvalidUtf8) {
    // Each count follows the maximal-subpart rule: "\xc0\xaf" is two units (C0 alone, then AF),
    // and "\xed\xa0\x80" is three (ED alone, then A0, then 80).
    EXPECT_EQ(json_escape("\xff"), kReplacement);
    EXPECT_EQ(json_escape("\xe2\x82"), kReplacement);
    EXPECT_EQ(json_escape("\xc0\xaf"), std::string{kReplacement} + std::string{kReplacement});
    EXPECT_EQ(json_escape("\xed\xa0\x80"),
              std::string{kReplacement} + std::string{kReplacement} + std::string{kReplacement});
    EXPECT_EQ(json_escape("\xe2\x82"
                          "A"),
              std::string{kReplacement} + "A");
}

TEST(Render, ValidUtf8PassesThrough) {
    // The hex escapes are split so that a hex digit after an escape cannot join it.
    const std::string text = "Gr\xc3\xbc\xc3\x9f"
                             "e \xe2\x80\x93 \xf0\x9f\x8e\xb5";
    EXPECT_EQ(json_escape(text), text);
    EXPECT_EQ(display_safe(text), text);
}

TEST(Render, JsonShape) {
    EXPECT_EQ(render_json(two_streams()),
              R"([{"serial":7,"pid":42,"app_name":"a","binary":"b","node_name":"n",)"
              R"("media_name":"m"},{"serial":9,"pid":null,"app_name":"x","binary":"y",)"
              R"("node_name":"z","media_name":""}])"
              "\n");
    EXPECT_EQ(render_json(std::vector<AppStream>{}), "[]\n");
}

TEST(Render, TextShape) {
    EXPECT_EQ(render_text(two_streams()), "SERIAL\tPID\tAPP\tBINARY\tNODE\tMEDIA\n"
                                          "7\t42\ta\tb\tn\tm\n"
                                          "9\t-\tx\ty\tz\t-\n");
    EXPECT_EQ(render_text(std::vector<AppStream>{}), "SERIAL\tPID\tAPP\tBINARY\tNODE\tMEDIA\n");
}

TEST(Render, TextNeutralisesControls) {
    EXPECT_EQ(display_safe("evil\x1b[2Jtitle\tx\ny"), "evil?[2Jtitle?x?y");
    EXPECT_EQ(display_safe("\x7f"), "?");
    // U+009B (CSI) is a C1 control, encoded as C2 9B.
    EXPECT_EQ(display_safe("\xc2\x9b"), "?");
    EXPECT_EQ(display_safe("\xff"), kReplacement);
}
