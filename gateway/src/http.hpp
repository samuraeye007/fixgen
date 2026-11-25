#pragma once
// Tiny HTTP/1.1 server on the fixcore reactor: one request per connection
// (Connection: close) plus long-lived Server-Sent Events streams.
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "fixcore/net.hpp"

namespace gateway {

struct Request {
    std::string method, path, query, body;
    std::map<std::string, std::string> headers;  // lower-case names
};

struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

class HttpServer {
public:
    HttpServer(fixcore::net::Reactor& r, const std::string& host, int port);
    ~HttpServer();
    bool ok() const { return listen_fd_ >= 0; }
    const std::string& error() const { return error_; }

    std::function<Response(const Request&)> handler;
    std::string sse_path = "/api/events";
    std::function<void(const std::shared_ptr<fixcore::net::TcpConnection>&)> on_sse_open;

    // Send one event to every connected SSE client.
    void broadcast(const std::string& event, const std::string& data);
    std::size_t sse_clients() const { return sse_.size(); }

private:
    void accept_ready();
    void on_data(const std::shared_ptr<fixcore::net::TcpConnection>& c, std::string_view d);

    fixcore::net::Reactor& r_;
    int listen_fd_ = -1;
    std::string error_;
    std::map<fixcore::net::TcpConnection*, std::pair<std::shared_ptr<fixcore::net::TcpConnection>, std::string>> pending_;
    std::set<std::shared_ptr<fixcore::net::TcpConnection>> sse_;
};

std::string status_text(int code);

}  // namespace gateway
