#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "common/clock.h"
#include "common/logging.h"
#include "common/posix_fs.h"
#include "common/version.h"
#include "server/config.h"
#include "server/server.h"

namespace {

// The signal handler may only do async-signal-safe work: Server::stop() is an
// atomic store plus a write() to the loop's wake-up pipe.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<baton::Server*> g_server{nullptr};

void on_signal(int /*signal*/) {
  if (baton::Server* server = g_server.load()) server->stop();
}

void install_signal_handlers() {
  struct sigaction action{};
  action.sa_handler = &on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, nullptr);
  sigaction(SIGINT, &action, nullptr);

  // A client that disconnects mid-reply must not kill the server.
  struct sigaction ignore{};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  sigaction(SIGPIPE, &ignore, nullptr);
}

int run(std::span<char*> raw_args) {
  const std::vector<std::string_view> args(raw_args.begin() + 1, raw_args.end());
  const auto parsed = baton::parse_args(args);
  if (!parsed.ok()) {
    std::cerr << "baton: " << parsed.error().message() << '\n';
    return 2;
  }
  if (parsed->action == baton::CliAction::kShowVersion) {
    std::cout << "baton " << baton::kVersion << '\n';
    return 0;
  }
  if (parsed->action == baton::CliAction::kShowHelp) {
    std::cout << baton::usage_text();
    return 0;
  }

  baton::set_log_level(parsed->config.log_level);
  baton::PosixFs fs;
  const baton::SystemClock clock;
  auto server = baton::Server::create(parsed->config, fs, clock);
  if (!server.ok()) {
    // Includes the refusal to start on a damaged log: say exactly why.
    BATON_ERROR("server", "cannot start: {}", server.error().to_string());
    return 1;
  }

  g_server.store(server->get());
  install_signal_handlers();
  const baton::Status served = (*server)->run();
  g_server.store(nullptr);
  if (!served.ok()) {
    BATON_ERROR("server", "event loop failed: {}", served.error().to_string());
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // baton itself never throws, but the standard library can (std::bad_alloc).
  // There is nothing to recover in-process: report it and exit; the durable log
  // makes the next start pick up exactly where the acknowledged work ended.
  try {
    return run(std::span<char*>(argv, static_cast<size_t>(argc)));
  } catch (const std::exception& e) {
    std::cerr << "baton: fatal: " << e.what() << '\n';
    return 1;
  }
}
