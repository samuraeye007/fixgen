#include "fixcore/json.hpp"
#include "test.hpp"

using namespace fixcore;

TEST(json_round_trip_keeps_number_text) {
    auto v = json::parse(R"({"px": 101.250, "n": [1, true, null, "a\"bé"], "o": {}})");
    CHECK(v.has_value());
    CHECK_EQ((*v)["px"].as_string(), std::string("101.250"));
    CHECK_EQ((*v)["n"].as_array().size(), 4u);
    CHECK_EQ((*v)["n"].as_array()[3].as_string(), std::string("a\"b\xc3\xa9"));
    auto again = json::parse(v->dump());
    CHECK(again.has_value());
    CHECK_EQ(again->dump(), v->dump());
}

TEST(json_errors) {
    std::string err;
    CHECK(!json::parse("{\"a\":}", &err));
    CHECK(!err.empty());
    CHECK(!json::parse("[1,2] x", &err));
}
