#include "cli/render.hpp"

#include "pwcap/list.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pwcap::cli {

namespace {

constexpr std::uint8_t kAsciiLimit = 0x80U;
constexpr std::uint8_t kContinuationFirst = 0x80U;
constexpr std::uint8_t kContinuationLast = 0xBFU;
constexpr std::uint8_t kContinuationMask = 0x3FU;
constexpr std::uint32_t kContinuationBits = 6U;

constexpr std::uint32_t kReplacement = 0xFFFDU;
constexpr std::string_view kReplacementBytes = "\xEF\xBF\xBD";

constexpr std::uint32_t kC0Limit = 0x20U;
constexpr std::uint32_t kDel = 0x7FU;
constexpr std::uint32_t kC1First = 0x80U;
constexpr std::uint32_t kC1Last = 0x9FU;

constexpr std::string_view kAbsent = "-";
constexpr char kSafeReplacement = '?';

// One unit of text: a valid code point with the bytes it occupies, or one U+FFFD standing for a
// maximal invalid subpart. length is at least 1, so a walk over the text always advances.
struct Step {
    std::uint32_t code_point;
    std::size_t length;
    bool valid;
};

// The shape of a multi-byte sequence, from the lead byte. The second byte has its own range
// (E0, ED, F0 and F4 narrow it), so overlong forms, surrogates and code points above U+10FFFF are
// rejected at the second byte. length 0 means the lead byte cannot start a sequence.
struct Shape {
    std::size_t length;
    std::uint8_t lead_mask;
    std::uint8_t second_min;
    std::uint8_t second_max;
};

[[nodiscard]] constexpr Shape shape_of(std::uint8_t lead) noexcept {
    constexpr Shape kInvalid{.length = 0, .lead_mask = 0, .second_min = 0, .second_max = 0};
    if (lead >= 0xC2U && lead <= 0xDFU) {
        return Shape{.length = 2,
                     .lead_mask = 0x1FU,
                     .second_min = kContinuationFirst,
                     .second_max = kContinuationLast};
    }
    if (lead == 0xE0U) {
        return Shape{
            .length = 3, .lead_mask = 0x0FU, .second_min = 0xA0U, .second_max = kContinuationLast};
    }
    if ((lead >= 0xE1U && lead <= 0xECU) || lead == 0xEEU || lead == 0xEFU) {
        return Shape{.length = 3,
                     .lead_mask = 0x0FU,
                     .second_min = kContinuationFirst,
                     .second_max = kContinuationLast};
    }
    if (lead == 0xEDU) {
        return Shape{
            .length = 3, .lead_mask = 0x0FU, .second_min = kContinuationFirst, .second_max = 0x9FU};
    }
    if (lead == 0xF0U) {
        return Shape{
            .length = 4, .lead_mask = 0x07U, .second_min = 0x90U, .second_max = kContinuationLast};
    }
    if (lead >= 0xF1U && lead <= 0xF3U) {
        return Shape{.length = 4,
                     .lead_mask = 0x07U,
                     .second_min = kContinuationFirst,
                     .second_max = kContinuationLast};
    }
    if (lead == 0xF4U) {
        return Shape{
            .length = 4, .lead_mask = 0x07U, .second_min = kContinuationFirst, .second_max = 0x8FU};
    }
    return kInvalid;
}

// Decodes the unit that starts at pos. The bytes after the lead byte are checked one at a time, and
// the first one that does not fit ends the unit. The unit is then a replacement covering the bytes
// read so far, which is the maximal subpart: "\xed\xa0\x80" is three replacements, not one.
[[nodiscard]] Step decode_at(std::string_view text, std::size_t pos) noexcept {
    const auto lead = static_cast<std::uint8_t>(text[pos]);
    if (lead < kAsciiLimit) {
        return Step{.code_point = lead, .length = 1, .valid = true};
    }
    const Shape shape = shape_of(lead);
    if (shape.length == 0) {
        return Step{.code_point = kReplacement, .length = 1, .valid = false};
    }
    auto code = static_cast<std::uint32_t>(lead & shape.lead_mask);
    // A raw loop: each continuation byte is checked in turn, and the walk stops at the first bad
    // one, which sets the length of the replacement. No range algorithm returns that consumed
    // count.
    for (std::size_t offset = 1; offset < shape.length; ++offset) {
        if (pos + offset >= text.size()) {
            return Step{.code_point = kReplacement, .length = offset, .valid = false};
        }
        const auto byte = static_cast<std::uint8_t>(text[pos + offset]);
        const std::uint8_t low = offset == 1 ? shape.second_min : kContinuationFirst;
        const std::uint8_t high = offset == 1 ? shape.second_max : kContinuationLast;
        if (byte < low || byte > high) {
            return Step{.code_point = kReplacement, .length = offset, .valid = false};
        }
        code = (code << kContinuationBits) | static_cast<std::uint32_t>(byte & kContinuationMask);
    }
    return Step{.code_point = code, .length = shape.length, .valid = true};
}

// Calls visit(step, bytes) for every unit of text, in order. bytes is the source slice of the unit.
template <typename Visit> void for_each_step(std::string_view text, const Visit& visit) {
    // A raw loop: every unit has a different length, so the next position comes from the unit just
    // decoded. No range algorithm walks text that way.
    std::size_t pos = 0;
    while (pos < text.size()) {
        const Step step = decode_at(text, pos);
        visit(step, text.substr(pos, step.length));
        pos += step.length;
    }
}

[[nodiscard]] bool is_terminal_control(std::uint32_t code_point) noexcept {
    return code_point < kC0Limit || code_point == kDel ||
           (code_point >= kC1First && code_point <= kC1Last);
}

// Appends the JSON form of one unit. Only the quote, the backslash and the C0 controls change.
void append_json_unit(std::string& out, const Step& step, std::string_view bytes) {
    if (!step.valid) {
        out += kReplacementBytes;
        return;
    }
    switch (step.code_point) {
    case '"':
        out += R"(\")";
        return;
    case '\\':
        out += R"(\\)";
        return;
    case '\b':
        out += R"(\b)";
        return;
    case '\f':
        out += R"(\f)";
        return;
    case '\n':
        out += R"(\n)";
        return;
    case '\r':
        out += R"(\r)";
        return;
    case '\t':
        out += R"(\t)";
        return;
    default:
        break;
    }
    if (step.code_point < kC0Limit) {
        out += std::format("\\u{:04x}", step.code_point);
        return;
    }
    out += bytes;
}

// Appends the terminal-safe form of one unit: a control becomes '?', an invalid unit U+FFFD.
void append_safe_unit(std::string& out, const Step& step, std::string_view bytes) {
    if (!step.valid) {
        out += kReplacementBytes;
    } else if (is_terminal_control(step.code_point)) {
        out += kSafeReplacement;
    } else {
        out += bytes;
    }
}

// The text form of a field: an empty value is '-', anything else is made terminal-safe.
[[nodiscard]] std::string text_field(std::string_view value) {
    if (value.empty()) {
        return std::string{kAbsent};
    }
    return display_safe(value);
}

[[nodiscard]] std::string text_pid(std::optional<std::int32_t> pid) {
    if (!pid) {
        return std::string{kAbsent};
    }
    return std::format("{}", *pid);
}

[[nodiscard]] std::string json_pid(std::optional<std::int32_t> pid) {
    if (!pid) {
        return "null";
    }
    return std::format("{}", *pid);
}

} // namespace

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for_each_step(text, [&out](const Step& step, std::string_view bytes) {
        append_json_unit(out, step, bytes);
    });
    return out;
}

std::string display_safe(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for_each_step(text, [&out](const Step& step, std::string_view bytes) {
        append_safe_unit(out, step, bytes);
    });
    return out;
}

std::string render_text(std::span<const pwcap::AppStream> streams) {
    std::string out = "SERIAL\tPID\tAPP\tBINARY\tNODE\tMEDIA\n";
    std::ranges::for_each(streams, [&out](const pwcap::AppStream& stream) {
        out += std::format("{}\t{}\t{}\t{}\t{}\t{}\n", stream.serial, text_pid(stream.pid),
                           text_field(stream.app_name), text_field(stream.binary),
                           text_field(stream.node_name), text_field(stream.media_name));
    });
    return out;
}

std::string render_json(std::span<const pwcap::AppStream> streams) {
    std::string out = "[";
    bool first = true;
    std::ranges::for_each(streams, [&out, &first](const pwcap::AppStream& stream) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += std::format(R"({{"serial":{},"pid":{},"app_name":"{}","binary":"{}",)"
                           R"("node_name":"{}","media_name":"{}"}})",
                           stream.serial, json_pid(stream.pid), json_escape(stream.app_name),
                           json_escape(stream.binary), json_escape(stream.node_name),
                           json_escape(stream.media_name));
    });
    out += "]\n";
    return out;
}

} // namespace pwcap::cli
