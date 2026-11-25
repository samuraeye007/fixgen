#include "fixcore/message.hpp"

#include <algorithm>

namespace fixcore {

bool is_header_tag(int tag) {
    switch (tag) {
        case 8: case 9: case 35: case 34: case 43: case 49: case 50: case 52: case 56: case 57:
        case 97: case 115: case 116: case 122: case 128: case 129: case 142: case 143: case 144:
        case 145: case 369: case 627: case 1128: case 1129: case 1156:
            return true;
        default:
            return false;
    }
}

bool is_admin_msg_type(std::string_view t) {
    return t == msgtype::Heartbeat || t == msgtype::TestRequest || t == msgtype::ResendRequest ||
           t == msgtype::Reject || t == msgtype::SequenceReset || t == msgtype::Logout ||
           t == msgtype::Logon;
}

Message& Message::set(int tag, std::string_view value) {
    if (tag == tags::MsgType) { msg_type_ = value; return *this; }
    if (tag == tags::BeginString) { begin_string_ = value; return *this; }
    auto& list = list_for(tag);
    auto it = std::find_if(list.begin(), list.end(), [&](const Field& f) { return f.tag == tag; });
    if (it != list.end()) it->value = value;
    else list.push_back({tag, std::string(value)});
    return *this;
}

Message& Message::add(int tag, std::string_view value) {
    list_for(tag).push_back({tag, std::string(value)});
    return *this;
}

void Message::remove(int tag) {
    auto& list = list_for(tag);
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Field& f) { return f.tag == tag; }),
               list.end());
}

std::optional<std::string_view> Message::get(int tag) const {
    if (tag == tags::MsgType) return msg_type_.empty() ? std::nullopt : std::optional<std::string_view>(msg_type_);
    if (tag == tags::BeginString) return begin_string_.empty() ? std::nullopt : std::optional<std::string_view>(begin_string_);
    for (const auto& f : list_for(tag))
        if (f.tag == tag) return f.value;
    return std::nullopt;
}

std::string Message::get_or(int tag, std::string_view dflt) const {
    auto v = get(tag);
    return std::string(v ? *v : dflt);
}

std::string Message::to_string(char sep) const {
    std::string out;
    if (!begin_string_.empty()) out += "8=" + begin_string_ + sep;
    out += "35=" + msg_type_ + sep;
    for (const auto* list : {&header_, &body_})
        for (const auto& f : *list) out += std::to_string(f.tag) + "=" + f.value + sep;
    return out;
}

}  // namespace fixcore
