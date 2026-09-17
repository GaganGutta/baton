#include "common/posix_fs.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "support/temp_dir.h"

namespace baton {
namespace {

class PosixFsTest : public ::testing::Test {
 protected:
  TempDir dir_;
  PosixFs fs_;
};

TEST_F(PosixFsTest, AppendSyncReadRoundTrip) {
  const std::string path = dir_.file("a.log");
  auto file = fs_.open_append(path, OpenMode::kCreateNew);
  ASSERT_TRUE(file.ok()) << file.error().to_string();
  EXPECT_EQ((*file)->size(), 0U);
  ASSERT_TRUE((*file)->append("hello ").ok());
  ASSERT_TRUE((*file)->append(std::string_view("wor\0ld", 6)).ok());
  ASSERT_TRUE((*file)->sync().ok());
  EXPECT_EQ((*file)->size(), 12U);

  const auto data = fs_.read_file(path);
  ASSERT_TRUE(data.ok());
  EXPECT_EQ(*data, std::string("hello wor\0ld", 12));
  EXPECT_EQ(fs_.file_size(path).value(), 12U);
}

TEST_F(PosixFsTest, CreateNewFailsIfFileExists) {
  const std::string path = dir_.file("a.log");
  ASSERT_TRUE(fs_.open_append(path, OpenMode::kCreateNew).ok());
  const auto again = fs_.open_append(path, OpenMode::kCreateNew);
  ASSERT_FALSE(again.ok());
  EXPECT_EQ(again.error().code(), ErrorCode::kIo);
  EXPECT_NE(again.error().message().find("File exists"), std::string::npos);
}

TEST_F(PosixFsTest, AppendExistingContinuesAtTheEnd) {
  const std::string path = dir_.file("a.log");
  {
    auto file = fs_.open_append(path, OpenMode::kCreateNew);
    ASSERT_TRUE(file.ok());
    ASSERT_TRUE((*file)->append("abc").ok());
  }
  auto file = fs_.open_append(path, OpenMode::kAppendExisting);
  ASSERT_TRUE(file.ok());
  EXPECT_EQ((*file)->size(), 3U);
  ASSERT_TRUE((*file)->append("def").ok());
  EXPECT_EQ(fs_.read_file(path).value(), "abcdef");
}

TEST_F(PosixFsTest, AppendExistingFailsIfMissing) {
  EXPECT_FALSE(fs_.open_append(dir_.file("missing"), OpenMode::kAppendExisting).ok());
  EXPECT_FALSE(fs_.read_file(dir_.file("missing")).ok());
  EXPECT_FALSE(fs_.file_size(dir_.file("missing")).ok());
}

TEST_F(PosixFsTest, TruncateShrinksFile) {
  const std::string path = dir_.file("a.log");
  {
    auto file = fs_.open_append(path, OpenMode::kCreateNew);
    ASSERT_TRUE(file.ok());
    ASSERT_TRUE((*file)->append("0123456789").ok());
  }
  ASSERT_TRUE(fs_.truncate(path, 4).ok());
  EXPECT_EQ(fs_.read_file(path).value(), "0123");
}

TEST_F(PosixFsTest, RenameReplacesAndRemoveDeletes) {
  const std::string a = dir_.file("a");
  const std::string b = dir_.file("b");
  for (const auto& [path, text] : {std::pair{a, "new"}, std::pair{b, "old"}}) {
    auto file = fs_.open_append(path, OpenMode::kCreateNew);
    ASSERT_TRUE(file.ok());
    ASSERT_TRUE((*file)->append(text).ok());
  }
  ASSERT_TRUE(fs_.rename(a, b).ok());
  ASSERT_TRUE(fs_.sync_dir(dir_.path()).ok());
  EXPECT_EQ(fs_.read_file(b).value(), "new");
  EXPECT_FALSE(fs_.read_file(a).ok());

  ASSERT_TRUE(fs_.remove(b).ok());
  EXPECT_FALSE(fs_.remove(b).ok());
}

TEST_F(PosixFsTest, ListDirReturnsNamesWithoutDots) {
  ASSERT_TRUE(fs_.open_append(dir_.file("one"), OpenMode::kCreateNew).ok());
  ASSERT_TRUE(fs_.open_append(dir_.file("two"), OpenMode::kCreateNew).ok());
  auto names = fs_.list_dir(dir_.path());
  ASSERT_TRUE(names.ok());
  std::ranges::sort(*names);
  EXPECT_EQ(*names, (std::vector<std::string>{"one", "two"}));
  EXPECT_FALSE(fs_.list_dir(dir_.file("nope")).ok());
}

TEST_F(PosixFsTest, CreateDirIfMissingIsIdempotent) {
  const std::string sub = dir_.file("sub");
  EXPECT_TRUE(fs_.create_dir_if_missing(sub).ok());
  EXPECT_TRUE(fs_.create_dir_if_missing(sub).ok());
  EXPECT_TRUE(fs_.sync_dir(sub).ok());
  EXPECT_FALSE(fs_.create_dir_if_missing(dir_.file("no/such/parent")).ok());
}

TEST(JoinPathTest, HandlesTrailingSlash) {
  EXPECT_EQ(join_path("/data", "wal.log"), "/data/wal.log");
  EXPECT_EQ(join_path("/data/", "wal.log"), "/data/wal.log");
  EXPECT_EQ(join_path("", "wal.log"), "wal.log");
}

}  // namespace
}  // namespace baton
