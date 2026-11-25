#pragma once
// Generic VenueAdapter driven by generated tables. Venues whose quirks cannot be
// expressed in the spec subclass SpecAdapter and override the relevant hook.
#include <span>

#include "fixcore/venue_adapter.hpp"

namespace fixcore {

struct NormMap {  // normalized name ("buy", "ioc", "trade", ...) -> venue wire value
    std::string_view norm;
    std::string_view value;
};

struct TagValue {
    int tag;
    std::string_view value;
};

struct LogonField {
    int tag;
    std::string_view credential;  // key in Credentials, or empty
    std::string_view value;       // constant, used when credential is empty
};

struct VenueProfile {
    std::string_view id, name, description;
    std::string_view begin_string, default_appl_ver_id;
    int heartbeat_secs;
    bool reset_seq_num_on_logon;
    std::span<const LogonField> logon_fields;
    const Dictionary* dict;
    int instrument_tag;
    std::span<const TagValue> instrument_extra;
    int account_tag;  // 0 = venue has no account mapping
    bool account_required;
    std::span<const NormMap> side, ord_type, tif, exec_type, ord_status;
    std::span<const TagValue> defaults;
    bool cancel, replace;
};

class SpecAdapter : public VenueAdapter {
public:
    explicit SpecAdapter(const VenueProfile& p);

    std::string_view id() const override { return p_.id; }
    std::string_view name() const override { return p_.name; }
    SessionDefaults session_defaults() const override;
    const Capabilities& capabilities() const override { return caps_; }
    const Dictionary& dictionary() const override { return *p_.dict; }

    Result<Message> build_new_order(const NewOrder&) const override;
    Result<Message> build_cancel(const CancelOrder&) const override;
    Result<Message> build_replace(const ReplaceOrder&) const override;
    Result<bool> decorate_logon(Message&, const Credentials&) const override;
    std::optional<ExecEvent> parse(const Message&) const override;

    Result<Message> build_exec_report(const Message& request, const ExecReportArgs&) const override;
    Result<Message> build_cancel_reject(const Message& request, std::string_view order_id,
                                        OrderStatus status, std::string_view text) const override;

protected:
    const VenueProfile& profile() const { return p_; }
    // Shared by new/replace. Returns error text or empty.
    std::string apply_order_fields(Message& m, const MessageDef& def, std::string_view symbol,
                                   Side side, std::string_view qty, OrdType ord_type,
                                   const std::optional<std::string>& price,
                                   const std::optional<std::string>& stop_price,
                                   const std::optional<TimeInForce>& tif,
                                   const std::optional<std::string>& account,
                                   const Extras& extra) const;
    std::string apply_extras(Message& m, const MessageDef& def, const Extras& extra) const;
    Result<Message> finish(Message m) const;

private:
    const VenueProfile& p_;
    Capabilities caps_;
};

std::optional<std::string_view> lookup(std::span<const NormMap> map, std::string_view norm);
std::optional<std::string_view> reverse_lookup(std::span<const NormMap> map, std::string_view value);

}  // namespace fixcore
