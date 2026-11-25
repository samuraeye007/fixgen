#pragma once
// Static, generated description of what a venue accepts/sends. Codegen emits
// constexpr tables of these types; validation is generic.
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fixcore/message.hpp"

namespace fixcore {

enum class FieldType { String, Char, Int, SeqNum, NumInGroup, Length, Qty, Price, PriceOffset,
                       Amt, Float, Percentage, Boolean, UtcTimestamp, UtcDateOnly, UtcTimeOnly,
                       LocalMktDate, MonthYear, Currency, Exchange, MultipleValueString, Data };

struct EnumValue {
    std::string_view value;
    std::string_view name;
};

struct FieldDef {
    int tag;
    std::string_view name;
    FieldType type;
    bool custom;
    std::span<const EnumValue> values;
    std::string_view description;
};

struct Condition {  // "required when <tag> is one of <values>"
    int tag;
    std::span<const std::string_view> values;
};

struct MessageFieldRule {
    int tag;
    bool required;
    std::span<const Condition> required_when;
};

enum class Direction { ToVenue, FromVenue, Both };

struct MessageDef {
    std::string_view msg_type;
    std::string_view name;
    Direction direction;
    std::span<const MessageFieldRule> fields;

    const MessageFieldRule* rule(int tag) const;
};

struct Dictionary {
    std::span<const FieldDef> fields;
    std::span<const MessageDef> messages;

    const FieldDef* field(int tag) const;
    const FieldDef* field(std::string_view name) const;
    const MessageDef* message(std::string_view msg_type) const;

    // Checks body fields against the message definition: required / conditional fields,
    // enum values, basic type syntax, and fields the venue does not define for that message.
    // Session-level messages and header fields are not checked here.
    std::vector<std::string> validate(const Message& msg) const;
};

bool value_matches_type(FieldType type, std::string_view value);

}  // namespace fixcore
