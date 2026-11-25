#include "fixcore/venue_adapter.hpp"
#include "fixgen/venues/acme_mtf.hpp"
#include "fixgen/venues/demo_exch.hpp"
#include "test.hpp"

using namespace fixcore;
namespace demo = fixgen::venues::demo_exch;
namespace acme = fixgen::venues::acme_mtf;

namespace {
NewOrder limit_buy() {
    NewOrder o;
    o.cl_ord_id = "C1";
    o.symbol = "VOD";
    o.side = Side::Buy;
    o.qty = "100";
    o.ord_type = OrdType::Limit;
    o.price = "101.25";
    return o;
}
}  // namespace

TEST(registry_lists_generated_venues) {
    auto all = make_all_venues();
    CHECK_EQ(all.size(), 2u);
    CHECK(make_venue("demo_exch") != nullptr);
    CHECK(make_venue("nope") == nullptr);
}

TEST(demo_new_order_translation) {
    auto v = demo::make_adapter();
    auto m = v->build_new_order(limit_buy());
    CHECK(static_cast<bool>(m));
    if (!m) { std::cerr << m.error() << "\n"; return; }
    CHECK_EQ(m->msg_type(), std::string("D"));
    CHECK_EQ(m->get_or(demo::tag::Symbol), std::string("VOD"));
    CHECK_EQ(m->get_or(demo::tag::Price), std::string("101.25"));
    CHECK_EQ(m->get_or(demo::tag::Side), std::string(demo::values::Side::Buy));
    CHECK_EQ(m->get_or(demo::tag::HandlInst), std::string("1"));  // spec default
    CHECK(m->has(demo::tag::TransactTime));
    CHECK(!m->has(demo::tag::TimeInForce));  // optional and not requested
}

TEST(demo_rejects_what_the_venue_cannot_take) {
    auto v = demo::make_adapter();
    auto o = limit_buy();
    o.price.reset();
    CHECK(!v->build_new_order(o));  // limit needs a price
    o = limit_buy();
    o.tif = TimeInForce::Gtc;
    auto r = v->build_new_order(o);
    CHECK(!r);
    CHECK(r.error().find("time in force gtc") != std::string::npos);
    o = limit_buy();
    o.extra["ClientCapacity"] = "A";  // an acme field, unknown here
    CHECK(!v->build_new_order(o));
    o = limit_buy();
    o.ord_type = OrdType::Market;
    o.price.reset();
    CHECK(static_cast<bool>(v->build_new_order(o)));
}

TEST(acme_needs_account_capacity_and_uses_isin) {
    auto v = acme::make_adapter();
    auto o = limit_buy();
    o.symbol = "GB00BH4HKS39";
    auto r = v->build_new_order(o);
    CHECK(!r);  // no account
    o.account = "ACC1";
    r = v->build_new_order(o);
    CHECK(!r);  // ClientCapacity missing
    CHECK(r.error().find("ClientCapacity") != std::string::npos);
    o.extra["ClientCapacity"] = "Z";
    CHECK(!v->build_new_order(o));  // not a valid value
    o.extra["ClientCapacity"] = "P";
    r = v->build_new_order(o);
    CHECK(static_cast<bool>(r));
    if (!r) { std::cerr << r.error() << "\n"; return; }
    CHECK_EQ(r->get_or(acme::tag::SecurityID), std::string("GB00BH4HKS39"));
    CHECK_EQ(r->get_or(acme::tag::SecurityIDSource), std::string("4"));
    CHECK_EQ(r->get_or(acme::tag::TimeInForce), std::string("0"));  // required -> day
    CHECK(!r->has(55));

    o.ord_type = OrdType::Market;
    o.price.reset();
    CHECK(!v->build_new_order(o));
    ReplaceOrder rep;
    CHECK(!v->build_replace(rep));
}

TEST(acme_capabilities_and_logon) {
    auto v = acme::make_adapter();
    const auto& caps = v->capabilities();
    CHECK_EQ(caps.extras.size(), 1u);
    if (!caps.extras.empty()) {
        CHECK_EQ(caps.extras[0].name, std::string("ClientCapacity"));
        CHECK(caps.extras[0].required);
        CHECK_EQ(caps.extras[0].values.size(), 3u);
    }
    CHECK(!caps.replace);
    Message logon("A");
    CHECK(!v->decorate_logon(logon, {{"username", "u"}, {"password", "p"}}));
    CHECK(static_cast<bool>(v->decorate_logon(logon, {{"username", "u"}, {"password", "p"}, {"member_code", "M7"}})));
    CHECK_EQ(logon.get_or(acme::tag::MemberCode), std::string("M7"));
    CHECK_EQ(logon.get_or(tags::DefaultApplVerID), std::string("9"));
}

TEST(exec_report_round_trip) {
    for (auto& v : make_all_venues()) {
        auto o = limit_buy();
        o.account = "ACC1";
        o.extra = v->id() == "acme_mtf" ? Extras{{"ClientCapacity", "A"}} : Extras{};
        auto req = v->build_new_order(o);
        CHECK(static_cast<bool>(req));
        if (!req) continue;
        VenueAdapter::ExecReportArgs a;
        a.order_id = "O1";
        a.exec_id = "E1";
        a.kind = ExecKind::Trade;
        a.status = OrderStatus::Filled;
        a.cum_qty = "100";
        a.leaves_qty = "0";
        a.avg_px = "101.25";
        a.last_qty = "100";
        a.last_px = "101.25";
        auto er = v->build_exec_report(req.value(), a);
        CHECK(static_cast<bool>(er));
        if (!er) { std::cerr << er.error() << "\n"; continue; }
        auto ev = v->parse(er.value());
        CHECK(ev.has_value());
        CHECK(ev->kind == ExecKind::Trade);
        CHECK(ev->status == OrderStatus::Filled);
        CHECK_EQ(ev->cl_ord_id, std::string("C1"));
        CHECK_EQ(ev->symbol, std::string("VOD"));
        CHECK(ev->warnings.empty());
    }
}

TEST(parse_flags_off_spec_messages) {
    auto v = demo::make_adapter();
    Message er("8");
    er.set(11, "C1").set(150, "Z").set(39, "0");
    auto ev = v->parse(er);
    CHECK(ev.has_value());
    CHECK(ev->kind == ExecKind::Unknown);
    CHECK(!ev->warnings.empty());
}
