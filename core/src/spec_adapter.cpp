#include "fixcore/spec_adapter.hpp"

#include <cctype>
#include <cstdlib>

#include "fixcore/codec.hpp"

namespace fixcore {
namespace t {
inline constexpr int Account = 1, AvgPx = 6, ClOrdID = 11, CumQty = 14, ExecID = 17, LastPx = 31,
                     LastQty = 32, OrderID = 37, OrderQty = 38, OrdStatus = 39, OrdType = 40,
                     OrigClOrdID = 41, Price = 44, Side = 54, Text = 58, TimeInForce = 59,
                     TransactTime = 60, StopPx = 99, ExecType = 150, LeavesQty = 151,
                     CxlRejResponseTo = 434, Username = 553;
}

std::optional<std::string_view> lookup(std::span<const NormMap> map, std::string_view norm) {
    for (const auto& m : map)
        if (m.norm == norm) return m.value;
    return std::nullopt;
}

std::optional<std::string_view> reverse_lookup(std::span<const NormMap> map, std::string_view value) {
    for (const auto& m : map)
        if (m.value == value) return m.norm;
    return std::nullopt;
}

namespace {
std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : "; ") + s;
    return out;
}
}  // namespace

SpecAdapter::SpecAdapter(const VenueProfile& p) : p_(p) {
    for (const auto& m : p.side)
        if (auto s = parse_side(m.norm)) caps_.sides.push_back(*s);
    for (const auto& m : p.ord_type)
        if (auto s = parse_ord_type(m.norm)) caps_.ord_types.push_back(*s);
    for (const auto& m : p.tif)
        if (auto s = parse_tif(m.norm)) caps_.tifs.push_back(*s);
    for (const auto& m : p.exec_type)
        if (auto s = parse_exec_kind(m.norm)) caps_.exec_kinds.push_back(*s);
    caps_.cancel = p.cancel;
    caps_.replace = p.replace;
    caps_.has_account = p.account_tag != 0;
    caps_.account_required = p.account_required;
    if (auto* f = p.dict->field(p.instrument_tag)) caps_.instrument_field = f->name;
    for (const auto& lf : p.logon_fields) {
        caps_.logon_tags.push_back(lf.tag);
        if (!lf.credential.empty()) caps_.logon_credentials.emplace_back(lf.credential);
    }

    // Extras: NewOrderSingle fields not covered by the normalized model.
    if (const auto* nos = p.dict->message("D")) {
        auto covered = [&](int tag) {
            if (tag == p.instrument_tag || tag == p.account_tag) return true;
            for (const auto& tv : p.instrument_extra) if (tv.tag == tag) return true;
            for (const auto& tv : p.defaults) if (tv.tag == tag) return true;
            switch (tag) {
                case t::ClOrdID: case t::Side: case t::OrderQty: case t::OrdType: case t::Price:
                case t::StopPx: case t::TimeInForce: case t::TransactTime: return true;
            }
            return false;
        };
        for (const auto& r : nos->fields) {
            if (covered(r.tag)) continue;
            const FieldDef* fd = p.dict->field(r.tag);
            if (!fd) continue;
            ExtraField ef{std::string(fd->name), fd->tag, r.required, {}, std::string(fd->description)};
            for (const auto& ev : fd->values) ef.values.emplace_back(ev.value, ev.name);
            caps_.extras.push_back(std::move(ef));
        }
    }
}

SessionDefaults SpecAdapter::session_defaults() const {
    return {std::string(p_.begin_string), std::string(p_.default_appl_ver_id), p_.heartbeat_secs,
            p_.reset_seq_num_on_logon};
}

std::string SpecAdapter::apply_extras(Message& m, const MessageDef& def, const Extras& extra) const {
    for (const auto& [key, value] : extra) {
        const FieldDef* fd = nullptr;
        if (!key.empty() && std::isdigit(static_cast<unsigned char>(key[0])))
            fd = p_.dict->field(std::atoi(key.c_str()));
        else
            fd = p_.dict->field(key);
        if (!fd) return "unknown field '" + key + "' for venue " + std::string(p_.id);
        if (!def.rule(fd->tag)) return std::string(def.name) + " does not accept " + std::string(fd->name);
        m.set(fd->tag, value);
    }
    return {};
}

std::string SpecAdapter::apply_order_fields(Message& m, const MessageDef& def, std::string_view symbol,
                                            Side side, std::string_view qty, OrdType ord_type,
                                            const std::optional<std::string>& price,
                                            const std::optional<std::string>& stop_price,
                                            const std::optional<TimeInForce>& tif,
                                            const std::optional<std::string>& account,
                                            const Extras& extra) const {
    auto allowed = [&](int tag) { return def.rule(tag) != nullptr; };

    if (symbol.empty()) return "symbol is required";
    m.set(p_.instrument_tag, symbol);
    for (const auto& tv : p_.instrument_extra)
        if (allowed(tv.tag)) m.set(tv.tag, tv.value);

    auto side_v = lookup(p_.side, to_string(side));
    if (!side_v) return std::string(p_.name) + " does not support side " + std::string(to_string(side));
    m.set(t::Side, *side_v);

    if (allowed(t::OrderQty)) {
        if (!is_decimal(qty)) return "qty must be a positive decimal";
        m.set(t::OrderQty, qty);
    }

    if (allowed(t::OrdType)) {
        auto ot = lookup(p_.ord_type, to_string(ord_type));
        if (!ot) return std::string(p_.name) + " does not support " + std::string(to_string(ord_type)) + " orders";
        m.set(t::OrdType, *ot);
    }
    bool priced = ord_type == OrdType::Limit || ord_type == OrdType::StopLimit;
    if (priced && !price) return std::string(to_string(ord_type)) + " orders need a price";
    if (price) {
        if (!is_decimal(*price, true)) return "price must be a decimal";
        if (!priced) return "price given for a " + std::string(to_string(ord_type)) + " order";
        if (!allowed(t::Price)) return std::string(def.name) + " does not accept a price";
        m.set(t::Price, *price);
    }
    if (ord_type == OrdType::Stop || ord_type == OrdType::StopLimit) {
        if (!stop_price) return "stop orders need a stop_price";
        if (!allowed(t::StopPx)) return std::string(def.name) + " does not accept StopPx";
        m.set(t::StopPx, *stop_price);
    }

    if (tif) {
        auto v = lookup(p_.tif, to_string(*tif));
        if (!v) return std::string(p_.name) + " does not support time in force " + std::string(to_string(*tif));
        if (!allowed(t::TimeInForce)) return std::string(def.name) + " does not accept TimeInForce";
        m.set(t::TimeInForce, *v);
    } else if (const auto* r = def.rule(t::TimeInForce); r && r->required) {
        auto v = lookup(p_.tif, "day");
        if (!v) return std::string(p_.name) + " requires an explicit time in force";
        m.set(t::TimeInForce, *v);
    }

    if (account && !account->empty()) {
        if (!p_.account_tag || !allowed(p_.account_tag))
            return std::string(p_.name) + " does not take an account on " + std::string(def.name);
        m.set(p_.account_tag, *account);
    } else if (p_.account_required && allowed(p_.account_tag)) {
        return std::string(p_.name) + " requires an account";
    }

    for (const auto& tv : p_.defaults)
        if (allowed(tv.tag)) m.set(tv.tag, tv.value);
    if (allowed(t::TransactTime)) m.set(t::TransactTime, utc_timestamp());
    return apply_extras(m, def, extra);
}

Result<Message> SpecAdapter::finish(Message m) const {
    auto errors = p_.dict->validate(m);
    if (!errors.empty()) return Result<Message>::fail(join(errors));
    return m;
}

Result<Message> SpecAdapter::build_new_order(const NewOrder& o) const {
    const auto* def = p_.dict->message("D");
    if (!def) return Result<Message>::fail("venue has no NewOrderSingle");
    if (o.cl_ord_id.empty()) return Result<Message>::fail("cl_ord_id is required");
    Message m("D");
    m.set(t::ClOrdID, o.cl_ord_id);
    if (auto e = apply_order_fields(m, *def, o.symbol, o.side, o.qty, o.ord_type, o.price, o.stop_price,
                                    o.tif, o.account, o.extra); !e.empty())
        return Result<Message>::fail(e);
    return finish(std::move(m));
}

Result<Message> SpecAdapter::build_replace(const ReplaceOrder& o) const {
    const auto* def = p_.dict->message("G");
    if (!p_.replace || !def) return Result<Message>::fail(std::string(p_.name) + " does not support cancel/replace");
    Message m("G");
    m.set(t::OrigClOrdID, o.orig_cl_ord_id);
    m.set(t::ClOrdID, o.cl_ord_id);
    if (auto e = apply_order_fields(m, *def, o.symbol, o.side, o.qty, o.ord_type, o.price, std::nullopt,
                                    o.tif, o.account, o.extra); !e.empty())
        return Result<Message>::fail(e);
    return finish(std::move(m));
}

Result<Message> SpecAdapter::build_cancel(const CancelOrder& o) const {
    const auto* def = p_.dict->message("F");
    if (!p_.cancel || !def) return Result<Message>::fail(std::string(p_.name) + " does not support cancels");
    auto allowed = [&](int tag) { return def->rule(tag) != nullptr; };
    Message m("F");
    m.set(t::OrigClOrdID, o.orig_cl_ord_id);
    m.set(t::ClOrdID, o.cl_ord_id);
    m.set(p_.instrument_tag, o.symbol);
    for (const auto& tv : p_.instrument_extra)
        if (allowed(tv.tag)) m.set(tv.tag, tv.value);
    auto side_v = lookup(p_.side, to_string(o.side));
    if (!side_v) return Result<Message>::fail("unsupported side");
    m.set(t::Side, *side_v);
    if (allowed(t::OrderQty) && !o.qty.empty()) m.set(t::OrderQty, o.qty);
    if (allowed(t::TransactTime)) m.set(t::TransactTime, utc_timestamp());
    if (auto e = apply_extras(m, *def, o.extra); !e.empty()) return Result<Message>::fail(e);
    return finish(std::move(m));
}

Result<bool> SpecAdapter::decorate_logon(Message& logon, const Credentials& creds) const {
    for (const auto& lf : p_.logon_fields) {
        if (lf.credential.empty()) {
            logon.set(lf.tag, lf.value);
            continue;
        }
        auto it = creds.find(std::string(lf.credential));
        if (it == creds.end() || it->second.empty())
            return Result<bool>::fail("missing credential '" + std::string(lf.credential) + "'");
        logon.set(lf.tag, it->second);
    }
    if (!p_.default_appl_ver_id.empty()) logon.set(tags::DefaultApplVerID, p_.default_appl_ver_id);
    return true;
}

std::optional<ExecEvent> SpecAdapter::parse(const Message& m) const {
    ExecEvent ev;
    if (m.msg_type() == "8") {
        ev.type = ExecEvent::Type::Execution;
        auto et = m.get_or(t::ExecType);
        if (auto k = reverse_lookup(p_.exec_type, et); k && parse_exec_kind(*k)) ev.kind = *parse_exec_kind(*k);
        else ev.warnings.push_back("unmapped ExecType " + et);
    } else if (m.msg_type() == "9") {
        ev.type = ExecEvent::Type::CancelReject;
        ev.kind = ExecKind::Rejected;
    } else {
        return std::nullopt;
    }
    auto os = m.get_or(t::OrdStatus);
    if (auto s = reverse_lookup(p_.ord_status, os); s && parse_order_status(*s)) ev.status = *parse_order_status(*s);
    else ev.warnings.push_back("unmapped OrdStatus " + os);
    ev.cl_ord_id = m.get_or(t::ClOrdID);
    ev.orig_cl_ord_id = m.get_or(t::OrigClOrdID);
    ev.order_id = m.get_or(t::OrderID);
    ev.exec_id = m.get_or(t::ExecID);
    ev.symbol = m.get_or(p_.instrument_tag);
    ev.last_qty = m.get_or(t::LastQty);
    ev.last_px = m.get_or(t::LastPx);
    ev.cum_qty = m.get_or(t::CumQty);
    ev.leaves_qty = m.get_or(t::LeavesQty);
    ev.avg_px = m.get_or(t::AvgPx);
    ev.text = m.get_or(t::Text);
    for (auto& e : p_.dict->validate(m)) ev.warnings.push_back(std::move(e));
    return ev;
}

Result<Message> SpecAdapter::build_exec_report(const Message& req, const ExecReportArgs& a) const {
    const auto* def = p_.dict->message("8");
    if (!def) return Result<Message>::fail("venue has no ExecutionReport");
    auto allowed = [&](int tag) { return def->rule(tag) != nullptr; };
    auto et = lookup(p_.exec_type, to_string(a.kind));
    if (!et) return Result<Message>::fail("ExecType " + std::string(to_string(a.kind)) + " not mapped");
    auto os = lookup(p_.ord_status, to_string(a.status));
    if (!os) return Result<Message>::fail("OrdStatus " + std::string(to_string(a.status)) + " not mapped");

    Message m("8");
    m.set(t::OrderID, a.order_id.empty() ? "NONE" : a.order_id);
    m.set(t::ClOrdID, req.get_or(t::ClOrdID));
    if (allowed(t::OrigClOrdID) && req.has(t::OrigClOrdID)) m.set(t::OrigClOrdID, req.get_or(t::OrigClOrdID));
    m.set(t::ExecID, a.exec_id);
    m.set(t::ExecType, *et);
    m.set(t::OrdStatus, *os);
    for (int tag : {p_.instrument_tag, t::Side, t::OrderQty, t::Price, t::Account})
        if (allowed(tag) && req.has(tag)) m.set(tag, req.get_or(tag));
    for (const auto& tv : p_.instrument_extra)
        if (allowed(tv.tag)) m.set(tv.tag, req.get_or(tv.tag, tv.value));
    if (!a.last_qty.empty() && allowed(t::LastQty)) m.set(t::LastQty, a.last_qty);
    if (!a.last_px.empty() && allowed(t::LastPx)) m.set(t::LastPx, a.last_px);
    m.set(t::LeavesQty, a.leaves_qty);
    m.set(t::CumQty, a.cum_qty);
    m.set(t::AvgPx, a.avg_px);
    if (!a.text.empty() && allowed(t::Text)) m.set(t::Text, a.text);
    if (allowed(t::TransactTime)) m.set(t::TransactTime, utc_timestamp());
    return finish(std::move(m));
}

Result<Message> SpecAdapter::build_cancel_reject(const Message& req, std::string_view order_id,
                                                 OrderStatus status, std::string_view text) const {
    if (!p_.dict->message("9")) return Result<Message>::fail("venue has no OrderCancelReject");
    auto os = lookup(p_.ord_status, to_string(status));
    if (!os) return Result<Message>::fail("OrdStatus not mapped");
    Message m("9");
    m.set(t::OrderID, order_id.empty() ? "NONE" : order_id);
    m.set(t::ClOrdID, req.get_or(t::ClOrdID));
    m.set(t::OrigClOrdID, req.get_or(t::OrigClOrdID));
    m.set(t::OrdStatus, *os);
    m.set(t::CxlRejResponseTo, req.msg_type() == "G" ? "2" : "1");
    if (!text.empty()) m.set(t::Text, text);
    return finish(std::move(m));
}

}  // namespace fixcore
