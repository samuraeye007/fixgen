#include "fixcore/net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fixcore::net {
namespace {
void set_err(std::string* e, const std::string& m) { if (e) *e = m; }
void set_nonblocking(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
struct IgnoreSigpipe { IgnoreSigpipe() { ::signal(SIGPIPE, SIG_IGN); } } ignore_sigpipe;
}  // namespace

void Reactor::add(int fd, Handler h) { fds_[fd] = Entry{std::move(h), false, ++gen_}; }
void Reactor::remove(int fd) { fds_.erase(fd); }
void Reactor::want_write(int fd, bool on) {
    if (auto it = fds_.find(fd); it != fds_.end()) it->second.write = on;
}
void Reactor::every(std::chrono::milliseconds period, std::function<void()> fn) {
    timers_.push_back({period, Clock::now() + period, std::move(fn)});
}

void Reactor::run_once(int timeout_ms) {
    std::vector<pollfd> pfds;
    std::vector<std::uint64_t> gens;
    for (auto& [fd, e] : fds_) {
        pfds.push_back({fd, static_cast<short>(POLLIN | (e.write ? POLLOUT : 0)), 0});
        gens.push_back(e.gen);
    }
    auto now0 = Clock::now();
    for (const auto& t : timers_) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.next - now0).count();
        if (ms < timeout_ms) timeout_ms = ms < 0 ? 0 : static_cast<int>(ms);
    }
    if (!posted_.empty()) timeout_ms = 0;
    ::poll(pfds.data(), pfds.size(), timeout_ms);
    for (std::size_t i = 0; i < pfds.size(); ++i) {
        if (!pfds[i].revents) continue;
        auto it = fds_.find(pfds[i].fd);
        if (it == fds_.end() || it->second.gen != gens[i]) continue;  // removed / replaced meanwhile
        auto h = it->second.h;  // copy: the handler may remove itself
        h(pfds[i].revents);
    }
    auto now = Clock::now();
    for (std::size_t i = 0; i < timers_.size(); ++i) {
        if (now >= timers_[i].next) {
            timers_[i].next = now + timers_[i].period;
            auto fn = timers_[i].fn;
            fn();
        }
    }
    auto posted = std::move(posted_);
    posted_.clear();
    for (auto& fn : posted) fn();
}

void Reactor::run() {
    running_ = true;
    while (running_) run_once();
}

std::shared_ptr<TcpConnection> TcpConnection::adopt(Reactor& r, int fd, std::string peer) {
    std::shared_ptr<TcpConnection> c(new TcpConnection(r, fd, std::move(peer)));
    std::weak_ptr<TcpConnection> weak = c;
    r.add(fd, [weak](short ev) {
        if (auto self = weak.lock()) self->on_events(ev);
    });
    return c;
}

TcpConnection::~TcpConnection() {
    if (fd_ >= 0) {
        r_.remove(fd_);
        ::close(fd_);
    }
}

void TcpConnection::send(std::string_view bytes) {
    if (fd_ < 0 || closing_) return;
    out_.append(bytes);
    flush();
}

void TcpConnection::flush() {
    while (!out_.empty() && fd_ >= 0) {
        ssize_t n = ::send(fd_, out_.data(), out_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out_.erase(0, n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            shutdown_now(std::string("send failed: ") + std::strerror(errno));
            return;
        }
    }
    if (fd_ < 0) return;
    r_.want_write(fd_, !out_.empty());
    if (out_.empty() && closing_) shutdown_now(close_reason_);
}

void TcpConnection::close(const std::string& reason, bool graceful) {
    if (fd_ < 0) return;
    closing_ = true;
    close_reason_ = reason;
    if (!graceful || out_.empty()) shutdown_now(reason);
}

void TcpConnection::shutdown_now(const std::string& reason) {
    if (fd_ < 0) return;
    r_.remove(fd_);
    ::close(fd_);
    fd_ = -1;
    auto self = shared_from_this();  // keep alive while callbacks run
    if (on_close) {
        auto cb = std::move(on_close);
        on_close = nullptr;
        r_.post([cb, reason, self] { cb(reason); });
    }
}

void TcpConnection::on_events(short ev) {
    auto self = shared_from_this();
    if (ev & POLLOUT) flush();
    if (fd_ < 0) return;
    if (ev & (POLLIN | POLLHUP | POLLERR)) {
        char buf[65536];
        while (fd_ >= 0) {
            ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
            if (n > 0) {
                if (on_data) on_data(std::string_view(buf, n));
            } else if (n == 0) {
                shutdown_now("peer closed");
                return;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                shutdown_now(std::string("recv failed: ") + std::strerror(errno));
                return;
            }
        }
    }
}

int tcp_listen(const std::string& host, int port, std::string* err) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { set_err(err, std::strerror(errno)); return -1; }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.empty() ? "0.0.0.0" : host.c_str(), &addr.sin_addr) != 1) {
        set_err(err, "bad listen address " + host);
        ::close(fd);
        return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || ::listen(fd, 64) < 0) {
        set_err(err, std::string("bind/listen: ") + std::strerror(errno));
        ::close(fd);
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

int tcp_accept(int listen_fd, std::string* peer) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    if (fd < 0) return -1;
    set_nonblocking(fd);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (peer) {
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
        *peer = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
    }
    return fd;
}

void close_fd(int fd) {
    if (fd >= 0) ::close(fd);
}

}  // namespace fixcore::net
