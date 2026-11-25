// fixgen-gateway: holds FIX sessions to venues (QuickFIX), translates normalized orders
// through the generated venue adapters, and serves the order-blaster UI + JSON/SSE API.
//
// Threading: everything runs on one thread. The reactor drives the HTTP server and calls
// SocketInitiator::poll() every few milliseconds, so QuickFIX callbacks never race the API.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "bridge.hpp"
#include "fixcore/codec.hpp"
#include "fixcore/json.hpp"
#include "fixcore/net.hpp"
#include "fixcore/venue_adapter.hpp"
#include "http.hpp"
#include "quickfix/Application.h"
#include "quickfix/FileStore.h"
#include "quickfix/Session.h"
#include "quickfix/SocketInitiator.h"

using namespace fixcore;
namespace js = fixcore::json;
using gateway::Request;
using gateway::Response;

namespace {

using Clock = std::chrono::steady_clock;

std::string read_file(const std::string& path, bool* ok = nullptr) {
    std::ifstream f(path, std::ios::binary);
    if (ok) *ok = static_cast<bool>(f);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// "${NAME}" -> getenv("NAME"), so secrets can stay out of the config file.
std::string expand_env(const std::string& s) {
    static const std::regex re(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)\})");
    std::string out;
    auto begin = std::sregex_iterator(s.begin(), s.end(), re);
    std::size_t last = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        out += s.substr(last, it->position() - last);
        const char* v = std::getenv((*it)[1].str().c_str());
        out += v ? v : "";
        last = it->position() + it->length();
    }
    return out + s.substr(last);
}

Response json_response(int status, const js::Value& v) { return {status, "application/json", v.dump()}; }
Response error_response(int status, const std::string& msg) {
    js::Object o{{"error", msg}};
    return json_response(status, o);
}

struct VenueSession {
    std::string name;
    std::unique_ptr<VenueAdapter> adapter;
    FIX::SessionID sid;
    std::string host;
    int port = 0;
    Credentials creds;
    std::string state = "disconnected";
    std::string last_event;
};

struct OrderRecord {
    std::string key;  // first ClOrdID; stable handle for the UI
    std::string session, venue, cl_ord_id, symbol, qty, price, account;
    Side side = Side::Buy;
    OrdType ord_type = OrdType::Limit;
    std::optional<TimeInForce> tif;
    Extras extra;
    OrderStatus status = OrderStatus::PendingNew;
    std::string cum_qty = "0", leaves_qty, avg_px = "0", order_id, text;
    std::string pending;  // "cancel" / "replace" while outstanding
    std::string pending_qty, pending_price;
    std::vector<std::string> warnings;
};

struct QueuedOrder {
    std::string session;
    NewOrder order;
};

class Gateway : public FIX::Application {
public:
    Gateway(net::Reactor& r, gateway::HttpServer& http, std::string ui_dir)
        : r_(r), http_(http), ui_dir_(std::move(ui_dir)),
          log_factory_([this](const FIX::SessionID* sid, fixgen::qf::CallbackLogFactory::Kind k,
                              const std::string& text) { on_log(sid, k, text); }) {
        auto t = std::chrono::system_clock::now().time_since_epoch();
        id_prefix_ = "FG" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(t).count() % 100000000) + "-";
        http_.handler = [this](const Request& rq) { return handle(rq); };
        http_.on_sse_open = [this](const auto& c) { snapshot(c); };
        r_.every(std::chrono::milliseconds(250), [this] { refresh_states(); });
        r_.every(std::chrono::milliseconds(10), [this] { drain_queue(); });
    }

    bool add_session(const js::Value& cfg, const std::string& dict_dir, const std::string& store_dir,
                     std::string* err) {
        auto vs = std::make_unique<VenueSession>();
        std::string venue = cfg["venue"].as_string();
        vs->adapter = make_venue(venue);
        if (!vs->adapter) { *err = "unknown venue '" + venue + "' (not in spec/venues?)"; return false; }
        vs->name = cfg["name"].as_string(venue);
        if (sessions_.count(vs->name)) { *err = "duplicate session name " + vs->name; return false; }
        vs->host = cfg["host"].as_string("127.0.0.1");
        vs->port = static_cast<int>(cfg["port"].as_int());
        for (const auto& [k, v] : cfg["credentials"].as_object()) vs->creds[k] = expand_env(v.as_string());
        Message probe("A");
        if (auto r = vs->adapter->decorate_logon(probe, vs->creds); !r) { *err = vs->name + ": " + r.error(); return false; }

        fixgen::qf::SessionParams p;
        p.sender_comp_id = cfg["sender_comp_id"].as_string();
        p.target_comp_id = cfg["target_comp_id"].as_string();
        if (p.sender_comp_id.empty() || p.target_comp_id.empty()) { *err = vs->name + ": comp ids required"; return false; }
        p.initiator = true;
        p.host = vs->host;
        p.port = vs->port;
        p.heartbeat_secs = static_cast<int>(cfg["heartbeat_secs"].as_int(0));
        p.reconnect_secs = static_cast<int>(cfg["reconnect_secs"].as_int(5));
        p.dict_dir = dict_dir;
        p.store_dir = store_dir;
        try {
            vs->sid = fixgen::qf::add_session(settings_, *vs->adapter, p);
        } catch (const std::exception& e) {
            *err = vs->name + ": " + e.what();
            return false;
        }
        by_sid_[vs->sid] = vs.get();
        order_.push_back(vs->name);
        sessions_[vs->name] = std::move(vs);
        return true;
    }

    // Call once all sessions are added.
    void start() {
        store_ = std::make_unique<FIX::FileStoreFactory>(settings_);
        initiator_ = std::make_unique<FIX::SocketInitiator>(*this, *store_, settings_, log_factory_);
        r_.every(std::chrono::milliseconds(2), [this] { poll(); });
    }

private:
    // ---------- QuickFIX side ----------
    void poll() {
        try {
            initiator_->poll();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "quickfix: %s\n", e.what());
        }
    }

    VenueSession* find(const FIX::SessionID& sid) {
        auto it = by_sid_.find(sid);
        return it == by_sid_.end() ? nullptr : it->second;
    }

    static FIX::Session* qf_session(const VenueSession& vs) { return FIX::Session::lookupSession(vs.sid); }

    static std::string state_of(const VenueSession& vs) {
        auto* s = qf_session(vs);
        if (!s) return "disconnected";
        if (s->isLoggedOn()) return "active";
        if (s->sentLogout()) return "logout_sent";
        if (s->sentLogon()) return "logon_sent";
        return s->isEnabled() ? "disconnected" : "disabled";
    }

    void refresh_states() {
        for (auto& [name, vs] : sessions_) {
            auto st = state_of(*vs);
            if (st == vs->state) continue;
            vs->state = st;
            std::printf("[%s] session %s\n", vs->name.c_str(), st.c_str());
            std::fflush(stdout);
            http_.broadcast("session", session_json(*vs).dump());
        }
    }

    void on_log(const FIX::SessionID* sid, fixgen::qf::CallbackLogFactory::Kind k, const std::string& text) {
        VenueSession* vs = sid ? find(*sid) : nullptr;
        std::string name = vs ? vs->name : "-";
        if (k == fixgen::qf::CallbackLogFactory::Kind::Event) {
            if (vs) vs->last_event = text;
            std::printf("[%s] %s\n", name.c_str(), text.c_str());
            std::fflush(stdout);
            return;
        }
        bool out = k == fixgen::qf::CallbackLogFactory::Kind::Outgoing;
        js::Object o{{"session", name}, {"dir", out ? "out" : "in"}, {"msg", fixgen::qf::display(text)}};
        std::string s = js::Value(o).dump();
        fix_log_.push_back(s);
        if (fix_log_.size() > 300) fix_log_.pop_front();
        http_.broadcast("fix", s);
    }

    // FIX::Application
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID&) override { r_.post([this] { refresh_states(); }); }
    void onLogout(const FIX::SessionID&) override { r_.post([this] { refresh_states(); }); }
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void toAdmin(FIX::Message& msg, const FIX::SessionID& sid) override {
        VenueSession* vs = find(sid);
        if (!vs || msg.getHeader().getField(FIX::FIELD::MsgType) != "A") return;
        Message extra("A");
        if (auto r = vs->adapter->decorate_logon(extra, vs->creds); !r) {
            vs->last_event = r.error();
            return;
        }
        for (const auto& f : extra.body()) msg.setField(f.tag, f.value);
    }
    void fromAdmin(const FIX::Message& msg, const FIX::SessionID& sid) override {
        VenueSession* vs = find(sid);
        if (!vs) return;
        const auto& type = msg.getHeader().getField(FIX::FIELD::MsgType);
        if (type == "3" || type == "5") {
            std::string text = msg.isSetField(58) ? msg.getField(58) : "";
            if (type == "5" && text.empty()) return;
            http_.broadcast("notice", js::Value(js::Object{{"session", vs->name}, {"level", "error"},
                {"text", (type == "3" ? "session reject of seq " + (msg.isSetField(45) ? msg.getField(45) : "?") + ": "
                                      : "logout: ") + text}}).dump());
            vs->last_event = text;
        }
    }
    void fromApp(const FIX::Message& msg, const FIX::SessionID& sid) override {
        if (VenueSession* vs = find(sid)) on_venue_message(*vs, fixgen::qf::from_quickfix(msg));
    }

    bool send(VenueSession& vs, const Message& m) {
        FIX::Message q = fixgen::qf::to_quickfix(m);
        try {
            return FIX::Session::sendToTarget(q, vs.sid);
        } catch (const FIX::SessionNotFound&) {
            return false;
        }
    }

    bool active(const VenueSession& vs) const {
        auto* s = qf_session(vs);
        return s && s->isLoggedOn();
    }

    void on_venue_message(VenueSession& vs, const Message& m) {
        auto ev = vs.adapter->parse(m);
        if (!ev) return;
        std::string key;
        for (const auto& id : {ev->cl_ord_id, ev->orig_cl_ord_id})
            if (auto it = by_cl_ord_id_.find(id); !id.empty() && it != by_cl_ord_id_.end()) { key = it->second; break; }
        auto it = orders_.find(key);
        if (it == orders_.end()) {
            http_.broadcast("notice", js::Value(js::Object{{"session", vs.name}, {"level", "warn"},
                {"text", "execution for unknown order " + ev->cl_ord_id}}).dump());
            return;
        }
        OrderRecord& o = it->second;
        for (auto& w : ev->warnings) o.warnings.push_back(w);
        if (ev->type == ExecEvent::Type::CancelReject) {
            o.text = std::string(o.pending) + " rejected: " + ev->text;
            o.pending.clear();
            publish(o);
            return;
        }
        if (ev->kind == ExecKind::Replaced) {
            o.cl_ord_id = ev->cl_ord_id;
            if (!o.pending_qty.empty()) o.qty = o.pending_qty;
            if (!o.pending_price.empty()) o.price = o.pending_price;
        }
        if (ev->kind == ExecKind::Canceled || ev->kind == ExecKind::Replaced || ev->kind == ExecKind::Rejected)
            o.pending.clear();
        if (ev->status != OrderStatus::Unknown) o.status = ev->status;
        if (!ev->order_id.empty()) o.order_id = ev->order_id;
        if (!ev->cum_qty.empty()) o.cum_qty = ev->cum_qty;
        if (!ev->leaves_qty.empty()) o.leaves_qty = ev->leaves_qty;
        if (!ev->avg_px.empty()) o.avg_px = ev->avg_px;
        o.text = ev->text;
        publish(o);
    }

    // ---------- orders ----------
    std::string next_id() { return id_prefix_ + std::to_string(++id_counter_); }

    Result<std::string> place(VenueSession& vs, NewOrder o) {
        if (!active(vs)) return Result<std::string>::fail(vs.name + " is not logged on");
        auto msg = vs.adapter->build_new_order(o);
        if (!msg) return Result<std::string>::fail(msg.error());
        OrderRecord rec;
        rec.key = rec.cl_ord_id = o.cl_ord_id;
        rec.session = vs.name;
        rec.venue = std::string(vs.adapter->id());
        rec.symbol = o.symbol;
        rec.side = o.side;
        rec.qty = rec.leaves_qty = o.qty;
        rec.ord_type = o.ord_type;
        rec.price = o.price.value_or("");
        rec.tif = o.tif;
        rec.account = o.account.value_or("");
        rec.extra = o.extra;
        by_cl_ord_id_[rec.cl_ord_id] = rec.key;
        if (!send(vs, msg.value())) return Result<std::string>::fail(vs.name + ": send failed");
        auto& stored = orders_[rec.key] = std::move(rec);
        publish(stored);
        return stored.key;
    }

    void drain_queue() {
        if (queue_.empty()) return;
        auto now = Clock::now();
        double dt = std::chrono::duration<double>(now - last_drain_).count();
        last_drain_ = now;
        budget_ = std::min(budget_ + dt * rate_, std::max(1.0, rate_ / 10));
        while (!queue_.empty() && budget_ >= 1.0) {
            auto q = std::move(queue_.front());
            queue_.pop_front();
            budget_ -= 1.0;
            auto it = sessions_.find(q.session);
            if (it == sessions_.end()) continue;
            auto r = place(*it->second, std::move(q.order));
            if (!r) http_.broadcast("notice", js::Value(js::Object{{"session", q.session}, {"level", "error"}, {"text", r.error()}}).dump());
        }
    }

    // ---------- JSON ----------
    js::Value session_json(const VenueSession& vs) const {
        const auto& caps = vs.adapter->capabilities();
        auto strs = [](const auto& v) {
            js::Array a;
            for (const auto& x : v) a.push_back(std::string(to_string(x)));
            return a;
        };
        js::Array extras;
        for (const auto& e : caps.extras) {
            js::Array vals;
            for (const auto& [v, n] : e.values) vals.push_back(js::Object{{"value", v}, {"name", n}});
            extras.push_back(js::Object{{"name", e.name}, {"tag", e.tag}, {"required", e.required},
                                        {"values", vals}, {"description", e.description}});
        }
        return js::Object{
            {"name", vs.name},
            {"venue", std::string(vs.adapter->id())},
            {"venue_name", std::string(vs.adapter->name())},
            {"begin_string", vs.sid.getBeginString().getValue()},
            {"state", vs.state},
            {"endpoint", vs.host + ":" + std::to_string(vs.port)},
            {"last_error", vs.last_event},
            {"enabled", qf_session(vs) && qf_session(vs)->isEnabled()},
            {"capabilities", js::Object{
                {"sides", strs(caps.sides)}, {"ord_types", strs(caps.ord_types)}, {"tifs", strs(caps.tifs)},
                {"cancel", caps.cancel}, {"replace", caps.replace}, {"instrument_field", caps.instrument_field},
                {"has_account", caps.has_account}, {"account_required", caps.account_required}, {"extras", extras}}},
        };
    }

    static js::Value order_json(const OrderRecord& o) {
        js::Object extra;
        for (const auto& [k, v] : o.extra) extra[k] = v;
        js::Array warnings;
        for (const auto& w : o.warnings) warnings.push_back(w);
        return js::Object{{"key", o.key}, {"cl_ord_id", o.cl_ord_id}, {"session", o.session}, {"venue", o.venue},
                          {"symbol", o.symbol}, {"side", std::string(to_string(o.side))}, {"qty", o.qty},
                          {"ord_type", std::string(to_string(o.ord_type))}, {"price", o.price},
                          {"tif", o.tif ? std::string(to_string(*o.tif)) : ""}, {"account", o.account},
                          {"extra", extra}, {"status", std::string(to_string(o.status))}, {"cum_qty", o.cum_qty},
                          {"leaves_qty", o.leaves_qty}, {"avg_px", o.avg_px}, {"order_id", o.order_id},
                          {"text", o.text}, {"pending", o.pending}, {"warnings", warnings}};
    }

    void publish(const OrderRecord& o) { http_.broadcast("order", order_json(o).dump()); }

    void snapshot(const std::shared_ptr<net::TcpConnection>& c) {
        for (const auto& n : order_) c->send("event: session\ndata: " + session_json(*sessions_[n]).dump() + "\n\n");
        for (const auto& [k, o] : orders_) c->send("event: order\ndata: " + order_json(o).dump() + "\n\n");
        for (const auto& f : fix_log_) c->send("event: fix\ndata: " + f + "\n\n");
    }

    // ---------- HTTP API ----------
    Response handle(const Request& rq) {
        if (rq.method == "GET" && (rq.path == "/" || rq.path == "/index.html")) {
            bool ok = false;
            std::string html = read_file(ui_dir_ + "/index.html", &ok);
            if (!ok) return {404, "text/plain", "UI not found in " + ui_dir_};
            return {200, "text/html; charset=utf-8", html};
        }
        if (rq.method == "GET" && rq.path == "/api/sessions") {
            js::Array a;
            for (const auto& n : order_) a.push_back(session_json(*sessions_[n]));
            return json_response(200, a);
        }
        if (rq.method == "GET" && rq.path == "/api/orders") {
            js::Array a;
            for (const auto& [k, o] : orders_) a.push_back(order_json(o));
            return json_response(200, a);
        }
        static const std::regex sess_re(R"(^/api/sessions/([^/]+)/(connect|disconnect)$)");
        static const std::regex order_re(R"(^/api/orders/([^/]+)/(cancel|replace)$)");
        std::smatch m;
        if (rq.method == "POST" && std::regex_match(rq.path, m, sess_re)) {
            auto it = sessions_.find(m[1]);
            if (it == sessions_.end()) return error_response(404, "no such session");
            auto& vs = *it->second;
            auto* s = qf_session(vs);
            if (!s) return error_response(500, "session not created");
            if (m[2] == "connect") s->logon();       // re-enables; the initiator reconnects
            else s->logout("operator disconnect");  // disables until connect
            vs.state = state_of(vs);
            return json_response(200, session_json(vs));
        }

        std::string err;
        std::optional<js::Value> body;
        if (rq.method == "POST") {
            body = js::parse(rq.body.empty() ? "{}" : rq.body, &err);
            if (!body || !body->is_object()) return error_response(400, "invalid JSON body: " + err);
        }
        if (rq.method == "POST" && rq.path == "/api/orders") return new_orders(*body);
        if (rq.method == "POST" && std::regex_match(rq.path, m, order_re)) {
            auto it = orders_.find(m[1]);
            if (it == orders_.end()) return error_response(404, "no such order");
            return m[2] == "cancel" ? cancel(it->second) : replace(it->second, *body);
        }
        if (rq.path.rfind("/api/", 0) == 0) return error_response(404, "no such endpoint");
        return {404, "text/plain", "not found"};
    }

    Response new_orders(const js::Value& b) {
        auto sit = sessions_.find(b["session"].as_string());
        if (sit == sessions_.end()) return error_response(400, "unknown session '" + b["session"].as_string() + "'");
        auto& vs = *sit->second;

        NewOrder o;
        o.symbol = b["symbol"].as_string();
        o.qty = b["qty"].as_string();
        auto side = parse_side(b["side"].as_string());
        auto ot = parse_ord_type(b["ord_type"].as_string("limit"));
        if (!side) return error_response(400, "bad side");
        if (!ot) return error_response(400, "bad ord_type");
        o.side = *side;
        o.ord_type = *ot;
        if (auto p = b.get_string("price"); p && !p->empty()) o.price = *p;
        if (auto p = b.get_string("stop_price"); p && !p->empty()) o.stop_price = *p;
        if (auto t = b.get_string("tif"); t && !t->empty()) {
            auto tif = parse_tif(*t);
            if (!tif) return error_response(400, "bad tif");
            o.tif = *tif;
        }
        if (auto a = b.get_string("account"); a && !a->empty()) o.account = *a;
        for (const auto& [k, v] : b["extra"].as_object())
            if (!v.as_string().empty()) o.extra[k] = v.as_string();

        long long count = b.has("count") ? b["count"].as_int(1) : 1;
        if (count < 1 || count > 100000) return error_response(400, "count must be 1..100000");
        double rate = b.has("rate_per_sec") ? std::atof(b["rate_per_sec"].as_string().c_str()) : 0;

        // Dry-run the translation once so validation errors come back synchronously.
        o.cl_ord_id = "DRYRUN";
        if (auto dry = vs.adapter->build_new_order(o); !dry) return error_response(400, dry.error());
        if (!active(vs)) return error_response(409, vs.name + " is not logged on");

        js::Array ids;
        if (rate > 0 || count > 1000) {
            rate_ = rate > 0 ? rate : 5000;
            last_drain_ = Clock::now();
            for (long long i = 0; i < count; ++i) {
                o.cl_ord_id = next_id();
                ids.push_back(o.cl_ord_id);
                queue_.push_back({vs.name, o});
            }
            return json_response(201, js::Object{{"queued", ids}});
        }
        for (long long i = 0; i < count; ++i) {
            o.cl_ord_id = next_id();
            auto r = place(vs, o);
            if (!r) return error_response(400, r.error());
            ids.push_back(r.value());
        }
        return json_response(201, js::Object{{"orders", ids}});
    }

    Response cancel(OrderRecord& o) {
        auto& vs = *sessions_[o.session];
        if (!active(vs)) return error_response(409, vs.name + " is not logged on");
        CancelOrder c{next_id(), o.cl_ord_id, o.symbol, o.side, o.qty, {}};
        auto msg = vs.adapter->build_cancel(c);
        if (!msg) return error_response(400, msg.error());
        by_cl_ord_id_[c.cl_ord_id] = o.key;
        if (!send(vs, msg.value())) return error_response(409, vs.name + ": send failed");
        o.pending = "cancel";
        publish(o);
        return json_response(200, order_json(o));
    }

    Response replace(OrderRecord& o, const js::Value& b) {
        auto& vs = *sessions_[o.session];
        if (!active(vs)) return error_response(409, vs.name + " is not logged on");
        ReplaceOrder r;
        r.cl_ord_id = next_id();
        r.orig_cl_ord_id = o.cl_ord_id;
        r.symbol = o.symbol;
        r.side = o.side;
        r.qty = b.get_string("qty").value_or(o.qty);
        r.ord_type = o.ord_type;
        auto px = b.get_string("price").value_or(o.price);
        if (!px.empty()) r.price = px;
        r.tif = o.tif;
        if (!o.account.empty()) r.account = o.account;
        r.extra = o.extra;
        auto msg = vs.adapter->build_replace(r);
        if (!msg) return error_response(400, msg.error());
        by_cl_ord_id_[r.cl_ord_id] = o.key;
        if (!send(vs, msg.value())) return error_response(409, vs.name + ": send failed");
        o.pending = "replace";
        o.pending_qty = r.qty;
        o.pending_price = px;
        publish(o);
        return json_response(200, order_json(o));
    }

    net::Reactor& r_;
    gateway::HttpServer& http_;
    std::string ui_dir_;
    fixgen::qf::CallbackLogFactory log_factory_;
    FIX::SessionSettings settings_;
    std::unique_ptr<FIX::FileStoreFactory> store_;
    std::unique_ptr<FIX::SocketInitiator> initiator_;
    std::map<FIX::SessionID, VenueSession*> by_sid_;
    std::map<std::string, std::unique_ptr<VenueSession>> sessions_;
    std::vector<std::string> order_;  // session names in config order
    std::map<std::string, OrderRecord> orders_;
    std::map<std::string, std::string> by_cl_ord_id_;
    std::deque<std::string> fix_log_;
    std::deque<QueuedOrder> queue_;
    double rate_ = 0, budget_ = 0;
    Clock::time_point last_drain_{};
    std::string id_prefix_;
    long long id_counter_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/gateway.json";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == "--list-venues") {
            for (const auto& v : make_all_venues())
                std::printf("%s\t%s\t%s\n", std::string(v->id()).c_str(), std::string(v->name()).c_str(),
                            v->session_defaults().begin_string.c_str());
            return 0;
        } else {
            std::fprintf(stderr, "usage: fixgen-gateway [--config path] [--list-venues]\n");
            return 2;
        }
    }
    bool ok = false;
    std::string text = read_file(config_path, &ok);
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", config_path.c_str()); return 1; }
    std::string err;
    auto cfg = js::parse(text, &err);
    if (!cfg) { std::fprintf(stderr, "%s: %s\n", config_path.c_str(), err.c_str()); return 1; }

    net::Reactor reactor;
    const auto& http_cfg = (*cfg)["http"];
    std::string host = http_cfg["host"].as_string("127.0.0.1");
    int port = static_cast<int>(http_cfg["port"].as_int(8080));
    gateway::HttpServer http(reactor, host, port);
    if (!http.ok()) { std::fprintf(stderr, "http: %s\n", http.error().c_str()); return 1; }

    Gateway gw(reactor, http, http_cfg["ui_dir"].as_string("ui"));
    // QuickFIX FileStore keeps sequence numbers and sent messages across restarts.
    std::string store_dir = (*cfg)["store_dir"].as_string("var/fixstore");
    std::string dict_dir = (*cfg)["dictionary_dir"].as_string();
    for (const auto& s : (*cfg)["sessions"].as_array()) {
        if (!gw.add_session(s, dict_dir, store_dir, &err)) { std::fprintf(stderr, "config: %s\n", err.c_str()); return 1; }
    }
    try {
        gw.start();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "quickfix: %s\n", e.what());
        return 1;
    }
    std::printf("fixgen-gateway listening on http://%s:%d\n", host.c_str(), port);
    std::fflush(stdout);
    reactor.run();
}
