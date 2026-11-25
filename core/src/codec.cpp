#include "fixcore/codec.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace fixcore {
namespace {

void append_field(std::string& out, int tag, std::string_view value) {
    out += std::to_string(tag);
    out += '=';
    out += value;
    out += SOH;
}

unsigned checksum(std::string_view s) {
    unsigned sum = 0;
    for (unsigned char c : s) sum += c;
    return sum % 256;
}

void set_error(std::string* e, std::string msg) {
    if (e) *e = std::move(msg);
}

}  // namespace

std::string encode(const Message& msg, std::string_view begin_string) {
    std::string body;
    append_field(body, tags::MsgType, msg.msg_type());
    // Standard header order: 49, 56, 34, 52 first, then any other header fields.
    for (int t : {tags::SenderCompID, tags::TargetCompID, tags::MsgSeqNum, tags::SendingTime})
        if (auto v = msg.get(t)) append_field(body, t, *v);
    for (const auto& f : msg.header())
        if (f.tag != tags::SenderCompID && f.tag != tags::TargetCompID && f.tag != tags::MsgSeqNum &&
            f.tag != tags::SendingTime)
            append_field(body, f.tag, f.value);
    for (const auto& f : msg.body()) append_field(body, f.tag, f.value);

    std::string out;
    append_field(out, tags::BeginString, begin_string.empty() ? msg.begin_string() : begin_string);
    append_field(out, tags::BodyLength, std::to_string(body.size()));
    out += body;
    char cs[4];
    std::snprintf(cs, sizeof cs, "%03u", checksum(out));
    append_field(out, tags::CheckSum, cs);
    return out;
}

std::optional<Message> decode(std::string_view frame, std::string* error) {
    Message msg;
    std::size_t pos = 0;
    int index = 0;
    std::size_t body_start = 0, checksum_start = 0;
    long body_len = -1;
    bool have_checksum = false;
    while (pos < frame.size()) {
        auto eq = frame.find('=', pos);
        auto end = frame.find(SOH, pos);
        if (eq == std::string_view::npos || end == std::string_view::npos || eq > end) {
            set_error(error, "malformed field at offset " + std::to_string(pos));
            return std::nullopt;
        }
        int tag = 0;
        auto [p, ec] = std::from_chars(frame.data() + pos, frame.data() + eq, tag);
        if (ec != std::errc() || p != frame.data() + eq || tag <= 0) {
            set_error(error, "bad tag at offset " + std::to_string(pos));
            return std::nullopt;
        }
        std::string_view value = frame.substr(eq + 1, end - eq - 1);
        if ((index == 0 && tag != tags::BeginString) || (index == 1 && tag != tags::BodyLength) ||
            (index == 2 && tag != tags::MsgType)) {
            set_error(error, "header must start with 8, 9, 35");
            return std::nullopt;
        }
        if (tag == tags::BodyLength) {
            body_len = std::strtol(std::string(value).c_str(), nullptr, 10);
            body_start = end + 1;
        } else if (tag == tags::CheckSum) {
            checksum_start = pos;
            unsigned expected = checksum(frame.substr(0, checksum_start));
            if (std::strtoul(std::string(value).c_str(), nullptr, 10) != expected) {
                set_error(error, "checksum mismatch, expected " + std::to_string(expected));
                return std::nullopt;
            }
            have_checksum = true;
            pos = end + 1;
            break;
        } else if (tag == tags::BeginString || tag == tags::MsgType) {
            msg.set(tag, value);
        } else {
            msg.add(tag, value);
        }
        pos = end + 1;
        ++index;
    }
    if (!have_checksum) {
        set_error(error, "missing checksum");
        return std::nullopt;
    }
    if (body_len < 0 || static_cast<std::size_t>(body_len) != checksum_start - body_start) {
        set_error(error, "body length mismatch");
        return std::nullopt;
    }
    return msg;
}

std::string soh(std::string_view s, char sep) {
    std::string out(s);
    for (auto& c : out)
        if (c == sep) c = SOH;
    return out;
}

std::string utc_timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

}  // namespace fixcore
