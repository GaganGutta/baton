// SimFs is the foundation of the durability tests, so its crash model is tested
// explicitly: if SimFs were too forgiving, the tests built on it would prove
// nothing.

#include "testing/sim_fs.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace baton {
namespace {

class SimFsTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(fs_.create_dir_if_missing("d").ok()); }

  std::unique_ptr<WritableFile> create(const std::string& path) {
    auto file = fs_.open_append(path, OpenMode::kCreateNew);
    EXPECT_TRUE(file.ok());
    return std::move(file).value();
  }

  SimFs fs_;
};

TEST_F(SimFsTest, BehavesLikeAFileSystem) {
  auto file = create("d/a");
  ASSERT_TRUE(file->append("abc").ok());
  EXPECT_EQ(file->size(), 3U);
  EXPECT_EQ(fs_.read_file("d/a").value(), "abc");
  EXPECT_EQ(fs_.file_size("d/a").value(), 3U);
  EXPECT_FALSE(fs_.open_append("d/a", OpenMode::kCreateNew).ok());
  EXPECT_FALSE(fs_.open_append("d/missing", OpenMode::kAppendExisting).ok());
  EXPECT_EQ(fs_.list_dir("d").value(), std::vector<std::string>{"a"});

  ASSERT_TRUE(fs_.rename("d/a", "d/b").ok());
  EXPECT_FALSE(fs_.exists("d/a"));
  EXPECT_EQ(fs_.read_file("d/b").value(), "abc");
  ASSERT_TRUE(fs_.truncate("d/b", 1).ok());
  EXPECT_EQ(fs_.read_file("d/b").value(), "a");
  ASSERT_TRUE(fs_.remove("d/b").ok());
  EXPECT_FALSE(fs_.exists("d/b"));
}

TEST_F(SimFsTest, UnsyncedDataIsLostOnPowerLoss) {
  auto file = create("d/a");
  ASSERT_TRUE(fs_.sync_dir("d").ok());
  ASSERT_TRUE(file->append("durable").ok());
  ASSERT_TRUE(file->sync().ok());
  ASSERT_TRUE(file->append("-volatile").ok());

  const auto image = fs_.crash_image(CrashMode::kLoseUnsynced);
  EXPECT_EQ(image->read_file("d/a").value(), "durable");
}

TEST_F(SimFsTest, EverythingWrittenSurvivesAProcessCrash) {
  auto file = create("d/a");
  ASSERT_TRUE(file->append("not synced at all").ok());
  const auto image = fs_.crash_image(CrashMode::kKeepUnsynced);
  EXPECT_EQ(image->read_file("d/a").value(), "not synced at all");
}

TEST_F(SimFsTest, SyncedFileVanishesIfDirectoryWasNotSynced) {
  auto file = create("d/a");
  ASSERT_TRUE(file->append("data").ok());
  ASSERT_TRUE(file->sync().ok());  // contents durable, directory entry not

  EXPECT_FALSE(fs_.crash_image(CrashMode::kLoseUnsynced)->exists("d/a"));

  ASSERT_TRUE(fs_.sync_dir("d").ok());
  EXPECT_EQ(fs_.crash_image(CrashMode::kLoseUnsynced)->read_file("d/a").value(), "data");
}

TEST_F(SimFsTest, RenameAndRemoveNeedDirectorySync) {
  fs_.write_file("d/old", "v1");
  fs_.write_file("d/doomed", "x");
  ASSERT_TRUE(fs_.rename("d/old", "d/new").ok());
  ASSERT_TRUE(fs_.remove("d/doomed").ok());

  auto before = fs_.crash_image(CrashMode::kLoseUnsynced);
  EXPECT_TRUE(before->exists("d/old"));
  EXPECT_FALSE(before->exists("d/new"));
  EXPECT_TRUE(before->exists("d/doomed"));

  ASSERT_TRUE(fs_.sync_dir("d").ok());
  auto after = fs_.crash_image(CrashMode::kLoseUnsynced);
  EXPECT_FALSE(after->exists("d/old"));
  EXPECT_EQ(after->read_file("d/new").value(), "v1");
  EXPECT_FALSE(after->exists("d/doomed"));
}

TEST_F(SimFsTest, TornImagesKeepSyncedPrefixAndNeverGrow) {
  auto file = create("d/a");
  ASSERT_TRUE(fs_.sync_dir("d").ok());
  ASSERT_TRUE(file->append("SYNCED--").ok());
  ASSERT_TRUE(file->sync().ok());
  ASSERT_TRUE(file->append(std::string(200, 'u')).ok());

  bool saw_partial = false;
  bool saw_garbage = false;
  for (uint64_t seed = 0; seed < 200; ++seed) {
    const auto image = fs_.crash_image(CrashMode::kTorn, seed);
    const std::string data = image->read_file("d/a").value();
    ASSERT_GE(data.size(), 8U);
    ASSERT_LE(data.size(), 208U);
    ASSERT_EQ(data.substr(0, 8), "SYNCED--") << "synced bytes must never be damaged";
    if (data.size() > 8 && data.size() < 208) saw_partial = true;
    if (data.find_first_not_of('u', 8) != std::string::npos) saw_garbage = true;
  }
  EXPECT_TRUE(saw_partial);
  EXPECT_TRUE(saw_garbage);
}

// One fsync of the directory after several changes promises nothing about
// which of them a crash in between keeps. Every combination must show up.
TEST_F(SimFsTest, TornImagesKeepAnySubsetOfUnsyncedDirectoryChanges) {
  fs_.write_file("d/one", "1");
  fs_.write_file("d/two", "2");
  fs_.write_file("d/old", "v");
  ASSERT_TRUE(fs_.remove("d/one").ok());
  ASSERT_TRUE(fs_.remove("d/two").ok());
  ASSERT_TRUE(fs_.rename("d/old", "d/new").ok());

  std::set<std::string> outcomes;
  for (uint64_t seed = 0; seed < 200; ++seed) {
    const auto image = fs_.crash_image(CrashMode::kTorn, seed);
    ASSERT_NE(image->exists("d/old"), image->exists("d/new")) << "a rename is atomic";
    outcomes.insert(std::string(image->exists("d/one") ? "1" : "-") +
                    (image->exists("d/two") ? "2" : "-") + (image->exists("d/new") ? "N" : "O"));
  }
  EXPECT_EQ(outcomes.size(), 8U) << "2 unlinks and a rename: 8 possible directories";

  ASSERT_TRUE(fs_.sync_dir("d").ok());
  for (uint64_t seed = 0; seed < 20; ++seed) {
    const auto image = fs_.crash_image(CrashMode::kTorn, seed);
    EXPECT_EQ(image->list_dir("d").value(), std::vector<std::string>{"new"});
  }
}

// Operations on the same name keep their order, whatever subset survives.
TEST_F(SimFsTest, TornDirectoryChangesAreAppliedInOrder) {
  fs_.write_file("d/a", "first");
  ASSERT_TRUE(fs_.remove("d/a").ok());
  auto second = create("d/a");
  ASSERT_TRUE(second->append("second").ok());
  ASSERT_TRUE(second->sync().ok());

  for (uint64_t seed = 0; seed < 100; ++seed) {
    const auto image = fs_.crash_image(CrashMode::kTorn, seed);
    if (!image->exists("d/a")) continue;
    const std::string data = image->read_file("d/a").value();
    EXPECT_TRUE(data == "first" || data == "second") << data;
  }
}

TEST_F(SimFsTest, ReadsStreamInPieces) {
  fs_.write_file("d/a", "0123456789");
  auto file = fs_.open_read("d/a");
  ASSERT_TRUE(file.ok());
  std::string out = "kept:";
  EXPECT_EQ((*file)->read(4, out).value(), 4U);
  EXPECT_EQ((*file)->read(100, out).value(), 6U);
  EXPECT_EQ((*file)->read(100, out).value(), 0U) << "end of file";
  EXPECT_EQ(out, "kept:0123456789") << "read() appends";
  EXPECT_FALSE(fs_.open_read("d/missing").ok());
}

TEST_F(SimFsTest, HeldSyncsBlockOnlyMatchingFiles) {
  auto log = create("d/wal-1");
  auto other = create("d/snapshot-1");
  fs_.hold_syncs("wal-");
  ASSERT_TRUE(other->append("x").ok());
  ASSERT_TRUE(other->sync().ok()) << "not held: returns at once";

  std::atomic<bool> synced{false};
  std::thread blocked([&] {
    EXPECT_TRUE(log->sync().ok());
    synced = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(synced.load());
  fs_.release_syncs();
  blocked.join();
  EXPECT_TRUE(synced.load());
}

TEST_F(SimFsTest, ImagesAreIndependentOfTheOriginal) {
  auto file = create("d/a");
  ASSERT_TRUE(file->append("one").ok());
  const auto image = fs_.crash_image(CrashMode::kKeepUnsynced);
  ASSERT_TRUE(file->append("two").ok());
  EXPECT_EQ(image->read_file("d/a").value(), "one");
  EXPECT_EQ(fs_.read_file("d/a").value(), "onetwo");
}

TEST_F(SimFsTest, OpenHandleSurvivesRename) {
  auto file = create("d/tmp");
  ASSERT_TRUE(fs_.rename("d/tmp", "d/final").ok());
  ASSERT_TRUE(file->append("late write").ok());
  EXPECT_EQ(fs_.read_file("d/final").value(), "late write");
}

TEST_F(SimFsTest, CapturesImageAfterNthOperation) {
  auto file = create("d/a");  // op 1
  int callbacks = 0;
  fs_.capture_crash_image_after(2, CrashMode::kKeepUnsynced, 0, [&] { ++callbacks; });
  ASSERT_TRUE(file->append("first").ok());  // op 2
  EXPECT_EQ(fs_.take_captured_image(), nullptr);
  ASSERT_TRUE(file->append("second").ok());  // op 3: capture fires here
  ASSERT_TRUE(file->append("third").ok());
  EXPECT_EQ(callbacks, 1);

  const auto image = fs_.take_captured_image();
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->read_file("d/a").value(), "firstsecond");
  EXPECT_EQ(fs_.take_captured_image(), nullptr);
}

TEST_F(SimFsTest, InjectedSyncFailureIsSticky) {
  auto file = create("d/a");
  fs_.fail_sync_after(2);
  EXPECT_TRUE(file->sync().ok());
  EXPECT_FALSE(file->sync().ok());
  EXPECT_FALSE(file->sync().ok());

  ASSERT_TRUE(file->append("x").ok());
  // A failed sync made nothing durable.
  EXPECT_EQ(fs_.crash_image(CrashMode::kLoseUnsynced)->exists("d/a"), false);
}

TEST_F(SimFsTest, InjectedAppendFailureWritesAPrefix) {
  auto file = create("d/a");
  fs_.fail_append_after(1);
  EXPECT_FALSE(file->append("12345678").ok());
  EXPECT_EQ(fs_.read_file("d/a").value(), "1234");
  EXPECT_TRUE(file->append("ok").ok());
}

TEST_F(SimFsTest, CapacityLimitActsLikeAFullDisk) {
  auto file = create("d/a");
  fs_.set_capacity(10);
  EXPECT_TRUE(file->append("12345678").ok());
  const Status full = file->append("abcdef");
  ASSERT_FALSE(full.ok());
  EXPECT_NE(full.error().message().find("No space left"), std::string::npos);
  EXPECT_EQ(fs_.read_file("d/a").value(), "12345678ab");

  fs_.set_capacity(std::nullopt);
  EXPECT_TRUE(file->append("more").ok());
}

TEST_F(SimFsTest, FlipBitChangesExactlyOneBit) {
  fs_.write_file("d/a", std::string("\x00\x00", 2));
  fs_.flip_bit("d/a", 1, 3);
  EXPECT_EQ(fs_.read_file("d/a").value(), std::string("\x00\x08", 2));
}

}  // namespace
}  // namespace baton
