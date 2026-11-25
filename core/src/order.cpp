#include "fixcore/order.hpp"

#include <array>
#include <cctype>

namespace fixcore {

bool is_decimal(std::string_view s, bool allow_negative) {
    if (s.empty()) return false;
    std::size_t i = 0;
    if (allow_negative && s[0] == '-') i = 1;
    bool digits = false, dot = false;
    for (; i < s.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i]))) digits = true;
        else if (s[i] == '.' && !dot) dot = true;
        else return false;
    }
    return digits && s.back() != '.';
}

namespace {
template <class E, std::size_t N>
std::optional<E> parse_enum(const std::array<std::string_view, N>& names, std::string_view s) {
    for (std::size_t i = 0; i < N; ++i)
        if (names[i] == s) return static_cast<E>(i);
    return std::nullopt;
}

constexpr std::array<std::string_view, 4> kSide{"buy", "sell", "sell_short", "sell_short_exempt"};
constexpr std::array<std::string_view, 4> kOrdType{"market", "limit", "stop", "stop_limit"};
constexpr std::array<std::string_view, 7> kTif{"day", "gtc", "ioc", "fok", "gtd", "at_open", "at_close"};
constexpr std::array<std::string_view, 13> kExec{"new", "partial_fill", "fill", "trade", "canceled",
    "replaced", "rejected", "expired", "pending_new", "pending_cancel", "pending_replace", "restated", "unknown"};
constexpr std::array<std::string_view, 11> kStatus{"new", "partially_filled", "filled", "canceled",
    "replaced", "rejected", "expired", "pending_new", "pending_cancel", "pending_replace", "unknown"};
}  // namespace

std::string_view to_string(Side v) { return kSide[static_cast<int>(v)]; }
std::string_view to_string(OrdType v) { return kOrdType[static_cast<int>(v)]; }
std::string_view to_string(TimeInForce v) { return kTif[static_cast<int>(v)]; }
std::string_view to_string(ExecKind v) { return kExec[static_cast<int>(v)]; }
std::string_view to_string(OrderStatus v) { return kStatus[static_cast<int>(v)]; }
std::optional<Side> parse_side(std::string_view s) { return parse_enum<Side>(kSide, s); }
std::optional<OrdType> parse_ord_type(std::string_view s) { return parse_enum<OrdType>(kOrdType, s); }
std::optional<TimeInForce> parse_tif(std::string_view s) { return parse_enum<TimeInForce>(kTif, s); }
std::optional<ExecKind> parse_exec_kind(std::string_view s) { return parse_enum<ExecKind>(kExec, s); }
std::optional<OrderStatus> parse_order_status(std::string_view s) { return parse_enum<OrderStatus>(kStatus, s); }

}  // namespace fixcore
