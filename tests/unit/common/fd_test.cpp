#include "common/fd.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <utility>

namespace baton {
namespace {

bool is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

std::pair<int, int> make_pipe() {
  std::array<int, 2> fds{};
  EXPECT_EQ(::pipe(fds.data()), 0);
  return {fds[0], fds[1]};
}

TEST(FdTest, DefaultIsInvalid) {
  const Fd fd;
  EXPECT_FALSE(fd.valid());
  EXPECT_EQ(fd.get(), -1);
}

TEST(FdTest, ClosesOnDestruction) {
  auto [r, w] = make_pipe();
  {
    const Fd owner(r);
    EXPECT_TRUE(owner.valid());
    EXPECT_TRUE(is_open(r));
  }
  EXPECT_FALSE(is_open(r));
  ::close(w);
}

TEST(FdTest, MoveTransfersOwnership) {
  auto [r, w] = make_pipe();
  Fd a(r);
  Fd b(std::move(a));
  EXPECT_FALSE(a.valid());  // NOLINT(bugprone-use-after-move): asserting the moved-from state
  EXPECT_EQ(b.get(), r);

  Fd c(w);
  c = std::move(b);  // closes w, takes r
  EXPECT_FALSE(is_open(w));
  EXPECT_TRUE(is_open(r));
  EXPECT_EQ(c.get(), r);
}

TEST(FdTest, ReleaseGivesUpOwnership) {
  auto [r, w] = make_pipe();
  int raw = -1;
  {
    Fd owner(r);
    raw = owner.release();
    EXPECT_FALSE(owner.valid());
  }
  EXPECT_TRUE(is_open(raw));
  ::close(raw);
  ::close(w);
}

TEST(FdTest, ResetClosesPrevious) {
  auto [r, w] = make_pipe();
  Fd owner(r);
  owner.reset(w);
  EXPECT_FALSE(is_open(r));
  EXPECT_EQ(owner.get(), w);
  owner.reset();
  EXPECT_FALSE(is_open(w));
}

}  // namespace
}  // namespace baton
