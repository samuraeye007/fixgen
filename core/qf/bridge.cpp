#include "bridge.hpp"

#include <filesystem>
#include <regex>

#include "quickfix/Dictionary.h"

namespace fixgen::qf {
namespace {

bool session_owned(int tag) {
    switch (tag) {
        case 8: case 9: case 10: case 34: case 49: case 52: case 56: case 43: case 97: case 122:
            return true;
        default:
            return false;
    }
}

void append_flat(const FIX::FieldMap& map, fixcore::Message& out) {
    for (auto it = map.begin(); it != map.end(); ++it) out.add(it->getTag(), it->getString());
    // Repeating groups follow their count field. Venue specs do not model groups yet, so
    // they are flattened in order; see docs/ARCHITECTURE.md.
    for (auto g = map.g_begin(); g != map.g_end(); ++g)
        for (const auto* entry : g->second) append_flat(*entry, out);
}

}  // namespace

FIX::Message to_quickfix(const fixcore::Message& m) {
    FIX::Message out;
    out.getHeader().setField(FIX::FIELD::MsgType, m.msg_type());
    for (const auto& f : m.header())
        if (!session_owned(f.tag)) out.getHeader().setField(f.tag, f.value);
    for (const auto& f : m.body()) out.setField(f.tag, f.value);
    return out;
}

fixcore::Message from_quickfix(const FIX::Message& m) {
    fixcore::Message out;
    const auto& h = m.getHeader();
    for (auto it = h.begin(); it != h.end(); ++it) {
        int tag = it->getTag();
        if (tag == FIX::FIELD::BeginString || tag == FIX::FIELD::MsgType) out.set(tag, it->getString());
        else if (tag != FIX::FIELD::BodyLength) out.add(tag, it->getString());
    }
    append_flat(m, out);
    return out;
}

std::string display(const std::string& raw) {
    static const std::regex secret("(^|\x01)(554|925|20010)=[^\x01]*");
    std::string s = std::regex_replace(raw, secret, "$1$2=***");
    for (auto& c : s)
        if (c == '\x01') c = '|';
    return s;
}

std::string default_dict_dir() {
#ifdef FIXGEN_DEFAULT_DICT_DIR
    return FIXGEN_DEFAULT_DICT_DIR;
#else
    return "dict";
#endif
}

FIX::SessionID add_session(FIX::SessionSettings& settings, const fixcore::VenueAdapter& venue,
                           const SessionParams& p) {
    const auto d = venue.session_defaults();
    const std::string dir = p.dict_dir.empty() ? default_dict_dir() : p.dict_dir;
    const std::string id(venue.id());
    auto must_exist = [](const std::string& path) {
        if (!std::filesystem::exists(path)) throw FIX::ConfigError("missing QuickFIX dictionary " + path);
        return path;
    };

    FIX::Dictionary s;
    s.setString("ConnectionType", p.initiator ? "initiator" : "acceptor");
    s.setString("StartTime", "00:00:00");
    s.setString("EndTime", "00:00:00");
    s.setBool("NonStopSession", true);
    s.setInt("HeartBtInt", p.heartbeat_secs > 0 ? p.heartbeat_secs : d.heartbeat_secs);
    s.setBool("ResetOnLogon", d.reset_seq_num_on_logon);
    s.setBool("SocketNodelay", true);
    s.setInt("TimestampPrecision", 3);
    s.setInt("LogonTimeout", 10);

    // Lenient parsing: strict validation happens in fixcore::Dictionary (see codegen.py).
    s.setBool("UseDataDictionary", true);
    s.setBool("AllowUnknownMsgFields", true);
    s.setBool("ValidateUserDefinedFields", false);
    s.setBool("ValidateFieldsOutOfOrder", false);
    if (d.begin_string == "FIXT.1.1") {
        s.setString("TransportDataDictionary", must_exist(dir + "/" + id + ".transport.xml"));
        s.setString("AppDataDictionary", must_exist(dir + "/" + id + ".xml"));
        s.setString("DefaultApplVerID", d.default_appl_ver_id);
    } else {
        s.setString("DataDictionary", must_exist(dir + "/" + id + ".xml"));
    }

    if (p.initiator) {
        s.setString("SocketConnectHost", p.host);
        s.setInt("SocketConnectPort", p.port);
        s.setInt("ReconnectInterval", p.reconnect_secs);
    } else {
        s.setInt("SocketAcceptPort", p.port);
        s.setBool("SocketReuseAddress", true);
    }
    if (!p.store_dir.empty()) {
        s.setString("FileStorePath", p.store_dir);
        s.setBool("PersistMessages", true);
    }

    FIX::SessionID sid(d.begin_string, p.sender_comp_id, p.target_comp_id);
    settings.set(sid, s);
    return sid;
}

namespace {
class CallbackLog : public FIX::Log {
public:
    CallbackLog(CallbackLogFactory::Sink& sink, const FIX::SessionID* sid)
        : sink_(sink), has_sid_(sid != nullptr) {
        if (sid) sid_ = *sid;
    }
    void clear() override {}
    void backup() override {}
    void onIncoming(const std::string& s) override { emit(CallbackLogFactory::Kind::Incoming, s); }
    void onOutgoing(const std::string& s) override { emit(CallbackLogFactory::Kind::Outgoing, s); }
    void onEvent(const std::string& s) override { emit(CallbackLogFactory::Kind::Event, s); }

private:
    void emit(CallbackLogFactory::Kind k, const std::string& s) {
        if (sink_) sink_(has_sid_ ? &sid_ : nullptr, k, s);
    }
    CallbackLogFactory::Sink& sink_;
    bool has_sid_;
    FIX::SessionID sid_;
};
}  // namespace

FIX::Log* CallbackLogFactory::create() { return new CallbackLog(sink_, nullptr); }
FIX::Log* CallbackLogFactory::create(const FIX::SessionID& sid) { return new CallbackLog(sink_, &sid); }
void CallbackLogFactory::destroy(FIX::Log* log) { delete log; }

}  // namespace fixgen::qf
