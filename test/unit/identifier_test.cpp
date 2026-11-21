// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "a2ui/identifier.h"
#include "test_main.h"

TEST(xid_properties) {
  EXPECT_TRUE(a2ui::is_xid_start('a'));
  EXPECT_TRUE(!a2ui::is_xid_start('_'));
  EXPECT_TRUE(!a2ui::is_xid_start('1'));
  EXPECT_TRUE(a2ui::is_xid_continue('_'));
  EXPECT_TRUE(a2ui::is_xid_continue('1'));
  EXPECT_TRUE(a2ui::is_xid_start(0x00DF));    // ß
  EXPECT_TRUE(a2ui::is_xid_start(0x5024));    // 値
  EXPECT_TRUE(a2ui::is_xid_start(0x20BB7));   // 𠮷
  EXPECT_TRUE(!a2ui::is_xid_start(0x0301));   // Combining acute: continue only.
  EXPECT_TRUE(a2ui::is_xid_continue(0x0301));
  EXPECT_TRUE(!a2ui::is_xid_continue(0x00BD));  // ½
  EXPECT_TRUE(!a2ui::is_xid_continue(0x1F680));  // 🚀
  EXPECT_TRUE(!a2ui::is_xid_continue(0xD800));
  EXPECT_TRUE(!a2ui::is_xid_continue(0x110000));
  EXPECT_TRUE(a2ui::is_xid_start(0x4E00) && a2ui::is_xid_start(0x9FFF));  // Long CJK range.
}

TEST(uax31_identifiers) {
  EXPECT_TRUE(a2ui::is_uax31_identifier("Text"));
  EXPECT_TRUE(a2ui::is_uax31_identifier("_private"));
  EXPECT_TRUE(a2ui::is_uax31_identifier("gr\xC3\xB6\xC3\x9F" "e"));
  EXPECT_TRUE(!a2ui::is_uax31_identifier(""));
  EXPECT_TRUE(!a2ui::is_uax31_identifier("1abc"));
  EXPECT_TRUE(!a2ui::is_uax31_identifier("a-b"));
  EXPECT_TRUE(!a2ui::is_uax31_identifier("a\xFF"));
}

TEST(utf8_decoding) {
  const std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  size_t pos = 0;
  EXPECT_EQ(a2ui::decode_utf8(s, pos), 0x61u);
  EXPECT_EQ(a2ui::decode_utf8(s, pos), 0xE9u);
  EXPECT_EQ(a2ui::decode_utf8(s, pos), 0x20ACu);
  EXPECT_EQ(a2ui::decode_utf8(s, pos), 0x1F600u);
  EXPECT_EQ(pos, s.size());
  EXPECT_EQ(a2ui::utf8_length(s), 4u);
  EXPECT_TRUE(a2ui::utf8_valid(s));
  EXPECT_TRUE(a2ui::utf8_valid("\xEF\xBF\xBD"));  // A real U+FFFD.
  for (const char* bad : {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE2\x82", "\x80"}) {
    EXPECT_TRUE(!a2ui::utf8_valid(bad));
    size_t p = 0;
    EXPECT_EQ(a2ui::decode_utf8(bad, p), 0xFFFDu);
    EXPECT_EQ(p, 1u);
  }
}
