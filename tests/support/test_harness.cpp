// Weighted Path Fabric - minimal deterministic test harness.
// Copyright 2026 Summon Software Labs.
#include "test_harness.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace wpftest {
namespace {

struct Failure {
  std::string text;
};

std::uint64_t process_id() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t counter() {
  static std::uint64_t value = 0;
  return ++value;
}

}  // namespace

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

Registrar::Registrar(const char* name, void (*body)()) { registry().push_back(Case{name, body}); }

void fail(const char* file, int line, const std::string& message) {
  throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + message};
}

void expect_code(wpf::OutcomeCode expected, const wpf::Outcome& actual, const char* file, int line) {
  if (actual.code() == expected) return;
  fail(file, line, std::string("expected outcome ") + wpf::to_string(expected) + " but got " +
                       wpf::to_string(actual.code()) + " (" + actual.detail() + ")");
}

std::string unique_temp_dir(const std::string& tag) {
  const std::filesystem::path base = std::filesystem::temp_directory_path();
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::string name = "wpf-" + tag + "-" + std::to_string(process_id()) + "-" +
                             std::to_string(counter());
    const std::filesystem::path candidate = base / name;
    std::error_code error;
    if (std::filesystem::create_directories(candidate, error)) {
      return candidate.string();
    }
  }
  throw Failure{"could not create a unique temporary directory"};
}

void remove_tree(const std::string& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

int run_all(const std::string& suite, int argc, char** argv) {
  std::size_t passed = 0;
  std::vector<std::string> failures;
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    filter = (argv[index] == nullptr) ? std::string() : std::string(argv[index]);
  }
  for (const Case& test_case : registry()) {
    if (!filter.empty() && test_case.name.find(filter) == std::string::npos) continue;
    std::cerr << "RUN " << suite << "." << test_case.name << std::endl;
    try {
      test_case.body();
      ++passed;
    } catch (const Failure& failure) {
      failures.push_back(test_case.name + ": " + failure.text);
      std::cout << "FAIL " << suite << "." << test_case.name << "\n  " << failure.text << "\n";
    } catch (const std::exception& error) {
      failures.push_back(test_case.name + ": unexpected exception: " + error.what());
      std::cout << "FAIL " << suite << "." << test_case.name
                << "\n  unexpected exception: " << error.what() << "\n";
    }
  }
  std::cout << suite << ": " << passed << "/" << registry().size() << " cases passed\n";
  if (!failures.empty()) {
    std::cout << suite << ": " << failures.size() << " failing cases\n";
    return 1;
  }
  return 0;
}

}  // namespace wpftest
