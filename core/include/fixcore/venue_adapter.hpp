#pragma once
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fixcore/dictionary.hpp"
#include "fixcore/message.hpp"
#include "fixcore/order.hpp"
#include "fixcore/result.hpp"

namespace fixcore {

using Credentials = std::map<std::string, std::string>;  // username, password, member_code, ...

struct SessionDefaults {
    std::string begin_string;
    std::string default_appl_ver_id;  // FIXT only
    int heartbeat_secs = 30;
    bool reset_seq_num_on_logon = true;
};

// An order-entry field the client may (or must) set that the normalized model does not
// cover, e.g. a venue-specific capacity flag.
struct ExtraField {
    std::string name;
    int tag = 0;
    bool required = false;
    std::vector<std::pair<std::string, std::string>> values;  // value, name
    std::string description;
};

struct Capabilities {
    std::vector<Side> sides;
    std::vector<OrdType> ord_types;
    std::vector<TimeInForce> tifs;
    std::vector<ExecKind> exec_kinds;  // ExecType values the venue reports
    bool cancel = false;
    bool replace = false;
    std::string instrument_field;
    bool account_required = false;
    bool has_account = false;
    std::vector<ExtraField> extras;
    std::vector<std::string> logon_credentials;  // credential keys the session needs
    std::vector<int> logon_tags;                 // tags the venue expects on Logon
};

class VenueAdapter {
public:
    virtual ~VenueAdapter() = default;

    virtual std::string_view id() const = 0;
    virtual std::string_view name() const = 0;
    virtual SessionDefaults session_defaults() const = 0;
    virtual const Capabilities& capabilities() const = 0;
    virtual const Dictionary& dictionary() const = 0;

    // Client -> venue.
    virtual Result<Message> build_new_order(const NewOrder&) const = 0;
    virtual Result<Message> build_cancel(const CancelOrder&) const = 0;
    virtual Result<Message> build_replace(const ReplaceOrder&) const = 0;
    // Adds venue-specific logon fields (credentials, ApplVerID ...). Error if a credential is missing.
    virtual Result<bool> decorate_logon(Message& logon, const Credentials&) const = 0;

    // Venue -> client. nullopt if the message is not an order-entry response.
    virtual std::optional<ExecEvent> parse(const Message&) const = 0;

    // --- Counterparty side, used by the test acceptor (fixgen-sim). ---
    struct ExecReportArgs {
        std::string order_id, exec_id;
        ExecKind kind = ExecKind::New;
        OrderStatus status = OrderStatus::New;
        std::string cum_qty = "0", leaves_qty = "0", avg_px = "0";
        std::string last_qty, last_px, text;
    };
    // Build an ExecutionReport answering `request` (a D, F or G this venue accepted).
    virtual Result<Message> build_exec_report(const Message& request, const ExecReportArgs&) const = 0;
    virtual Result<Message> build_cancel_reject(const Message& request, std::string_view order_id,
                                                OrderStatus status, std::string_view text) const = 0;
};

// Implemented by generated code (registry.cpp).
std::vector<std::unique_ptr<VenueAdapter>> make_all_venues();
std::unique_ptr<VenueAdapter> make_venue(std::string_view id);

}  // namespace fixcore
