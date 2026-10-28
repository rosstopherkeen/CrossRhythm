#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "util/BookCacheUtils.h"

namespace {

const std::vector<uint8_t> RHYTHM_BYTES = {'C', 'I', 'B', 'R', 1, 0, 1, 0, 0x10, 0x00, 0x00, 0x00, 0x2d, 0x00};
const std::vector<uint8_t> RHYTHM_BAK_BYTES = {'C', 'I', 'B', 'R', 1, 0, 0, 0};
const std::vector<uint8_t> STATS_BYTES = {'S', 'T', '5'};
const std::vector<uint8_t> JUNK_BYTES = {0xde, 0xad, 0xbe, 0xef};

// Mirrors the stub cache path derivation in stubs/BookFormatStubs.h.
std::string cachePathFor(const std::string& prefix, const std::string& stem) { return "/.crosspoint/" + prefix + stem; }

class BookCachePreservationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    ASSERT_TRUE(Storage.mkdir("/.crosspoint", true));
  }

  // Writes a cache directory holding the rhythm sidecar, a stats file, and
  // throwaway derived output.
  static void seedCache(const std::string& cachePath) {
    ASSERT_TRUE(Storage.mkdir(cachePath.c_str(), true));
    Storage.putFile(cachePath + "/rhythm_days.bin", RHYTHM_BYTES);
    Storage.putFile(cachePath + "/rhythm_days.bin.bak", RHYTHM_BAK_BYTES);
    Storage.putFile(cachePath + "/stats_v5.bin", STATS_BYTES);
    Storage.putFile(cachePath + "/progress.bin", JUNK_BYTES);
    Storage.putFile(cachePath + "/section_0.bin", JUNK_BYTES);
    Storage.putFile(cachePath + "/spine.bin", JUNK_BYTES);
    Storage.putFile(cachePath + "/images/cover.pxc", JUNK_BYTES);
  }

  static void expectRhythmPreserved(const std::string& cachePath) {
    EXPECT_TRUE(Storage.exists((cachePath + "/rhythm_days.bin").c_str()));
    EXPECT_EQ(Storage.bytes(cachePath + "/rhythm_days.bin"), RHYTHM_BYTES);
    EXPECT_TRUE(Storage.exists((cachePath + "/rhythm_days.bin.bak").c_str()));
    EXPECT_EQ(Storage.bytes(cachePath + "/rhythm_days.bin.bak"), RHYTHM_BAK_BYTES);
  }

  static void expectDerivedCacheGone(const std::string& cachePath) {
    EXPECT_FALSE(Storage.exists((cachePath + "/section_0.bin").c_str()));
    EXPECT_FALSE(Storage.exists((cachePath + "/spine.bin").c_str()));
    EXPECT_FALSE(Storage.exists((cachePath + "/images").c_str()));
    EXPECT_FALSE(Storage.exists((cachePath + "/images/cover.pxc").c_str()));
  }

  // No preservation temp may be left beside the cache directory.
  static void expectNoLeftoverTemps(const std::string& cachePath) {
    for (const std::string& child : Storage.childrenOf("/.crosspoint")) {
      EXPECT_EQ(child.find(cachePath + "."), std::string::npos) << "leftover preservation temp: " << child;
    }
  }
};

TEST_F(BookCachePreservationTest, EpubUserStateClearKeepsRhythmSidecar) {
  const std::string cachePath = cachePathFor("epub_", "book");
  seedCache(cachePath);

  ASSERT_TRUE(clearBookCachePreservingUserState("/books/book.epub"));

  expectRhythmPreserved(cachePath);
  EXPECT_EQ(Storage.bytes(cachePath + "/stats_v5.bin"), STATS_BYTES);
  EXPECT_TRUE(Storage.exists((cachePath + "/progress.bin").c_str()));
  expectDerivedCacheGone(cachePath);
  expectNoLeftoverTemps(cachePath);
}

TEST_F(BookCachePreservationTest, XtcUserStateClearKeepsRhythmSidecar) {
  const std::string cachePath = cachePathFor("xtc_", "comic");
  seedCache(cachePath);

  ASSERT_TRUE(clearBookCachePreservingUserState("/books/comic.xtc"));

  expectRhythmPreserved(cachePath);
  EXPECT_EQ(Storage.bytes(cachePath + "/stats_v5.bin"), STATS_BYTES);
  expectDerivedCacheGone(cachePath);
  expectNoLeftoverTemps(cachePath);
}

TEST_F(BookCachePreservationTest, DirectoryStatsClearKeepsRhythmSidecar) {
  const std::string cachePath = cachePathFor("epub_", "book");
  seedCache(cachePath);

  ASSERT_TRUE(clearBookCacheDirectoryPreservingStats(cachePath));

  expectRhythmPreserved(cachePath);
  EXPECT_EQ(Storage.bytes(cachePath + "/stats_v5.bin"), STATS_BYTES);
  // Settings > Clear Cache intentionally drops progress; only stats and
  // dictionary history survive that path.
  EXPECT_FALSE(Storage.exists((cachePath + "/progress.bin").c_str()));
  expectDerivedCacheGone(cachePath);
  expectNoLeftoverTemps(cachePath);
}

TEST_F(BookCachePreservationTest, DirectoryStatsClearKeepsRhythmSidecarForXtcCache) {
  const std::string cachePath = cachePathFor("xtc_", "comic");
  seedCache(cachePath);

  ASSERT_TRUE(clearBookCacheDirectoryPreservingStats(cachePath));

  expectRhythmPreserved(cachePath);
  expectDerivedCacheGone(cachePath);
  expectNoLeftoverTemps(cachePath);
}

// A power loss between the move-aside and the restore leaves the sidecar in its
// temp sibling; the next clear must bring it back rather than lose it.
TEST_F(BookCachePreservationTest, InterruptedUserStatePreservationRecoversRhythmSidecar) {
  const std::string cachePath = cachePathFor("epub_", "book");
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str(), true));
  Storage.putFile(cachePath + "/section_0.bin", JUNK_BYTES);
  Storage.putFile(cachePath + ".upload_preserve_rhythm_days.bin", RHYTHM_BYTES);

  ASSERT_TRUE(clearBookCachePreservingUserState("/books/book.epub"));

  EXPECT_EQ(Storage.bytes(cachePath + "/rhythm_days.bin"), RHYTHM_BYTES);
  EXPECT_FALSE(Storage.exists((cachePath + "/section_0.bin").c_str()));
  expectNoLeftoverTemps(cachePath);
}

TEST_F(BookCachePreservationTest, InterruptedDirectoryPreservationRecoversRhythmSidecar) {
  const std::string cachePath = cachePathFor("epub_", "book");
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str(), true));
  Storage.putFile(cachePath + "/section_0.bin", JUNK_BYTES);
  Storage.putFile(cachePath + ".clear_preserve_rhythm_days.bin", RHYTHM_BYTES);

  ASSERT_TRUE(clearBookCacheDirectoryPreservingStats(cachePath));

  EXPECT_EQ(Storage.bytes(cachePath + "/rhythm_days.bin"), RHYTHM_BYTES);
  EXPECT_FALSE(Storage.exists((cachePath + "/section_0.bin").c_str()));
  expectNoLeftoverTemps(cachePath);
}

// The rhythm entries are shared with the TXT reader, which never writes the
// sidecar. Their absence must not fail the clear.
TEST_F(BookCachePreservationTest, TxtClearSucceedsWithoutRhythmSidecar) {
  const std::string cachePath = cachePathFor("txt_", "notes");
  ASSERT_TRUE(Storage.mkdir(cachePath.c_str(), true));
  Storage.putFile(cachePath + "/progress.bin", JUNK_BYTES);
  Storage.putFile(cachePath + "/pages.bin", JUNK_BYTES);

  ASSERT_TRUE(clearBookCachePreservingUserState("/books/notes.txt"));

  EXPECT_TRUE(Storage.exists((cachePath + "/progress.bin").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/rhythm_days.bin").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/pages.bin").c_str()));
  expectNoLeftoverTemps(cachePath);
}

}  // namespace
