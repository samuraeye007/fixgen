#pragma once
// Glue between fixcore (venue adapters, normalized model) and QuickFIX (session layer).
// fixcore itself stays QuickFIX-free so adapters can be unit-tested without a session.
#include <functional>
#include <string>

#include "fixcore/message.hpp"
#include "fixcore/venue_adapter.hpp"
#include "quickfix/Log.h"
#include "quickfix/Message.h"
#include "quickfix/SessionSettings.h"

namespace fixgen::qf {

// Application-level conversion. Header session fields (8, 9, 34, 49, 52, 56, 10) are owned by
// QuickFIX and are not copied into the outgoing message.
FIX::Message to_quickfix(const fixcore::Message& m);
fixcore::Message from_quickfix(const FIX::Message& m);

// Raw FIX (SOH separated) -> '|' separated with credentials masked, for logs and the UI.
std::string display(const std::string& raw);

struct SessionParams {
    std::string sender_comp_id, target_comp_id;
    bool initiator = true;
    std::string host;      // initiator
    int port = 0;          // connect port (initiator) or accept port (acceptor)
    int heartbeat_secs = 0;  // 0 = venue default from the spec
    int reconnect_secs = 5;
    std::string dict_dir;   // generated QuickFIX dictionaries; empty = build-time default
    std::string store_dir;  // FileStore path; empty = caller uses an in-memory store
};

// Adds one session for `venue` to `settings`, configured from the venue spec (BeginString,
// dictionaries, ResetOnLogon, heartbeat) plus `p`. Throws FIX::ConfigError.
FIX::SessionID add_session(FIX::SessionSettings& settings, const fixcore::VenueAdapter& venue,
                           const SessionParams& p);

std::string default_dict_dir();

// LogFactory that hands every raw message and session event to a callback.
class CallbackLogFactory : public FIX::LogFactory {
public:
    enum class Kind { Incoming, Outgoing, Event };
    using Sink = std::function<void(const FIX::SessionID*, Kind, const std::string&)>;
    explicit CallbackLogFactory(Sink sink) : sink_(std::move(sink)) {}
    FIX::Log* create() override;
    FIX::Log* create(const FIX::SessionID&) override;
    void destroy(FIX::Log* log) override;

private:
    Sink sink_;
};

}  // namespace fixgen::qf
