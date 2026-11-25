#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "fixcore/message.hpp"

namespace fixcore {

inline constexpr char SOH = '\x01';

// Serialize with correct BodyLength(9) and CheckSum(10). Uses msg.begin_string() unless
// begin_string is given.
std::string encode(const Message& msg, std::string_view begin_string = {});

// Parse one complete frame. Verifies BodyLength and CheckSum.
std::optional<Message> decode(std::string_view frame, std::string* error = nullptr);

// Accumulates a byte stream and yields complete frames.
class FrameParser {
public:
    void feed(std::string_view bytes) { buf_.append(bytes); }
    // Returns the next complete raw frame, if any. Garbage before "8=" is discarded.
    // On a malformed frame returns an empty string and sets *error.
    std::optional<std::string> next_frame(std::string* error = nullptr);
    std::size_t buffered() const { return buf_.size(); }

private:
    std::string buf_;
};

// "?" replaced by SOH, handy for tests and logs.
std::string soh(std::string_view pipe_separated, char sep = '|');
// UTC "YYYYMMDD-HH:MM:SS.sss".
std::string utc_timestamp();

}  // namespace fixcore
