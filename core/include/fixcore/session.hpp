#pragma once
// FIX session layer (FIX 4.x / FIXT.1.1): logon, heartbeats, test requests, sequence
// numbers, gap detection, resend and gap fill. Transport-agnostic: feed it bytes,
// it calls `send_bytes`. Single-threaded; drive tick() periodically.
#include <chrono>
#include <functional>
#include <map>
#include <string>

#include "fixcore/codec.hpp"
#include "fixcore/message.hpp"

namespace fixcore {

struct SessionConfig {
    std::string begin_string = "FIX.4.4";
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string default_appl_ver_id;  // FIXT.1.1
    int heartbeat_secs = 30;
    bool reset_seq_num_on_logon = true;
    bool initiator = true;
};

enum class SessionState { Disconnected, AwaitingLogon, LogonSent, Active, LogoutSent };
std::string_view to_string(SessionState);

class Session {
public:
    using Clock = std::chrono::steady_clock;

    Session(SessionConfig cfg, std::function<void(const std::string&)> send_bytes);

    // Callbacks (all optional).
    std::function<void(const Message&)> on_app_message;
    std::function<void(SessionState, const std::string& reason)> on_state;
    std::function<void(Message& logon)> decorate_logon;  // add venue logon fields
    // Acceptor: return empty string to accept the logon, or a reason to reject it.
    std::function<std::string(const Message& logon)> accept_logon;
    std::function<void(bool outgoing, const std::string& raw)> on_raw;
    std::function<void()> request_disconnect;  // ask the transport to close

    void connected();
    void disconnected(const std::string& reason = "transport closed");
    void on_bytes(std::string_view bytes);
    void tick(Clock::time_point now = Clock::now());

    // Send an application message. Returns false (and sends nothing) unless Active.
    bool send(Message msg);
    void logout(const std::string& text = {});

    SessionState state() const { return state_; }
    const SessionConfig& config() const { return cfg_; }
    int next_out_seq() const { return next_out_; }
    int next_in_seq() const { return next_in_; }

private:
    void handle(Message msg);
    void handle_admin(const Message& msg);
    void send_raw(Message msg, bool store);
    void send_logon(bool reply);
    void send_reject(const Message& ref, int reason, const std::string& text);
    void handle_resend_request(const Message& msg);
    void set_state(SessionState s, const std::string& reason);
    void fail(const std::string& reason);
    bool resend_pending() const { return resend_target_ > 0; }

    SessionConfig cfg_;
    std::function<void(const std::string&)> send_bytes_;
    FrameParser parser_;
    SessionState state_ = SessionState::Disconnected;
    int next_out_ = 1;
    int next_in_ = 1;
    int resend_target_ = 0;  // > 0 while waiting for a gap to be filled
    std::map<int, Message> sent_;  // app messages by seq, for resend
    Clock::time_point last_sent_{}, last_recv_{};
    bool test_request_outstanding_ = false;
    int test_req_counter_ = 0;
};

}  // namespace fixcore
