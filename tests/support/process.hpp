// Weighted Path Fabric - real operating-system process control for tests.
// Copyright 2026 Summon Software Labs.
//
// The distributed proofs run actual processes and end them with a real
// operating-system termination. The child's lifetime is never controlled by a
// test-owned pipe: stdin is the null device.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wpftest {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Starts an executable. Returns false when the process could not be created.
  bool start(const std::string& executable, const std::vector<std::string>& arguments);

  /// Blocks until one line of standard output is available.
  std::optional<std::string> read_line();
  /// Blocks until a line starting with prefix arrives, or the stream ends.
  std::optional<std::string> wait_for_line(const std::string& prefix);

  bool running() const;
  std::uint32_t process_id() const { return pid_; }
  /// The exit code observed after the process has been reaped.
  std::uint32_t exit_code() const { return exit_code_; }

  /// Terminates the process the way an operating-system kill does.
  void kill_hard();
  /// Waits for the process to exit and returns its exit code.
  std::uint32_t wait();

  /// Every line read so far, for diagnostics.
  const std::vector<std::string>& transcript() const { return transcript_; }

 private:
  void close_handles();

  void* process_ = nullptr;
  void* thread_ = nullptr;
  void* read_pipe_ = nullptr;
  std::uint32_t pid_ = 0;
  std::string pending_;
  bool eof_ = false;
  bool reaped_ = false;
  std::uint32_t exit_code_ = 0;
  std::vector<std::string> transcript_;
};

}  // namespace wpftest
