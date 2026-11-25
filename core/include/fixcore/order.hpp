#pragma once
// Venue-neutral order model. Everything the client/UI speaks is in these terms;
// venue adapters translate to and from FIX.
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fixcore {

// Prices and quantities are kept as validated decimal strings end to end so nothing
// gets rounded through a double on its way to the wire.
bool is_decimal(std::string_view s, bool allow_negative = false);

enum class Side { Buy, Sell, SellShort, SellShortExempt };
enum class OrdType { Market, Limit, Stop, StopLimit };
enum class TimeInForce { Day, Gtc, Ioc, Fok, Gtd, AtOpen, AtClose };
enum class ExecKind { New, PartialFill, Fill, Trade, Canceled, Replaced, Rejected, Expired,
                      PendingNew, PendingCancel, PendingReplace, Restated, Unknown };
enum class OrderStatus { New, PartiallyFilled, Filled, Canceled, Replaced, Rejected, Expired,
                         PendingNew, PendingCancel, PendingReplace, Unknown };

// snake_case names used in venue specs, the gateway API and the UI.
std::string_view to_string(Side);
std::string_view to_string(OrdType);
std::string_view to_string(TimeInForce);
std::string_view to_string(ExecKind);
std::string_view to_string(OrderStatus);
std::optional<Side> parse_side(std::string_view);
std::optional<OrdType> parse_ord_type(std::string_view);
std::optional<TimeInForce> parse_tif(std::string_view);
std::optional<ExecKind> parse_exec_kind(std::string_view);
std::optional<OrderStatus> parse_order_status(std::string_view);

using Extras = std::map<std::string, std::string>;  // venue field name (or tag) -> value

struct NewOrder {
    std::string cl_ord_id;
    std::string symbol;
    Side side = Side::Buy;
    std::string qty;
    OrdType ord_type = OrdType::Limit;
    std::optional<std::string> price;
    std::optional<std::string> stop_price;
    std::optional<TimeInForce> tif;
    std::optional<std::string> account;
    Extras extra;
};

struct CancelOrder {
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string symbol;
    Side side = Side::Buy;
    std::string qty;
    Extras extra;
};

struct ReplaceOrder {
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string symbol;
    Side side = Side::Buy;
    std::string qty;
    OrdType ord_type = OrdType::Limit;
    std::optional<std::string> price;
    std::optional<TimeInForce> tif;
    std::optional<std::string> account;
    Extras extra;
};

struct ExecEvent {
    enum class Type { Execution, CancelReject } type = Type::Execution;
    std::string cl_ord_id, orig_cl_ord_id, order_id, exec_id, symbol;
    ExecKind kind = ExecKind::Unknown;
    OrderStatus status = OrderStatus::Unknown;
    std::string last_qty, last_px, cum_qty, leaves_qty, avg_px;
    std::string text;
    std::vector<std::string> warnings;  // venue sent something that disagrees with its spec
};

}  // namespace fixcore
