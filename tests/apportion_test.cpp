// Weighted Path Fabric - apportionment tests against an independent oracle.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <string>
#include <vector>

#include "oracle.hpp"
#include "test_harness.hpp"
#include "wpf/apportion.hpp"

using namespace wpf;

namespace {

using WeightList = std::vector<std::pair<WeightedMemberId, WeightValue>>;

WeightList weights(std::initializer_list<WeightValue> values) {
  WeightList out;
  std::uint64_t id = 1;
  for (WeightValue value : values) {
    out.emplace_back(WeightedMemberId::from_rep(id), value);
    id += 1;
  }
  return out;
}

std::vector<std::uint32_t> seats_of(const Apportionment& apportionment) {
  std::vector<std::uint32_t> out;
  for (const SlotQuota& quota : apportionment.quotas) out.push_back(quota.seats);
  return out;
}

Apportionment must_apportion(std::uint32_t space, const WeightList& list) {
  const Result<Apportionment> result =
      apportion(*SelectionSpaceSize::make(space), list, 1024);
  WPF_CHECK(result.ok());
  return result.value();
}

void compare_with_oracle(std::uint32_t space, const WeightList& list) {
  const SelectionSpaceSize size = *SelectionSpaceSize::make(space);
  const Result<Apportionment> product = apportion(size, list, 1024);
  const Result<Apportionment> reference = wpforacle::apportion(size, list);
  WPF_CHECK(product.ok());
  WPF_CHECK(reference.ok());
  WPF_CHECK_EQ(product.value().total_seats, reference.value().total_seats);
  WPF_CHECK_EQ(product.value().total_weight, reference.value().total_weight);
  for (std::size_t i = 0; i < product.value().quotas.size(); ++i) {
    WPF_CHECK_EQ(product.value().quotas[i].seats, reference.value().quotas[i].seats);
    WPF_CHECK_EQ(product.value().quotas[i].exact_floor, reference.value().quotas[i].exact_floor);
    WPF_CHECK_EQ(product.value().quotas[i].member.value(),
                 reference.value().quotas[i].member.value());
  }
  std::uint32_t total = 0;
  for (const SlotQuota& quota : product.value().quotas) total += quota.seats;
  WPF_CHECK_EQ(total, space);
}

}  // namespace

WPF_TEST(pinned_64_slot_distributions) {
  WPF_CHECK(seats_of(must_apportion(64, weights({1, 1}))) == std::vector<std::uint32_t>({32, 32}));
  WPF_CHECK(seats_of(must_apportion(64, weights({1, 3}))) == std::vector<std::uint32_t>({16, 48}));
  WPF_CHECK(seats_of(must_apportion(64, weights({1, 2, 1}))) ==
            std::vector<std::uint32_t>({16, 32, 16}));
  // 64 / 3 has three equal remainders, so the tie breaks by ascending member identity.
  WPF_CHECK(seats_of(must_apportion(64, weights({1, 1, 1}))) ==
            std::vector<std::uint32_t>({22, 21, 21}));
  WPF_CHECK(seats_of(must_apportion(64, weights({7, 2, 1}))) ==
            std::vector<std::uint32_t>({45, 13, 6}));
  WPF_CHECK(seats_of(must_apportion(64, weights({50, 30, 20}))) ==
            std::vector<std::uint32_t>({32, 19, 13}));
}

WPF_TEST(awkward_ratios_are_deterministic) {
  WPF_CHECK(seats_of(must_apportion(7, weights({1, 1, 1}))) ==
            std::vector<std::uint32_t>({3, 2, 2}));
  // 13 slots over 5:3:2 -> ideal 6.5 / 3.9 / 2.6; the two largest remainders
  // (.9 and .6) go to the second and third member.
  WPF_CHECK(seats_of(must_apportion(13, weights({5, 3, 2}))) ==
            std::vector<std::uint32_t>({6, 4, 3}));
  // 17 slots over 7:5:3:1 sums exactly to 16, so the single leftover seat goes
  // to the largest remainder (.4375).
  WPF_CHECK(seats_of(must_apportion(17, weights({7, 5, 3, 1}))) ==
            std::vector<std::uint32_t>({8, 5, 3, 1}));
  WPF_CHECK(seats_of(must_apportion(31, weights({7, 11, 13}))) ==
            std::vector<std::uint32_t>({7, 11, 13}));
  WPF_CHECK(seats_of(must_apportion(31, weights({13, 11, 7}))) ==
            std::vector<std::uint32_t>({13, 11, 7}));
  WPF_CHECK(seats_of(must_apportion(1024, weights({1, 1, 1}))) ==
            std::vector<std::uint32_t>({342, 341, 341}));
}

WPF_TEST(scale_equivalence_is_exact) {
  const Apportionment primitive = must_apportion(4096, weights({1, 2, 3}));
  const Apportionment tenfold = must_apportion(4096, weights({10, 20, 30}));
  const Apportionment thousandfold = must_apportion(4096, weights({1000, 2000, 3000}));
  WPF_CHECK(seats_of(primitive) == seats_of(tenfold));
  WPF_CHECK(seats_of(primitive) == seats_of(thousandfold));
  WPF_CHECK_EQ(primitive.total_weight, tenfold.total_weight);
  WPF_CHECK_EQ(primitive.total_weight, thousandfold.total_weight);
}

WPF_TEST(quota_property_holds_for_every_member) {
  for (std::uint32_t space = 1; space <= 129; ++space) {
    for (WeightValue a = 1; a <= 5; ++a) {
      for (WeightValue b = 1; b <= 5; ++b) {
        for (WeightValue c = 1; c <= 5; ++c) {
          const WeightList list = weights({a, b, c});
          compare_with_oracle(space, list);
          const Apportionment result = must_apportion(space, list);
          for (const SlotQuota& quota : result.quotas) {
            WPF_CHECK(quota.seats >= quota.exact_floor);
            WPF_CHECK(quota.seats <= quota.exact_floor + 1);
          }
        }
      }
    }
  }
}

WPF_TEST(input_order_never_changes_the_result) {
  const WeightList ordered = weights({50, 30, 20});
  WeightList reversed(ordered.rbegin(), ordered.rend());
  const Apportionment left = must_apportion(256, ordered);
  const Apportionment right = must_apportion(256, reversed);
  WPF_CHECK(seats_of(left) == seats_of(right));
  for (std::size_t i = 0; i < left.quotas.size(); ++i) {
    WPF_CHECK(left.quotas[i].member == right.quotas[i].member);
  }
}

WPF_TEST(zero_weight_members_receive_nothing) {
  const WeightList list = weights({5, 0, 5});
  const Apportionment result = must_apportion(64, list);
  WPF_CHECK_EQ(result.quotas[1].seats, 0u);
  WPF_CHECK_EQ(result.quotas[1].exact_floor, 0ull);
  WPF_CHECK_EQ(result.quotas[1].exact_remainder, 0ull);
  std::uint32_t total = 0;
  for (const SlotQuota& quota : result.quotas) total += quota.seats;
  WPF_CHECK_EQ(total, 64u);
}

WPF_TEST(rejections_are_structured) {
  const WeightList all_zero = weights({0, 0, 0});
  WPF_EXPECT_CODE(OutcomeCode::AllZeroWeight,
                  apportion(*SelectionSpaceSize::make(64), all_zero, 1024).error());

  WeightList duplicate = weights({1, 2});
  duplicate[1].first = duplicate[0].first;
  WPF_EXPECT_CODE(OutcomeCode::DuplicateMember,
                  apportion(*SelectionSpaceSize::make(64), duplicate, 1024).error());

  WPF_EXPECT_CODE(OutcomeCode::InvalidSelectionSpace,
                  apportion(SelectionSpaceSize{}, weights({1, 1}), 1024).error());
  WPF_EXPECT_CODE(OutcomeCode::ResourceLimit,
                  apportion(*SelectionSpaceSize::make(64), weights({1, 1, 1, 1}), 3).error());
  WPF_EXPECT_CODE(OutcomeCode::InvalidIdentity,
                  apportion(*SelectionSpaceSize::make(64),
                            WeightList{{WeightedMemberId{}, 1}}, 1024)
                      .error());
}

WPF_TEST(large_weights_are_checked_not_wrapped) {
  // The two largest representable weights are coprime, so their canonical total
  // does not fit in 64 bits and the request must be rejected rather than wrapped.
  const WeightList extreme = weights({kWeightValueMaxValue, kWeightValueMaxValue - 1});
  WPF_EXPECT_CODE(OutcomeCode::WeightOverflow,
                  apportion(*SelectionSpaceSize::make(4096), extreme, 1024).error());

  // Large but reducible weights stay exact.
  const WeightValue huge = 1ull << 40;
  const WeightList reducible = weights({huge, huge, huge * 2});
  const Result<Apportionment> result = apportion(*SelectionSpaceSize::make(4096), reducible, 1024);
  WPF_CHECK(result.ok());
  std::uint32_t total = 0;
  for (const SlotQuota& quota : result.value().quotas) total += quota.seats;
  WPF_CHECK_EQ(total, 4096u);
  compare_with_oracle(4096, reducible);

  // Large coprime weights whose exact 128-bit product still divides cleanly.
  const WeightList coprime = weights({(1ull << 62) + 1, (1ull << 62) + 3});
  const Result<Apportionment> coprime_result =
      apportion(*SelectionSpaceSize::make(1024), coprime, 1024);
  WPF_CHECK(coprime_result.ok());
  std::uint32_t coprime_total = 0;
  for (const SlotQuota& quota : coprime_result.value().quotas) coprime_total += quota.seats;
  WPF_CHECK_EQ(coprime_total, 1024u);
}

WPF_TEST_MAIN("apportion")
