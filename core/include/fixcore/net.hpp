#pragma once
// Minimal single-threaded poll() reactor and non-blocking TCP connections (POSIX).
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fixcore::net {

class Reactor {
public:
    using Handler = std::function<void(short revents)>;
    using Clock = std::chrono::steady_clock;

    void add(int fd, Handler h);
    void remove(int fd);
    void want_write(int fd, bool on);
    void every(std::chrono::milliseconds period, std::function<void()> fn);
    void post(std::function<void()> fn) { posted_.push_back(std::move(fn)); }

    void run_once(int timeout_ms = 50);
    void run();
    void stop() { running_ = false; }

private:
    struct Entry { Handler h; bool write = false; std::uint64_t gen = 0; };
    struct Timer { std::chrono::milliseconds period; Clock::time_point next; std::function<void()> fn; };
    std::map<int, Entry> fds_;
    std::vector<Timer> timers_;
    std::vector<std::function<void()>> posted_;
    std::uint64_t gen_ = 0;
    bool running_ = false;
};

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    static std::shared_ptr<TcpConnection> adopt(Reactor& r, int fd, std::string peer = {});
    ~TcpConnection();

    std::function<void(std::string_view)> on_data;
    std::function<void(const std::string& reason)> on_close;

    void send(std::string_view bytes);
    // Close after the write buffer drains (graceful) or immediately.
    void close(const std::string& reason = "closed", bool graceful = true);
    bool open() const { return fd_ >= 0; }
    const std::string& peer() const { return peer_; }

private:
    TcpConnection(Reactor& r, int fd, std::string peer) : r_(r), fd_(fd), peer_(std::move(peer)) {}
    void on_events(short revents);
    void flush();
    void shutdown_now(const std::string& reason);

    Reactor& r_;
    int fd_;
    std::string peer_;
    std::string out_;
    bool closing_ = false;
    std::string close_reason_;
};

// Returns a listening fd or -1 (err set).
int tcp_listen(const std::string& host, int port, std::string* err = nullptr);
// Blocking connect with timeout; returns a non-blocking fd or -1.
int tcp_connect(const std::string& host, int port, int timeout_ms, std::string* err = nullptr);
// Accept one pending connection (non-blocking fd) or -1.
int tcp_accept(int listen_fd, std::string* peer = nullptr);
void close_fd(int fd);

}  // namespace fixcore::net
