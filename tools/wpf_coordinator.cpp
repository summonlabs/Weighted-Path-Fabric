// Weighted Path Fabric - authoritative coordinator process.
// Copyright 2026 Summon Software Labs.
//
// One coordinator is authoritative for one durable store. It owns the engine,
// serves the framed protocol and writes the store through after every committed
// change, so a hard kill never loses a committed mutation. Live worker authority
// is never durable: recovery fences every boot and requires revalidation.
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "tool_support.hpp"
#include "wpf/engine.hpp"
#include "wpf/net.hpp"
#include "wpf/persistence.hpp"
#include "wpf/version.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

}  // namespace

int main(int argc, char** argv) {
  const wpftool::Arguments arguments = wpftool::Arguments::parse(argc, argv);

  wpf::ResourceLimits limits;
  if (const std::optional<std::uint64_t> sessions = arguments.get_u64("max-sessions")) {
    limits.max_sessions = static_cast<std::uint32_t>(*sessions);
  }
  if (const std::optional<std::uint64_t> frame = arguments.get_u64("max-frame-bytes")) {
    limits.max_frame_bytes = static_cast<std::uint32_t>(*frame);
  }

  wpf::EngineConfig config;
  config.limits = limits;
  wpf::WeightedFabricEngine engine(config);

  const std::string store = arguments.get_or("store", "");
  bool have_store = false;
  if (!store.empty() && std::filesystem::exists(store)) {
    const wpf::Result<wpf::EngineState> loaded = wpf::load_engine_state(store, limits);
    if (!loaded.ok()) {
      std::cerr << "error: could not load the store: " << loaded.error().to_string() << "\n";
      return 1;
    }
    std::uint64_t next_epoch = loaded.value().epoch.valid() ? loaded.value().epoch.value() + 1 : 1;
    if (const std::optional<std::uint64_t> requested = arguments.get_u64("epoch")) {
      next_epoch = *requested;
    }
    const wpf::Outcome imported =
        engine.import_state(loaded.value(), wpf::CoordinatorEpoch::from_rep(next_epoch));
    if (!imported.ok()) {
      std::cerr << "error: could not recover the store: " << imported.to_string() << "\n";
      return 1;
    }
    have_store = true;
  } else if (const std::optional<std::uint64_t> requested = arguments.get_u64("epoch")) {
    if (*requested > 1) {
      const wpf::Outcome advanced =
          engine.advance_epoch(wpf::CoordinatorEpoch::initial(), wpf::CoordinatorEpoch::from_rep(*requested),
                               wpftool::fresh_attempt());
      if (!advanced.ok()) {
        std::cerr << "error: could not set the initial epoch: " << advanced.to_string() << "\n";
        return 1;
      }
    }
  }

  wpf::net::ServerOptions options;
  options.limits = limits;
  if (const std::optional<std::uint64_t> timeout = arguments.get_u64("session-timeout-ms")) {
    options.session_read_timeout_ms = static_cast<std::uint32_t>(*timeout);
  }
  if (const std::optional<std::uint64_t> timeout = arguments.get_u64("frame-timeout-ms")) {
    options.frame_assembly_timeout_ms = static_cast<std::uint32_t>(*timeout);
  }
  bool persist_enabled = !store.empty();
  if (arguments.get_or("persist", "on-change") == "never") persist_enabled = false;
  if (persist_enabled) {
    options.persist = [&engine, &store, &limits]() -> wpf::Outcome {
      return wpf::save_engine_state(engine.export_state(), store, limits);
    };
  }

  wpf::net::CoordinatorServer server(engine, options);
  const std::string listen = arguments.get_or("listen", "127.0.0.1:0");
  const std::optional<wpf::net::Endpoint> endpoint = wpf::net::Endpoint::parse(listen);
  if (!endpoint.has_value()) {
    std::cerr << "error: --listen is malformed\n";
    return 1;
  }
  const wpf::Outcome started = server.start(endpoint->host, endpoint->port);
  if (!started.ok()) {
    std::cerr << "error: " << started.to_string() << "\n";
    return 1;
  }
  const wpf::net::Endpoint bound = server.endpoint();

  // The endpoint line is the contract used by operators and by the process-level
  // tests to discover an ephemeral port.
  std::cout << "ENDPOINT " << bound.to_string() << "\n";
  std::cout << "EPOCH " << engine.epoch().value() << "\n";
  std::cout << "RECOVERED " << (have_store ? 1 : 0) << "\n";
  std::cout << "SETS " << engine.set_count() << "\n";
  std::cout.flush();

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const std::optional<std::uint64_t> run_for = arguments.get_u64("run-for-ms");
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(run_for.value_or(0));
  while (!g_stop.load()) {
    if (run_for.has_value() && std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  static_cast<void>(server.stop());
  if (persist_enabled) {
    const wpf::Outcome saved = wpf::save_engine_state(engine.export_state(), store, limits);
    if (!saved.ok()) {
      std::cerr << "error: final store write failed: " << saved.to_string() << "\n";
      return 1;
    }
  }
  std::cout << "STOPPED " << engine.set_count() << "\n";
  std::cout.flush();
  return 0;
}
