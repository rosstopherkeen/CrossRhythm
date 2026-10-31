#include <gtest/gtest.h>

#include <cstring>

#include "activities/boot_sleep/SceneFileName.h"

namespace {
size_t extLen(const char* name) { return SceneFileName::extensionLength(name, strlen(name)); }
}  // namespace

TEST(SceneFileNameTest, AcceptsTheCurrentExtension) {
  EXPECT_EQ(extLen("lalbagh.rhythmscene"), strlen(".rhythmscene"));
  EXPECT_EQ(extLen("LALBAGH.RHYTHMSCENE"), strlen(".rhythmscene"));
}

TEST(SceneFileNameTest, StillAcceptsTheLegacyExtension) {
  EXPECT_EQ(extLen("kr_market.cpscene"), strlen(".cpscene"));
  EXPECT_TRUE(SceneFileName::isLegacy("kr_market.cpscene", strlen("kr_market.cpscene")));
  EXPECT_FALSE(SceneFileName::isLegacy("kr_market.rhythmscene", strlen("kr_market.rhythmscene")));
}

TEST(SceneFileNameTest, RejectsEverythingElse) {
  EXPECT_EQ(extLen(""), 0u);
  EXPECT_EQ(extLen(".rhythmscene"), 0u);  // no stem
  EXPECT_EQ(extLen(".cpscene"), 0u);
  EXPECT_EQ(extLen("lalbagh.scene"), 0u);
  EXPECT_EQ(extLen("lalbagh.rhythmscene.bak"), 0u);
  EXPECT_EQ(extLen("lalbagh.cpfont"), 0u);
  EXPECT_EQ(extLen("rhythmscene"), 0u);
}
