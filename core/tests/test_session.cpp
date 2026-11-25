#include <deque>

#include "fixcore/session.hpp"
#include "test.hpp"

using namespace fixcore;

namespace {
// Two sessions wired back to back. Bytes are queued (not delivered re-entrantly) and a
// drop filter lets tests lose specific messages on the wire.
struct Pair {
    std::deque<std::pair<bool, std::string>> wire;  // (to_acceptor, bytes)
    std::function<bool(bool to_acceptor, const Message&)> drop;
    std::vector<Message> at_acceptor, at_initiator;
    Session init, acc;

    static SessionConfig cfg(bool initiator) {
        SessionConfig c;
        c.begin_string = "FIX.4.4";
        c.sender_comp_id = initiator ? "CLIENT" : "VENUE";
        c.target_comp_id = initiator ? "VENUE" : "CLIENT";
        c.heartbeat_secs = 30;
        c.initiator = initiator;
        return c;
    }

    Pair()
        : init(cfg(true), [this](const std::string& b) { wire.emplace_back(true, b); }),
          acc(cfg(false), [this](const std::string& b) { wire.emplace_back(false, b); }) {
        init.on_app_message = [this](const Message& m) { at_initiator.push_back(m); };
        acc.on_app_message = [this](const Message& m) { at_acceptor.push_back(m); };
    }

    void pump() {
        while (!wire.empty()) {
            auto [to_acc, bytes] = wire.front();
            wire.pop_front();
            if (drop) {
                if (auto m = decode(bytes); m && drop(to_acc, *m)) continue;
            }
            (to_acc ? acc : init).on_bytes(bytes);
        }
    }

    void logon() {
        acc.connected();
        init.connected();
        pump();
    }
};

Message order(const std::string& id) {
    Message m("D");
    m.set(11, id);
    return m;
}
}  // namespace

TEST(session_logon_and_app_message) {
    Pair p;
    p.init.decorate_logon = [](Message& m) { m.set(553, "user"); };
    std::string seen_user;
    p.acc.accept_logon = [&](const Message& m) { seen_user = m.get_or(553); return std::string(); };
    p.logon();
    CHECK(p.init.state() == SessionState::Active);
    CHECK(p.acc.state() == SessionState::Active);
    CHECK_EQ(seen_user, std::string("user"));
    CHECK(p.init.send(order("A1")));
    p.pump();
    CHECK_EQ(p.at_acceptor.size(), 1u);
    CHECK_EQ(p.at_acceptor[0].get_or(11), std::string("A1"));
    CHECK_EQ(p.acc.next_in_seq(), 3);
}

TEST(session_rejects_logon) {
    Pair p;
    p.acc.accept_logon = [](const Message&) { return std::string("bad password"); };
    p.logon();
    CHECK(p.acc.state() == SessionState::Disconnected);
    CHECK(p.init.state() == SessionState::Disconnected);
}

TEST(session_gap_triggers_resend_with_possdup) {
    Pair p;
    p.logon();
    bool dropped = false;
    p.drop = [&](bool to_acc, const Message& m) {
        if (to_acc && m.get_or(11) == "A1" && !dropped) return dropped = true;
        return false;
    };
    p.init.send(order("A1"));  // lost on the wire
    p.init.send(order("A2"));  // arrives with a gap -> ResendRequest
    p.pump();
    CHECK(dropped);
    CHECK_EQ(p.at_acceptor.size(), 2u);
    if (p.at_acceptor.size() == 2) {
        CHECK_EQ(p.at_acceptor[0].get_or(11), std::string("A1"));
        CHECK_EQ(p.at_acceptor[0].get_or(43), std::string("Y"));
        CHECK_EQ(p.at_acceptor[1].get_or(11), std::string("A2"));
    }
    CHECK_EQ(p.acc.next_in_seq(), p.init.next_out_seq());
}

TEST(session_gap_fills_admin_messages) {
    Pair p;
    p.logon();
    // Lose a heartbeat from the acceptor; it must be gap-filled, not resent.
    p.drop = [&](bool to_acc, const Message& m) { return !to_acc && m.msg_type() == "0"; };
    p.acc.tick(Session::Clock::now() + std::chrono::seconds(31));
    p.pump();
    p.drop = nullptr;
    Message er("8");
    er.set(11, "A1");
    p.acc.send(er);
    p.pump();
    CHECK_EQ(p.at_initiator.size(), 1u);
    CHECK_EQ(p.init.next_in_seq(), p.acc.next_out_seq());
}

TEST(session_heartbeat_and_test_request) {
    Pair p;
    p.logon();
    auto now = Session::Clock::now();
    p.init.tick(now + std::chrono::seconds(32));  // silent counterparty -> TestRequest (+ heartbeat)
    bool saw_test_request = false;
    for (auto& [to_acc, b] : p.wire)
        if (auto m = decode(b); m && m->msg_type() == "1") saw_test_request = true;
    CHECK(saw_test_request);
    p.pump();  // acceptor answers with Heartbeat(112)
    CHECK(p.init.state() == SessionState::Active);
    p.init.tick(now + std::chrono::seconds(200));
    CHECK(p.init.state() == SessionState::Disconnected);  // no traffic for > 2 intervals
}

TEST(session_seq_too_low_logs_out) {
    Pair p;
    p.logon();
    Message m("D");
    m.set(tags::SenderCompID, "CLIENT").set(tags::TargetCompID, "VENUE").set(tags::MsgSeqNum, "1");
    m.set(tags::SendingTime, utc_timestamp());
    p.acc.on_bytes(encode(m, "FIX.4.4"));
    CHECK(p.acc.state() == SessionState::Disconnected);
}
