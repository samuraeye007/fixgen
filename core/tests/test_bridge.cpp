#include "bridge.hpp"
#include "fixcore/codec.hpp"
#include "fixgen/venues/acme_mtf.hpp"
#include "fixgen/venues/demo_exch.hpp"
#include "quickfix/DataDictionary.h"
#include "test.hpp"

using namespace fixcore;

namespace {
NewOrder order() {
    NewOrder o;
    o.cl_ord_id = "C1";
    o.symbol = "VOD";
    o.qty = "100";
    o.price = "101.25";
    o.account = "A1";
    return o;
}
}  // namespace

TEST(bridge_round_trip_through_quickfix_wire_format) {
    auto v = fixgen::venues::demo_exch::make_adapter();
    auto m = v->build_new_order(order());
    CHECK(static_cast<bool>(m));
    FIX::Message q = fixgen::qf::to_quickfix(m.value());
    q.getHeader().setField(FIX::FIELD::BeginString, "FIX.4.4");
    q.getHeader().setField(FIX::FIELD::SenderCompID, "A");
    q.getHeader().setField(FIX::FIELD::TargetCompID, "B");
    q.getHeader().setField(FIX::FIELD::MsgSeqNum, "7");
    q.getHeader().setField(FIX::FIELD::SendingTime, utc_timestamp());
    std::string wire = q.toString();

    // QuickFIX's output must be valid FIX by our own codec too.
    auto ours = decode(wire);
    CHECK(ours.has_value());

    FIX::Message parsed(wire, false);
    auto back = fixgen::qf::from_quickfix(parsed);
    CHECK_EQ(back.msg_type(), std::string("D"));
    CHECK_EQ(back.get_or(44), std::string("101.25"));
    CHECK_EQ(back.get_or(49), std::string("A"));
    CHECK(v->dictionary().validate(back).empty());
}

TEST(generated_quickfix_dictionaries_load_and_are_lenient) {
    std::string dir = fixgen::qf::default_dict_dir();
    for (const char* id : {"demo_exch", "acme_mtf"}) {
        try {
            FIX::DataDictionary dd(dir + "/" + id + ".xml");
            CHECK(dd.isMsgType("D"));
            CHECK(dd.isMsgType("8"));
        } catch (const std::exception& e) {
            std::cerr << id << ": " << e.what() << "\n";
            CHECK(false);
        }
    }
    FIX::DataDictionary transport(dir + "/acme_mtf.transport.xml");
    CHECK(transport.isMsgType("A"));
    CHECK(transport.isField(20010));  // venue logon field

    // An ExecutionReport with an ExecType value the spec does not list must still parse
    // and validate in QuickFIX (fixcore flags it instead of the session dropping it).
    FIX::DataDictionary dd(dir + "/demo_exch.xml");
    FIX::Message er;
    er.getHeader().setField(FIX::FIELD::BeginString, "FIX.4.4");
    er.getHeader().setField(FIX::FIELD::MsgType, "8");
    er.getHeader().setField(FIX::FIELD::SenderCompID, "V");
    er.getHeader().setField(FIX::FIELD::TargetCompID, "C");
    er.getHeader().setField(FIX::FIELD::MsgSeqNum, "2");
    er.getHeader().setField(FIX::FIELD::SendingTime, utc_timestamp());
    er.setField(150, "Z");
    er.setField(39, "0");
    er.setField(11, "C1");
    bool ok = true;
    try {
        FIX::Message parsed(er.toString(), dd, true);  // parse + validate, as the session does
    } catch (const std::exception& e) {
        std::cerr << "quickfix rejected: " << e.what() << "\n";
        ok = false;
    }
    CHECK(ok);
}

TEST(session_settings_follow_the_spec) {
    FIX::SessionSettings settings;
    auto acme = fixgen::venues::acme_mtf::make_adapter();
    fixgen::qf::SessionParams p;
    p.sender_comp_id = "ME";
    p.target_comp_id = "THEM";
    p.host = "127.0.0.1";
    p.port = 9999;
    auto sid = fixgen::qf::add_session(settings, *acme, p);
    CHECK_EQ(sid.getBeginString().getValue(), std::string("FIXT.1.1"));
    const auto& d = settings.get(sid);
    CHECK_EQ(d.getString("DefaultApplVerID"), std::string("9"));
    CHECK_EQ(d.getString("HeartBtInt"), std::string("20"));
    CHECK(d.has("TransportDataDictionary"));
}

TEST(display_masks_credentials) {
    std::string s = fixgen::qf::display(soh("8=FIX.4.4|35=A|553=u|554=secret|20010=M1|10=000|"));
    CHECK_EQ(s, std::string("8=FIX.4.4|35=A|553=u|554=***|20010=***|10=000|"));
}
