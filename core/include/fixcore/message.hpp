#pragma once
// Tag=value FIX message. Fields keep wire order, so repeating groups are simply
// consecutive fields added with add().
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fixcore {

namespace tags {
inline constexpr int BeginString = 8, BodyLength = 9, CheckSum = 10, MsgType = 35;
inline constexpr int MsgSeqNum = 34, SenderCompID = 49, TargetCompID = 56, SendingTime = 52;
inline constexpr int PossDupFlag = 43, OrigSendingTime = 122, PossResend = 97;
inline constexpr int BeginSeqNo = 7, EndSeqNo = 16, NewSeqNo = 36, GapFillFlag = 123;
inline constexpr int RefSeqNum = 45, Text = 58, EncryptMethod = 98, HeartBtInt = 108;
inline constexpr int TestReqID = 112, ResetSeqNumFlag = 141, SessionRejectReason = 373;
inline constexpr int RefTagID = 371, RefMsgType = 372, DefaultApplVerID = 1137;
}  // namespace tags

namespace msgtype {
inline constexpr std::string_view Heartbeat = "0", TestRequest = "1", ResendRequest = "2",
    Reject = "3", SequenceReset = "4", Logout = "5", Logon = "A";
}

bool is_header_tag(int tag);
bool is_admin_msg_type(std::string_view msg_type);

struct Field {
    int tag;
    std::string value;
};

class Message {
public:
    Message() = default;
    explicit Message(std::string_view msg_type) : msg_type_(msg_type) {}

    const std::string& msg_type() const { return msg_type_; }
    void set_msg_type(std::string_view t) { msg_type_ = t; }

    const std::string& begin_string() const { return begin_string_; }
    void set_begin_string(std::string_view b) { begin_string_ = b; }

    // Replace the first occurrence of tag, or append it. Header tags go to the header.
    Message& set(int tag, std::string_view value);
    // Always append (repeating groups).
    Message& add(int tag, std::string_view value);
    void remove(int tag);

    std::optional<std::string_view> get(int tag) const;
    std::string get_or(int tag, std::string_view dflt = {}) const;
    bool has(int tag) const { return get(tag).has_value(); }

    const std::vector<Field>& header() const { return header_; }
    const std::vector<Field>& body() const { return body_; }

    // Human-readable, '|' separated. Includes 8 and 35 but not 9/10.
    std::string to_string(char sep = '|') const;

private:
    std::vector<Field>& list_for(int tag) { return is_header_tag(tag) ? header_ : body_; }
    const std::vector<Field>& list_for(int tag) const { return is_header_tag(tag) ? header_ : body_; }

    std::string begin_string_;
    std::string msg_type_;
    std::vector<Field> header_;
    std::vector<Field> body_;
};

}  // namespace fixcore
