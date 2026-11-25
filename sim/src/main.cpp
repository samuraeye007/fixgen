// fixgen-sim: a thin, spec-driven FIX acceptor for testing. It is NOT a matching
// engine: it validates what it receives against the venue's own generated dictionary,
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

#include "fixcore/codec.hpp"
#include "fixcore/net.hpp"
#include "fixcore/session.hpp"
#include "fixcore/venue_adapter.hpp"

using namespace fixcore;

namespace {

enum class FillMode { None, Immediate, Partial };

struct Options {
    std::string venue;
    std::string host = "127.0.0.1";
    int port = 0;
    std::string comp_id = "SIM";
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

class SimConnection {
public:
    SimConnection(const Options& opt, const VenueAdapter& venue, std::shared_ptr<net::TcpConnection> c)
        : opt_(opt), venue_(venue), conn_(std::move(c)) {
        auto d = venue_.session_defaults();
        SessionConfig sc;
        sc.begin_string = d.begin_string;
        sc.default_appl_ver_id = d.default_appl_ver_id;
        sc.heartbeat_secs = d.heartbeat_secs;
        sc.reset_seq_num_on_logon = d.reset_seq_num_on_logon;
        sc.sender_comp_id = opt_.comp_id;
        sc.initiator = false;
        session_ = std::make_unique<Session>(sc, [this](const std::string& b) { conn_->send(b); });
        session_->accept_logon = [this](const Message& logon) -> std::string {
            for (int tag : venue_.capabilities().logon_tags)
                if (!logon.has(tag)) return "logon missing tag " + std::to_string(tag);
            return {};
        };
        session_->on_raw = [this](bool out, const std::string& raw) {
            if (opt_.quiet) return;
            std::string s = raw;
            for (auto& ch : s) if (ch == SOH) ch = '|';
            std::printf("%s %s\n", out ? ">>" : "<<", s.c_str());
            std::fflush(stdout);
        };
        session_->on_state = [this](SessionState s, const std::string& why) {
            std::printf("[%s] %s (%s)\n", conn_->peer().c_str(), std::string(to_string(s)).c_str(), why.c_str());
            std::fflush(stdout);
        };
        session_->request_disconnect = [this] { conn_->close("session ended"); };
        session_->on_app_message = [this](const Message& m) { on_app(m); };
        conn_->on_data = [this](std::string_view d) { session_->on_bytes(d); };
        session_->connected();
    }

    Session& session() { return *session_; }
    std::shared_ptr<net::TcpConnection> conn() { return conn_; }

private:
    void send(Result<Message> r) {
        if (!r) {
            std::printf("!! cannot build response: %s\n", r.error().c_str());
            return;
        }
        session_->send(std::move(r.value()));
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
        if (!r) {  // venue cannot express a reject ER: fall back to a session-level reject
            Message rj(msgtype::Reject);
            rj.set(tags::RefSeqNum, req.get_or(tags::MsgSeqNum));
            rj.set(tags::Text, why.substr(0, 200));
            session_->send(std::move(rj));
            return;
        }
        session_->send(std::move(r.value()));
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
    std::shared_ptr<net::TcpConnection> conn_;
    std::unique_ptr<Session> session_;
    std::map<std::string, SimOrder> orders_;
    long order_seq_ = 0, exec_seq_ = 0;
};

int usage() {
    std::fprintf(stderr,
                 "usage: fixgen-sim --venue ID --port N [--host H] [--comp-id SIM]\n"
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
        else if (a == "--host") opt.host = next();
        else if (a == "--comp-id") opt.comp_id = next();
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

    std::string err;
    int lfd = net::tcp_listen(opt.host, opt.port, &err);
    if (lfd < 0) { std::fprintf(stderr, "listen: %s\n", err.c_str()); return 1; }

    net::Reactor reactor;
    std::map<net::TcpConnection*, std::unique_ptr<SimConnection>> conns;
    reactor.add(lfd, [&](short) {
        std::string peer;
        int fd;
        while ((fd = net::tcp_accept(lfd, &peer)) >= 0) {
            auto c = net::TcpConnection::adopt(reactor, fd, peer);
            auto* key = c.get();
            auto sim = std::make_unique<SimConnection>(opt, *venue, c);
            c->on_close = [&, key](const std::string& why) {
                if (auto it = conns.find(key); it != conns.end()) {
                    it->second->session().disconnected(why);
                    conns.erase(it);
                }
            };
            conns[key] = std::move(sim);
        }
    });
    reactor.every(std::chrono::milliseconds(500), [&] {
        for (auto& [k, c] : conns) c->session().tick();
    });
    std::printf("fixgen-sim: %s (%s) on %s:%d as %s\n", std::string(venue->id()).c_str(),
                venue->session_defaults().begin_string.c_str(), opt.host.c_str(), opt.port, opt.comp_id.c_str());
    std::fflush(stdout);
    reactor.run();
}
