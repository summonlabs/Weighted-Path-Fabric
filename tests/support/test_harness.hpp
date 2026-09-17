// Weighted Path Fabric - minimal deterministic test harness.
// Copyright 2026 Summon Software Labs.
//
// The harness has no notion of a timeout: a hanging test is a defect in the
// product or in the test, never something to be papered over here.
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "wpf/lifecycle.hpp"
#include "wpf/model.hpp"
#include "wpf/outcome.hpp"
#include "wpf/snapshot.hpp"
#include "wpf/types.hpp"

namespace wpftest {

struct Case {
  std::string name;
  void (*body)();
};

std::vector<Case>& registry();

struct Registrar {
  Registrar(const char* name, void (*body)());
};

/// Runs every registered case, or only those whose name contains the optional
/// filter argument. Progress goes to stderr so that a crash is locatable.
/// Returns the process exit code.
int run_all(const std::string& suite, int argc = 0, char** argv = nullptr);

/// Records a failed expectation and aborts the current case.
void fail(const char* file, int line, const std::string& message);

/// Convenience for tests that expect a specific outcome code.
void expect_code(wpf::OutcomeCode expected, const wpf::Outcome& actual, const char* file, int line);

// --- value rendering -------------------------------------------------------
template <class T>
auto describe_impl(const T& value, int) -> decltype(value.to_string()) {
  return value.to_string();
}

template <class T>
std::string describe_impl(const T& value, long) {
  std::ostringstream out;
  out << value;
  return out.str();
}

inline std::string describe(bool value) { return value ? "true" : "false"; }
inline std::string describe(const std::string& value) { return value; }
inline std::string describe(const char* value) { return std::string(value); }
inline std::string describe(const wpf::Outcome& value) { return value.to_string(); }
inline std::string describe(wpf::SetLifecycle value) { return wpf::to_string(value); }
inline std::string describe(wpf::MemberState value) { return wpf::to_string(value); }
inline std::string describe(wpf::LifecycleEvent value) { return wpf::to_string(value); }
inline std::string describe(wpf::PathLegality value) { return wpf::to_string(value); }
inline std::string describe(wpf::AuthorityScopeKind value) { return wpf::to_string(value); }
inline std::string describe(wpf::OutcomeCode value) { return wpf::to_string(value); }
inline std::string describe(wpf::DiffKind value) { return wpf::to_string(value); }

template <class T>
std::string describe(const T& value) {
  return describe_impl(value, 0);
}

// Result<T> carries an Outcome; Outcome carries itself.
template <class T>
auto failure_text_impl(const T& value, int) -> decltype(value.error().to_string()) {
  return value.error().to_string();
}

template <class T>
std::string failure_text_impl(const T& value, long) {
  return value.to_string();
}

template <class T>
std::string failure_text(const T& value) {
  return failure_text_impl(value, 0);
}

// --- temporary paths -------------------------------------------------------
/// Creates a unique directory under the system temporary directory. Never a
/// fixed path, so parallel test processes cannot collide.
std::string unique_temp_dir(const std::string& tag);
/// Removes a directory tree created by unique_temp_dir, ignoring absence.
void remove_tree(const std::string& path);

/// Deterministic pseudo-random generator so failures are reproducible.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    state_ = wpf::splitmix64(state_);
    return state_;
  }
  std::uint64_t uniform(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }
  std::uint64_t seed() const { return seed_; }

 private:
  std::uint64_t state_;
  std::uint64_t seed_ = 0;
};

}  // namespace wpftest

#define WPF_TEST(name)                                                          \
  static void wpf_test_##name();                                                \
  static ::wpftest::Registrar wpf_registrar_##name(#name, &wpf_test_##name);    \
  static void wpf_test_##name()

#define WPF_CHECK(cond)                                                       \
  do {                                                                        \
    if (!(cond)) ::wpftest::fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
  } while (false)

#define WPF_CHECK_EQ(a, b)                                                     \
  do {                                                                         \
    const auto& wpf_lhs_value = (a);                                           \
    const auto& wpf_rhs_value = (b);                                           \
    if (!(wpf_lhs_value == wpf_rhs_value)) {                                   \
      ::wpftest::fail(__FILE__, __LINE__,                                      \
                      std::string("CHECK_EQ failed: " #a " == " #b " (") +     \
                          ::wpftest::describe(wpf_lhs_value) + " vs " +        \
                          ::wpftest::describe(wpf_rhs_value) + ")");           \
    }                                                                          \
  } while (false)

#define WPF_CHECK_OK(expr)                                                          \
  do {                                                                              \
    const auto& wpf_result_value = (expr);                                          \
    if (!wpf_result_value.ok()) {                                                   \
      ::wpftest::fail(__FILE__, __LINE__, std::string("expected success from "      \
                                                      #expr " but got ") +          \
                                              ::wpftest::failure_text(wpf_result_value)); \
    }                                                                               \
  } while (false)

#define WPF_EXPECT_CODE(expected, expr) \
  ::wpftest::expect_code((expected), (expr), __FILE__, __LINE__)

#define WPF_TEST_MAIN(suite)                                            \
  int main(int argc, char** argv) { return ::wpftest::run_all(suite, argc, argv); }
