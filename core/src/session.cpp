#include "fixcore/session.hpp"

namespace fixcore {

std::string_view to_string(SessionState s) {
    switch (s) {
        case SessionState::Disconnected: return "disconnected";
        case SessionState::AwaitingLogon: return "awaiting_logon";
        case SessionState::LogonSent: return "logon_sent";
        case SessionState::Active: return "active";
        case SessionState::LogoutSent: return "logout_sent";
    }
    return "?";
}

namespace {
int to_int(std::optional<std::string_view> v) {
    return v ? std::atoi(std::string(*v).c_str()) : 0;
}
}  // namespace

Session::Session(SessionConfig cfg, std::function<void(const std::string&)> send_bytes)
    : cfg_(std::move(cfg)), send_bytes_(std::move(send_bytes)) {}

void Session::set_state(SessionState s, const std::string& reason) {
    if (s == state_) return;
    state_ = s;
    if (on_state) on_state(s, reason);
}

void Session::fail(const std::string& reason) {
    if (state_ == SessionState::Active || state_ == SessionState::LogonSent ||
        state_ == SessionState::AwaitingLogon) {
        Message lo(msgtype::Logout);
        lo.set(tags::Text, reason);
        send_raw(std::move(lo), false);
    }
    set_state(SessionState::Disconnected, reason);
    if (request_disconnect) request_disconnect();
}

void Session::connected() {
    parser_ = FrameParser();
    resend_target_ = 0;
    test_request_outstanding_ = false;
    if (cfg_.reset_seq_num_on_logon) {
        next_out_ = next_in_ = 1;
        sent_.clear();
    }
    last_recv_ = Clock::now();
    if (cfg_.initiator) {
        send_logon(false);
        set_state(SessionState::LogonSent, "logon sent");
    } else {
        set_state(SessionState::AwaitingLogon, "connection accepted");
    }
}

void Session::disconnected(const std::string& reason) { set_state(SessionState::Disconnected, reason); }

void Session::send_logon(bool reply) {
    Message logon(msgtype::Logon);
    logon.set(tags::EncryptMethod, "0");
    logon.set(tags::HeartBtInt, std::to_string(cfg_.heartbeat_secs));
    if (cfg_.reset_seq_num_on_logon) logon.set(tags::ResetSeqNumFlag, "Y");
    if (!cfg_.default_appl_ver_id.empty()) logon.set(tags::DefaultApplVerID, cfg_.default_appl_ver_id);
    if (!reply && decorate_logon) decorate_logon(logon);
    send_raw(std::move(logon), false);
}

void Session::send_raw(Message msg, bool store) {
    int seq = next_out_++;
    msg.set(tags::SenderCompID, cfg_.sender_comp_id);
    msg.set(tags::TargetCompID, cfg_.target_comp_id);
    msg.set(tags::MsgSeqNum, std::to_string(seq));
    msg.set(tags::SendingTime, utc_timestamp());
    std::string raw = encode(msg, cfg_.begin_string);
    if (store) sent_[seq] = std::move(msg);
    last_sent_ = Clock::now();
    if (on_raw) on_raw(true, raw);
    send_bytes_(raw);
}

bool Session::send(Message msg) {
    if (state_ != SessionState::Active) return false;
    send_raw(std::move(msg), true);
    return true;
}

void Session::logout(const std::string& text) {
    if (state_ != SessionState::Active) return;
    Message lo(msgtype::Logout);
    if (!text.empty()) lo.set(tags::Text, text);
    send_raw(std::move(lo), false);
    set_state(SessionState::LogoutSent, "logout sent");
}

void Session::send_reject(const Message& ref, int reason, const std::string& text) {
    Message rj(msgtype::Reject);
    rj.set(tags::RefSeqNum, ref.get_or(tags::MsgSeqNum, "0"));
    rj.set(tags::RefMsgType, ref.msg_type());
    rj.set(tags::SessionRejectReason, std::to_string(reason));
    rj.set(tags::Text, text);
    send_raw(std::move(rj), false);
}

void Session::on_bytes(std::string_view bytes) {
    parser_.feed(bytes);
    while (state_ != SessionState::Disconnected) {
        std::string err;
        auto frame = parser_.next_frame(&err);
        if (!frame) break;
        if (frame->empty()) {  // garbled framing: per spec, ignore and keep going
            if (on_raw) on_raw(false, "<garbled: " + err + ">");
            continue;
        }
        if (on_raw) on_raw(false, *frame);
        auto msg = decode(*frame, &err);
        if (!msg) continue;  // bad checksum etc.: ignored, the gap will trigger a resend
        last_recv_ = Clock::now();
        test_request_outstanding_ = false;
        handle(std::move(*msg));
    }
}

void Session::handle(Message msg) {
    // An acceptor configured without a TargetCompID adopts whoever logs on first.
    if (!cfg_.initiator && cfg_.target_comp_id.empty() && msg.msg_type() == msgtype::Logon)
        cfg_.target_comp_id = msg.get_or(tags::SenderCompID);
    if (msg.begin_string() != cfg_.begin_string) return fail("wrong BeginString " + msg.begin_string());
    if (msg.get_or(tags::SenderCompID) != cfg_.target_comp_id ||
        msg.get_or(tags::TargetCompID) != cfg_.sender_comp_id)
        return fail("CompID mismatch");

    const bool is_logon = msg.msg_type() == msgtype::Logon;
    const bool awaiting = state_ == SessionState::AwaitingLogon || state_ == SessionState::LogonSent;
    if (awaiting && !is_logon) return fail("first message was not a Logon");

    if (is_logon) {
        if (!awaiting) return send_reject(msg, 0, "unexpected Logon");
        if (state_ == SessionState::AwaitingLogon) {
            if (accept_logon) {
                if (auto why = accept_logon(msg); !why.empty()) return fail(why);
            }
            if (msg.get_or(tags::ResetSeqNumFlag) == "Y") {
                next_in_ = next_out_ = 1;
                sent_.clear();
            }
            if (auto hb = to_int(msg.get(tags::HeartBtInt)); hb > 0) cfg_.heartbeat_secs = hb;
        } else if (msg.get_or(tags::ResetSeqNumFlag) == "Y") {
            next_in_ = 1;
        }
    }

    const int seq = to_int(msg.get(tags::MsgSeqNum));
    const bool poss_dup = msg.get_or(tags::PossDupFlag) == "Y";
    const bool seq_reset = msg.msg_type() == msgtype::SequenceReset;
    const bool gap_fill = seq_reset && msg.get_or(tags::GapFillFlag) == "Y";

    if (seq_reset && !gap_fill) {  // reset mode ignores MsgSeqNum
        int ns = to_int(msg.get(tags::NewSeqNo));
        if (ns < next_in_) return send_reject(msg, 5, "NewSeqNo lower than expected");
        next_in_ = ns;
        if (resend_pending() && next_in_ > resend_target_) resend_target_ = 0;
        return;
    }

    if (seq > next_in_) {
        if (is_logon) {  // logon itself is accepted, then the gap is requested
            if (state_ == SessionState::AwaitingLogon) send_logon(true);
            set_state(SessionState::Active, "logged on");
        }
        if (msg.msg_type() == msgtype::ResendRequest) handle_resend_request(msg);
        if (!resend_pending()) {
            Message rr(msgtype::ResendRequest);
            rr.set(tags::BeginSeqNo, std::to_string(next_in_));
            rr.set(tags::EndSeqNo, "0");
            send_raw(std::move(rr), false);
            resend_target_ = seq;
        }
        return;
    }
    if (seq < next_in_) {
        if (poss_dup) return;  // duplicate from a resend
        return fail("MsgSeqNum too low, expected " + std::to_string(next_in_) + " got " + std::to_string(seq));
    }

    ++next_in_;
    if (gap_fill) {
        int ns = to_int(msg.get(tags::NewSeqNo));
        if (ns > next_in_) next_in_ = ns;
    }
    if (resend_pending() && next_in_ > resend_target_) resend_target_ = 0;
    if (gap_fill) return;

    if (is_admin_msg_type(msg.msg_type()) && msg.msg_type() != msgtype::Reject) {
        handle_admin(msg);
        return;
    }
    if (on_app_message) on_app_message(msg);
}

void Session::handle_admin(const Message& msg) {
    const auto& t = msg.msg_type();
    if (t == msgtype::Logon) {
        if (state_ == SessionState::AwaitingLogon) send_logon(true);
        set_state(SessionState::Active, "logged on");
    } else if (t == msgtype::TestRequest) {
        Message hb(msgtype::Heartbeat);
        hb.set(tags::TestReqID, msg.get_or(tags::TestReqID));
        send_raw(std::move(hb), false);
    } else if (t == msgtype::ResendRequest) {
        handle_resend_request(msg);
    } else if (t == msgtype::Logout) {
        if (state_ != SessionState::LogoutSent) {
            Message lo(msgtype::Logout);
            send_raw(std::move(lo), false);
        }
        set_state(SessionState::Disconnected, "logout: " + msg.get_or(tags::Text));
        if (request_disconnect) request_disconnect();
    }
    // Heartbeat: nothing beyond the receive-time bookkeeping already done.
}

void Session::handle_resend_request(const Message& msg) {
    int begin = to_int(msg.get(tags::BeginSeqNo));
    int end = to_int(msg.get(tags::EndSeqNo));
    int last = next_out_ - 1;
    if (end == 0 || end > last) end = last;
    if (begin < 1) begin = 1;

    auto emit = [&](Message m, int seq) {
        m.set(tags::SenderCompID, cfg_.sender_comp_id);
        m.set(tags::TargetCompID, cfg_.target_comp_id);
        m.set(tags::MsgSeqNum, std::to_string(seq));
        m.set(tags::PossDupFlag, "Y");
        m.set(tags::SendingTime, utc_timestamp());
        std::string raw = encode(m, cfg_.begin_string);
        last_sent_ = Clock::now();
        if (on_raw) on_raw(true, raw);
        send_bytes_(raw);
    };
    auto gap_fill = [&](int from, int to_next) {
        Message gf(msgtype::SequenceReset);
        gf.set(tags::GapFillFlag, "Y");
        gf.set(tags::NewSeqNo, std::to_string(to_next));
        emit(std::move(gf), from);
    };

    int gap_start = 0;
    for (int s = begin; s <= end; ++s) {
        auto it = sent_.find(s);
        if (it == sent_.end()) {
            if (!gap_start) gap_start = s;
            continue;
        }
        if (gap_start) { gap_fill(gap_start, s); gap_start = 0; }
        Message m = it->second;
        m.set(tags::OrigSendingTime, m.get_or(tags::SendingTime));
        emit(std::move(m), s);
    }
    if (gap_start) gap_fill(gap_start, end + 1);
}

void Session::tick(Clock::time_point now) {
    using std::chrono::seconds;
    if (state_ == SessionState::Disconnected) return;
    const auto hb = seconds(cfg_.heartbeat_secs);
    if (state_ == SessionState::LogonSent || state_ == SessionState::AwaitingLogon) {
        if (now - last_recv_ > seconds(10) && now - last_sent_ > seconds(10)) fail("logon timeout");
        return;
    }
    if (now - last_recv_ > 2 * hb + seconds(2)) return fail("heartbeat timeout");
    if (now - last_recv_ > hb + seconds(1) && !test_request_outstanding_) {
        Message tr(msgtype::TestRequest);
        tr.set(tags::TestReqID, "TEST" + std::to_string(++test_req_counter_));
        send_raw(std::move(tr), false);
        test_request_outstanding_ = true;
    }
    if (now - last_sent_ >= hb) send_raw(Message(msgtype::Heartbeat), false);
}

}  // namespace fixcore
