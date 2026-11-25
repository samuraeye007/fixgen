#include "fixcore/codec.hpp"
#include "test.hpp"

using namespace fixcore;

TEST(encode_sets_length_and_checksum) {
    Message m("D");
    m.set(tags::SenderCompID, "A").set(tags::TargetCompID, "B").set(tags::MsgSeqNum, "1");
    m.set(11, "X1").set(54, "1");
    std::string raw = encode(m, "FIX.4.4");
    // body = "35=D|49=A|56=B|34=1|11=X1|54=1|" = 31 bytes
    CHECK(raw.rfind(soh("8=FIX.4.4|9=31|35=D|49=A|56=B|34=1|11=X1|54=1|"), 0) == 0);
    auto back = decode(raw);
    CHECK(back.has_value());
    CHECK_EQ(back->msg_type(), std::string("D"));
    CHECK_EQ(back->get_or(11), std::string("X1"));
    CHECK_EQ(back->get_or(49), std::string("A"));
}

TEST(decode_rejects_bad_checksum_and_length) {
    Message m("0");
    std::string raw = encode(m, "FIX.4.4");
    std::string bad = raw;
    bad[bad.size() - 2] = bad[bad.size() - 2] == '0' ? '1' : '0';
    std::string err;
    CHECK(!decode(bad, &err));
    CHECK(err.find("checksum") != std::string::npos);
    CHECK(!decode(soh("8=FIX.4.4|9=99|35=0|10=000|"), &err));
}

TEST(repeating_groups_keep_order) {
    Message m("D");
    m.set(453, "2").add(448, "P1").add(452, "3").add(448, "P2").add(452, "12");
    auto back = decode(encode(m, "FIX.4.4"));
    CHECK(back.has_value());
    CHECK_EQ(back->body().size(), 5u);
    CHECK_EQ(back->body()[3].value, std::string("P2"));
}
