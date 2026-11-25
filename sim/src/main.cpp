// fixgen-sim: a thin, spec-driven FIX acceptor for testing, built on QuickFIX's acceptor.
// It is NOT a matching engine: it validates what it receives against the venue's own generated dictionary,
// then acks / fills / cancels / replaces according to a simple policy.
//
// Because it shares the venue spec with the gateway, it proves the plumbing and the
// spec's internal consistency, not that the spec matches the real venue. That part
// needs the venue's UAT/certification environment (see docs/ARCHITECTURE.md).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include "bridge.hpp"
#include "fixcore/codec.hpp"
#include "fixcore/venue_adapter.hpp"
#include "quickfix/Application.h"
#include "quickfix/MessageStore.h"
#include "quickfix/Session.h"
#include "quickfix/SocketAcceptor.h"

using namespace fixcore;

namespace {

enum class FillMode { None, Immediate, Partial };

struct Options {
    std::string venue;
    int port = 0;
    std::string comp_id = "SIM";
    std::string client_comp_id = "BLASTER";
    FillMode fill = FillMode::Immediate;
    std::string market_px = "100";
    std::string reject_symbol = "REJECT";  // orders for this symbol are rejected
    bool quiet = false;
};

struct SimOrder {
    std::string order_id;
    Message request;  // latest D or G
    double qty = 0, cum = 0, px = 0;
    bool live = true;
};

std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.8g", v);
    return buf;
}

class Sim : public FIX::Application {
public:
    Sim(const Options& opt, const VenueAdapter& venue) : opt_(opt), venue_(venue) {}

private:
    // FIX::Application
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID& sid) override {
        std::printf("[%s] logged on\n", sid.toString().c_str());
        std::fflush(stdout);
    }
    void onLogout(const FIX::SessionID& sid) override {
        std::printf("[%s] logged out\n", sid.toString().c_str());
        std::fflush(stdout);
        orders_.clear();
    }
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message& m, const FIX::SessionID&) override {
        if (m.getHeader().getField(FIX::FIELD::MsgType) != "A") return;
        for (int tag : venue_.capabilities().logon_tags)
            if (!m.isSetField(tag)) throw FIX::RejectLogon("logon missing tag " + std::to_string(tag));
    }
    void fromApp(const FIX::Message& m, const FIX::SessionID& sid) override {
        sid_ = sid;
        on_app(fixgen::qf::from_quickfix(m));
    }

    void send(Result<Message> r) {
        if (!r) {
            std::printf("!! cannot build response: %s\n", r.error().c_str());
            return;
        }
        FIX::Message q = fixgen::qf::to_quickfix(r.value());
        FIX::Session::sendToTarget(q, sid_);
    }

    ExecKind fill_kind(bool full) const {
        // FIX 4.4+ venues report fills as ExecType=Trade; FIX 4.2 ones as PartialFill/Fill.
        const auto& k = venue_.capabilities().exec_kinds;
        if (std::find(k.begin(), k.end(), ExecKind::Trade) != k.end()) return ExecKind::Trade;
        return full ? ExecKind::Fill : ExecKind::PartialFill;
    }

    VenueAdapter::ExecReportArgs args(const SimOrder& o, ExecKind k, OrderStatus s) {
        VenueAdapter::ExecReportArgs a;
        a.order_id = o.order_id;
        a.exec_id = "E" + std::to_string(++exec_seq_);
        a.kind = k;
        a.status = s;
        a.cum_qty = fmt(o.cum);
        a.leaves_qty = fmt(o.live ? o.qty - o.cum : 0);
        a.avg_px = o.cum > 0 ? fmt(o.px) : "0";
        return a;
    }

    void fill(SimOrder& o, double qty) {
        o.cum += qty;
        bool full = o.cum >= o.qty;
        auto a = args(o, fill_kind(full), full ? OrderStatus::Filled : OrderStatus::PartiallyFilled);
        a.last_qty = fmt(qty);
        a.last_px = fmt(o.px);
        if (full) {
            o.live = false;
            a.leaves_qty = "0";
        }
        send(venue_.build_exec_report(o.request, a));
    }

    void reject(const Message& req, const std::string& why) {
        VenueAdapter::ExecReportArgs a;
        a.exec_id = "E" + std::to_string(++exec_seq_);
        a.kind = ExecKind::Rejected;
        a.status = OrderStatus::Rejected;
        a.text = why.substr(0, 200);
        auto r = venue_.build_exec_report(req, a);
        if (!r) {  // venue cannot express a reject ER: fall back to a business message reject
            Message rj("j");
            rj.set(45, req.get_or(tags::MsgSeqNum));
            rj.set(372, req.msg_type());
            rj.set(380, "0");
            rj.set(tags::Text, why.substr(0, 200));
            FIX::Message q = fixgen::qf::to_quickfix(rj);
            FIX::Session::sendToTarget(q, sid_);
            return;
        }
        send(std::move(r));
    }

    void on_app(const Message& m) {
        auto errors = venue_.dictionary().validate(m);
        const auto& t = m.msg_type();
        if (!errors.empty()) {
            std::string why;
            for (auto& e : errors) why += (why.empty() ? "" : "; ") + e;
            std::printf("!! invalid %s: %s\n", t.c_str(), why.c_str());
            if (t == "F" || t == "G") send(venue_.build_cancel_reject(m, "", OrderStatus::Rejected, why));
            else reject(m, why);
            return;
        }
        if (t == "D") new_order(m);
        else if (t == "F") cancel(m);
        else if (t == "G") replace(m);
    }

    double price_of(const Message& m) const {
        auto p = m.get(44);
        return std::atof((p ? std::string(*p) : opt_.market_px).c_str());
    }

    std::string symbol_of(const Message& m) const {
        for (const auto& f : venue_.dictionary().fields)
            if (f.name == venue_.capabilities().instrument_field) return m.get_or(f.tag);
        return {};
    }

    void new_order(const Message& m) {
        std::string id = m.get_or(11);
        if (orders_.count(id)) return reject(m, "duplicate ClOrdID");
        if (symbol_of(m) == opt_.reject_symbol) return reject(m, "symbol not tradeable");
        SimOrder o;
        o.order_id = "O" + std::to_string(++order_seq_);
        o.request = m;
        o.qty = std::atof(m.get_or(38, "0").c_str());
        o.px = price_of(m);
        auto& stored = orders_[id] = o;
        send(venue_.build_exec_report(m, args(stored, ExecKind::New, OrderStatus::New)));
        if (opt_.fill == FillMode::Immediate) fill(stored, stored.qty);
        else if (opt_.fill == FillMode::Partial) fill(stored, std::max(1.0, static_cast<double>(static_cast<long>(stored.qty / 2))));
    }

    SimOrder* find_live(const Message& m) {
        auto it = orders_.find(m.get_or(41));
        return it == orders_.end() || !it->second.live ? nullptr : &it->second;
    }

    void cancel(const Message& m) {
        SimOrder* o = find_live(m);
        if (!o) return send(venue_.build_cancel_reject(m, "", OrderStatus::Rejected, "unknown or closed order"));
        o->live = false;
        auto a = args(*o, ExecKind::Canceled, OrderStatus::Canceled);
        send(venue_.build_exec_report(m, a));
        orders_[m.get_or(11)] = *o;
    }

    void replace(const Message& m) {
        SimOrder* o = find_live(m);
        if (!o) return send(venue_.build_cancel_reject(m, "", OrderStatus::Rejected, "unknown or closed order"));
        double new_qty = std::atof(m.get_or(38, "0").c_str());
        if (new_qty < o->cum) return send(venue_.build_cancel_reject(m, o->order_id, OrderStatus::PartiallyFilled, "qty below filled"));
        SimOrder updated = *o;
        o->live = false;
        updated.request = m;
        updated.qty = new_qty;
        updated.px = price_of(m);
        auto& stored = orders_[m.get_or(11)] = updated;
        send(venue_.build_exec_report(m, args(stored, ExecKind::Replaced, stored.cum > 0 ? OrderStatus::PartiallyFilled : OrderStatus::New)));
    }

    const Options& opt_;
    const VenueAdapter& venue_;
    FIX::SessionID sid_;
    std::map<std::string, SimOrder> orders_;
    long order_seq_ = 0, exec_seq_ = 0;
};

int usage() {
    std::fprintf(stderr,
                 "usage: fixgen-sim --venue ID --port N [--comp-id SIM] [--client-comp-id BLASTER]\n"
                 "                  [--fill none|immediate|partial] [--market-px 100]\n"
                 "                  [--reject-symbol REJECT] [--quiet]\n"
                 "venues:");
    for (const auto& v : make_all_venues()) std::fprintf(stderr, " %s", std::string(v->id()).c_str());
    std::fprintf(stderr, "\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--venue") opt.venue = next();
        else if (a == "--port") opt.port = std::atoi(next().c_str());
        else if (a == "--comp-id") opt.comp_id = next();
        else if (a == "--client-comp-id") opt.client_comp_id = next();
        else if (a == "--market-px") opt.market_px = next();
        else if (a == "--reject-symbol") opt.reject_symbol = next();
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--fill") {
            auto f = next();
            if (f == "none") opt.fill = FillMode::None;
            else if (f == "immediate") opt.fill = FillMode::Immediate;
            else if (f == "partial") opt.fill = FillMode::Partial;
            else return usage();
        } else return usage();
    }
    auto venue = make_venue(opt.venue);
    if (!venue || opt.port <= 0) return usage();

    fixgen::qf::SessionParams p;
    p.sender_comp_id = opt.comp_id;
    p.target_comp_id = opt.client_comp_id;
    p.initiator = false;
    p.port = opt.port;
    try {
        FIX::SessionSettings settings;
        fixgen::qf::add_session(settings, *venue, p);
        Sim app(opt, *venue);
        FIX::MemoryStoreFactory store;
        fixgen::qf::CallbackLogFactory log([&](const FIX::SessionID*, fixgen::qf::CallbackLogFactory::Kind k,
                                               const std::string& text) {
            using K = fixgen::qf::CallbackLogFactory::Kind;
            if (opt.quiet && k != K::Event) return;
            std::printf("%s %s\n", k == K::Incoming ? "<<" : k == K::Outgoing ? ">>" : "--",
                        fixgen::qf::display(text).c_str());
            std::fflush(stdout);
        });
        FIX::SocketAcceptor acceptor(app, store, settings, log);
        std::printf("fixgen-sim: %s (%s) on port %d as %s, expecting %s\n", std::string(venue->id()).c_str(),
                    venue->session_defaults().begin_string.c_str(), opt.port, opt.comp_id.c_str(),
                    opt.client_comp_id.c_str());
        std::fflush(stdout);
        acceptor.block();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fixgen-sim: %s\n", e.what());
        return 1;
    }
}
