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

// "?" replaced by SOH, handy for tests and logs.
std::string soh(std::string_view pipe_separated, char sep = '|');
// UTC "YYYYMMDD-HH:MM:SS.sss".
std::string utc_timestamp();

}  // namespace fixcore
