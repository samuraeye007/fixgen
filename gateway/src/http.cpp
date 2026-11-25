#include "http.hpp"

#include <algorithm>
#include <cctype>

namespace gateway {
using fixcore::net::TcpConnection;

std::string status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        default: return code >= 500 ? "Internal Server Error" : "Unknown";
    }
}

HttpServer::HttpServer(fixcore::net::Reactor& r, const std::string& host, int port) : r_(r) {
    listen_fd_ = fixcore::net::tcp_listen(host, port, &error_);
    if (listen_fd_ >= 0) r_.add(listen_fd_, [this](short) { accept_ready(); });
}

HttpServer::~HttpServer() {
    if (listen_fd_ >= 0) {
        r_.remove(listen_fd_);
        fixcore::net::close_fd(listen_fd_);
    }
}

void HttpServer::accept_ready() {
    while (true) {
        std::string peer;
        int fd = fixcore::net::tcp_accept(listen_fd_, &peer);
        if (fd < 0) return;
        auto c = TcpConnection::adopt(r_, fd, peer);
        TcpConnection* key = c.get();
        pending_[key] = {c, {}};
        std::weak_ptr<TcpConnection> weak = c;
        c->on_data = [this, weak](std::string_view d) {
            if (auto self = weak.lock()) on_data(self, d);
        };
        c->on_close = [this, key, weak](const std::string&) {
            pending_.erase(key);
            if (auto self = weak.lock()) sse_.erase(self);
        };
    }
}

void HttpServer::on_data(const std::shared_ptr<TcpConnection>& c, std::string_view d) {
    auto it = pending_.find(c.get());
    if (it == pending_.end()) return;  // SSE clients do not send anything we care about
    std::string& buf = it->second.second;
    buf.append(d);
    if (buf.size() > (1 << 20)) {
        c->send("HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        c->close();
        return;
    }
    auto hdr_end = buf.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return;

    Request req;
    auto line_end = buf.find("\r\n");
    std::string line = buf.substr(0, line_end);
    auto sp1 = line.find(' '), sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) { c->close("bad request", false); return; }
    req.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (auto q = target.find('?'); q != std::string::npos) {
        req.query = target.substr(q + 1);
        target.resize(q);
    }
    req.path = target;
    std::size_t pos = line_end + 2;
    while (pos < hdr_end) {
        auto e = buf.find("\r\n", pos);
        std::string h = buf.substr(pos, e - pos);
        if (auto colon = h.find(':'); colon != std::string::npos) {
            std::string name = h.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return std::tolower(ch); });
            std::string value = h.substr(colon + 1);
            value.erase(0, value.find_first_not_of(' '));
            req.headers[name] = value;
        }
        pos = e + 2;
    }
    std::size_t content_length = 0;
    if (auto cl = req.headers.find("content-length"); cl != req.headers.end())
        content_length = std::strtoul(cl->second.c_str(), nullptr, 10);
    if (buf.size() < hdr_end + 4 + content_length) return;  // wait for the body
    req.body = buf.substr(hdr_end + 4, content_length);
    pending_.erase(it);

    if (req.method == "GET" && req.path == sse_path) {
        c->send("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n\r\n: connected\n\n");
        sse_.insert(c);
        if (on_sse_open) on_sse_open(c);
        return;
    }
    Response resp = handler ? handler(req) : Response{404, "text/plain", "not found"};
    c->send("HTTP/1.1 " + std::to_string(resp.status) + " " + status_text(resp.status) +
            "\r\nContent-Type: " + resp.content_type + "\r\nContent-Length: " + std::to_string(resp.body.size()) +
            "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + resp.body);
    c->close();
}

void HttpServer::broadcast(const std::string& event, const std::string& data) {
    std::string frame = "event: " + event + "\ndata: " + data + "\n\n";
    for (const auto& c : sse_) c->send(frame);
}

}  // namespace gateway
