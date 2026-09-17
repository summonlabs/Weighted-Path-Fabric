// Weighted Path Fabric - real operating-system process control for tests.
// Copyright 2026 Summon Software Labs.
#include "process.hpp"

#include <windows.h>

#include <stdexcept>

namespace wpftest {
namespace {

/// Quotes one argument for the Windows command line.
std::string quote(const std::string& argument) {
  if (argument.find_first_of(" \t\"") == std::string::npos) return argument;
  std::string out = "\"";
  for (char character : argument) {
    if (character == '"') out += "\\\"";
    else out.push_back(character);
  }
  out += "\"";
  return out;
}

}  // namespace

ChildProcess::~ChildProcess() {
  if (process_ != nullptr && running()) kill_hard();
  close_handles();
}

void ChildProcess::close_handles() {
  if (read_pipe_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(read_pipe_));
    read_pipe_ = nullptr;
  }
  if (thread_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(thread_));
    thread_ = nullptr;
  }
  if (process_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
}

bool ChildProcess::start(const std::string& executable, const std::vector<std::string>& arguments) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!::CreatePipe(&read_end, &write_end, &attributes, 0)) return false;
  ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  // The child's standard input is the null device, so no test-owned pipe can
  // keep it alive or deadlock it.
  SECURITY_ATTRIBUTES null_attributes{};
  null_attributes.nLength = sizeof(null_attributes);
  null_attributes.bInheritHandle = TRUE;
  HANDLE null_input = ::CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &null_attributes, OPEN_EXISTING, 0, nullptr);

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  startup.hStdInput = null_input;

  // The child derives argv[0] from the command line, so the executable must be
  // the first token even though lpApplicationName is also supplied.
  std::string command_line = quote(executable);
  for (const std::string& argument : arguments) {
    command_line.push_back(' ');
    command_line += quote(argument);
  }

  PROCESS_INFORMATION information{};
  const BOOL created =
      ::CreateProcessA(executable.c_str(), command_line.empty() ? nullptr : command_line.data(),
                       nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &information);
  ::CloseHandle(write_end);
  if (null_input != INVALID_HANDLE_VALUE) ::CloseHandle(null_input);
  if (!created) {
    ::CloseHandle(read_end);
    return false;
  }
  process_ = information.hProcess;
  thread_ = information.hThread;
  pid_ = information.dwProcessId;
  read_pipe_ = read_end;
  return true;
}

std::optional<std::string> ChildProcess::read_line() {
  while (true) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      transcript_.push_back(line);
      return line;
    }
    if (eof_) {
      if (!pending_.empty()) {
        std::string line = pending_;
        pending_.clear();
        transcript_.push_back(line);
        return line;
      }
      return std::nullopt;
    }
    char buffer[512];
    DWORD read = 0;
    if (!::ReadFile(static_cast<HANDLE>(read_pipe_), buffer, sizeof(buffer), &read, nullptr) ||
        read == 0) {
      eof_ = true;
      continue;
    }
    pending_.append(buffer, read);
  }
}

std::optional<std::string> ChildProcess::wait_for_line(const std::string& prefix) {
  while (true) {
    const std::optional<std::string> line = read_line();
    if (!line.has_value()) return std::nullopt;
    if (line->rfind(prefix, 0) == 0) return line;
  }
}

bool ChildProcess::running() const {
  if (process_ == nullptr || reaped_) return false;
  return ::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
}

void ChildProcess::kill_hard() {
  if (process_ == nullptr) return;
  ::TerminateProcess(static_cast<HANDLE>(process_), 1);
  if (!reaped_) {
    ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    exit_code_ = code;
    reaped_ = true;
  }
}

std::uint32_t ChildProcess::wait() {
  if (process_ == nullptr) return exit_code_;
  if (!reaped_) {
    ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    exit_code_ = code;
    reaped_ = true;
  }
  return exit_code_;
}

}  // namespace wpftest
