// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "a2ui/events.h"
#include "a2ui/json.h"
#include "test_main.h"

using a2ui::Json;

TEST(json_round_trip) {
  const char* text = R"({"a":[1,2.5,"x\n",true,null,{"b":{}}],"c":"é😀"})";
  auto r = Json::parse(text);
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.value["a"][1].as_number(), 2.5);
  EXPECT_EQ(r.value["c"].as_string(), std::string("\xC3\xA9\xF0\x9F\x98\x80"));
  auto again = Json::parse(r.value.dump());
  EXPECT_TRUE(again.ok);
  EXPECT_TRUE(again.value == r.value);
}

TEST(json_rejects_malformed) {
  EXPECT_TRUE(!Json::parse("{").ok);
  EXPECT_TRUE(!Json::parse("[1,]").ok);
  EXPECT_TRUE(!Json::parse("01").ok);
  EXPECT_TRUE(!Json::parse("\"a\x01\"").ok);
  EXPECT_TRUE(!Json::parse("[[[[1]]]]", 3).ok);
  EXPECT_TRUE(Json::parse("[[[1]]]", 3).ok);
}

TEST(json_object_equality_ignores_order) {
  EXPECT_TRUE(Json::parse(R"({"a":1,"b":2})").value == Json::parse(R"({"b":2,"a":1})").value);
  EXPECT_TRUE(Json::parse("1").value == Json(1.0));
  EXPECT_TRUE(!(Json() == Json(nullptr)));
}

TEST(number_to_string_matches_ecmascript) {
  EXPECT_EQ(a2ui::number_to_string(0.1 + 0.2), std::string("0.30000000000000004"));
  EXPECT_EQ(a2ui::number_to_string(1e21), std::string("1e+21"));
  EXPECT_EQ(a2ui::number_to_string(123456789012345680000.0), std::string("123456789012345680000"));
  EXPECT_EQ(a2ui::number_to_string(0.000001), std::string("0.000001"));
  EXPECT_EQ(a2ui::number_to_string(1e-7), std::string("1e-7"));
  EXPECT_EQ(a2ui::number_to_string(-42), std::string("-42"));
  EXPECT_EQ(a2ui::number_to_string(-0.0), std::string("0"));
  EXPECT_EQ(a2ui::number_to_string(1.5e300), std::string("1.5e+300"));
}

TEST(signal_batches_notifications) {
  a2ui::Signal<int> s(1);
  int calls = 0, last = 0;
  auto sub = s.subscribe([&](const int& v) {
    ++calls;
    last = v;
  });
  {
    a2ui::Batch batch;
    s.set(2);
    s.set(3);
    EXPECT_EQ(calls, 0);
  }
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(last, 3);
  sub.unsubscribe();
  s.set(4);
  EXPECT_EQ(calls, 1);
}

TEST(event_source_unsubscribe_during_emit) {
  a2ui::EventSource<int> ev;
  int a = 0, b = 0;
  a2ui::Subscription sb;
  auto sa = ev.subscribe([&](const int&) {
    ++a;
    sb.unsubscribe();
  });
  sb = ev.subscribe([&](const int&) { ++b; });
  ev.emit(1);
  ev.emit(2);
  EXPECT_EQ(a, 2);
  EXPECT_EQ(b, 0);
  EXPECT_EQ(ev.listener_count(), size_t(1));
}
