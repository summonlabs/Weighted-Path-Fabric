// Weighted Path Fabric - identity, arithmetic and digest tests.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "test_harness.hpp"
#include "wpf/digest.hpp"
#include "wpf/model.hpp"
#include "wpf/version.hpp"
#include "wpf/weight.hpp"

using namespace wpf;

namespace {

std::string sha(std::string_view text) { return Sha256::hash(text).hex(); }

}  // namespace

WPF_TEST(sha256_known_vectors) {
  WPF_CHECK_EQ(sha(""),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  WPF_CHECK_EQ(sha("abc"),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  WPF_CHECK_EQ(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
               std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  const std::string million(1000000, 'a');
  WPF_CHECK_EQ(sha(million),
               std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

WPF_TEST(digest_builder_is_tagged_and_deterministic) {
  DigestBuilder left;
  left.field("a", std::uint64_t(1));
  left.field("b", std::uint64_t(2));
  DigestBuilder right;
  right.field("a", std::uint64_t(1));
  right.field("b", std::uint64_t(2));
  DigestBuilder swapped;
  swapped.field("b", std::uint64_t(2));
  swapped.field("a", std::uint64_t(1));
  WPF_CHECK(left.finalize() == right.finalize());
  WPF_CHECK(!(left.finalize() == swapped.finalize()));
  DigestBuilder shorter;
  shorter.field("a", std::uint64_t(1));
  WPF_CHECK(!(shorter.finalize() == left.finalize()));
  DigestBuilder typed;
  typed.field("a", std::uint32_t(1));
  WPF_CHECK(!(typed.finalize() == shorter.finalize()));
}

WPF_TEST(uint128_arithmetic_is_checked) {
  const UInt128 product = UInt128::widen_mul(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull);
  WPF_CHECK_EQ(product.high(), 0xFFFFFFFFFFFFFFFEull);
  WPF_CHECK_EQ(product.low(), 1ull);

  const auto added = UInt128::add(UInt128(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull),
                                  UInt128::from_u64(1));
  WPF_CHECK(!added.has_value());

  const auto multiplied = UInt128::mul(UInt128(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull), 2);
  WPF_CHECK(!multiplied.has_value());

  WPF_CHECK(UInt128::from_u64(3).fits_u64());
  WPF_CHECK(!UInt128(1, 0).fits_u64());

  const auto division = UInt128::divmod_u64(UInt128::from_u64(1000), 7);
  WPF_CHECK_EQ(division.second, 6ull);
  WPF_CHECK_EQ(division.first.to_u64_checked().value(), 142ull);

  const UInt128 big = UInt128::widen_mul(0xFFFFFFFFFFFFFFFFull, 3);
  const auto big_division = UInt128::divmod_u64(big, 0xFFFFFFFFFFFFFFFFull);
  WPF_CHECK_EQ(big_division.first.to_u64_checked().value(), 3ull);
  WPF_CHECK_EQ(big_division.second, 0ull);
  WPF_CHECK_EQ(big.to_string(), std::string("55340232221128654845"));

  const UInt128 full = UInt128::add(UInt128(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFEull),
                                    UInt128::from_u64(1))
                           .value();
  WPF_CHECK_EQ(full.high(), 0xFFFFFFFFFFFFFFFFull);
  WPF_CHECK_EQ(full.low(), 0xFFFFFFFFFFFFFFFFull);
  WPF_CHECK_EQ(full.to_string(), std::string("340282366920938463463374607431768211455"));
}

WPF_TEST(strong_identities_reject_zero) {
  WPF_CHECK(!WeightedPathSetId{}.valid());
  WPF_CHECK(!WeightedPathSetId::make(0).has_value());
  WPF_CHECK(WeightedPathSetId::make(7).has_value());
  WPF_CHECK(!WeightedMemberId::make(0).has_value());
  WPF_CHECK_EQ(PathId::make(1).value().value(), 1ull);
  WPF_CHECK(std::hash<PathId>{}(PathId::from_rep(1)) == std::hash<PathId>{}(PathId::from_rep(1)));

  // Slot zero is a legitimate ordinal.
  WPF_CHECK_EQ(SelectionSlotId::from_rep(0).value(), 0u);
  WPF_CHECK_EQ(SelectionSlotId::from_rep(9).value(), 9u);

  WPF_CHECK(!MutationAttemptId{}.valid());
  WPF_CHECK(MutationAttemptId::make(0, 0).has_value() == false);
  WPF_CHECK(MutationAttemptId::from_seed(1).valid());
  WPF_CHECK(MutationAttemptId::from_seed(1) == MutationAttemptId::from_seed(1));
  WPF_CHECK(!(MutationAttemptId::from_seed(1) == MutationAttemptId::from_seed(2)));
}

WPF_TEST(generations_never_wrap) {
  WeightPolicyGeneration generation = WeightPolicyGeneration::initial();
  WPF_CHECK_EQ(generation.value(), 1ull);
  generation = generation.next().value();
  WPF_CHECK_EQ(generation.value(), 2ull);
  const WeightPolicyGeneration maximum(std::numeric_limits<std::uint64_t>::max());
  WPF_CHECK(!maximum.next().has_value());
  WPF_CHECK(!WeightPolicyGeneration{}.valid());
  WPF_CHECK(!WeightPolicyGeneration::make(0).has_value());
  WPF_CHECK(CoordinatorEpoch::make(9).value() < CoordinatorEpoch::make(10).value());
}

WPF_TEST(worker_boot_ids_are_fresh_and_nonzero) {
  const WorkerBootId first = generate_worker_boot_id();
  const WorkerBootId second = generate_worker_boot_id();
  WPF_CHECK(first.valid());
  WPF_CHECK(second.valid());
  WPF_CHECK(!(first == second));
}

WPF_TEST(names_are_validated) {
  WPF_CHECK(FabricId::parse("prod-fabric").has_value());
  WPF_CHECK(FabricId::parse("a.b_c:d-1").has_value());
  WPF_CHECK(!FabricId::parse("").has_value());
  WPF_CHECK(!FabricId::parse("has space").has_value());
  WPF_CHECK(!FabricId::parse("semi;colon").has_value());
  WPF_CHECK(!FabricId::parse(std::string(129, 'a')).has_value());

  SetKey key;
  key.fabric = *FabricId::parse("prod");
  key.routing_namespace = *RoutingNamespaceId::parse("edge");
  key.policy_name = *PolicyName::parse("primary");
  WPF_CHECK_EQ(key.canonical(), std::string("prod|edge|-|-|primary"));
  key.multipath = MultipathSetId::from_rep(9);
  WPF_CHECK_EQ(key.canonical(), std::string("prod|edge|-|9|primary"));
  WPF_CHECK(!(key == SetKey{}));
}

WPF_TEST(member_identity_is_order_independent) {
  SetKey key;
  key.fabric = *FabricId::parse("prod");
  key.routing_namespace = *RoutingNamespaceId::parse("edge");
  key.policy_name = *PolicyName::parse("primary");
  const WeightedMemberId first = derive_member_id(key, PathId::from_rep(11));
  const WeightedMemberId second = derive_member_id(key, PathId::from_rep(11));
  WPF_CHECK(first == second);
  WPF_CHECK(first.valid());
  WPF_CHECK(!(derive_member_id(key, PathId::from_rep(12)) == first));

  SetKey other = key;
  other.policy_name = *PolicyName::parse("secondary");
  WPF_CHECK(!(derive_member_id(other, PathId::from_rep(11)) == first));

  // A large path population must not produce identity collisions within a set.
  std::vector<WeightedMemberId> ids;
  for (std::uint64_t path = 1; path <= 20000; ++path) {
    ids.push_back(derive_member_id(key, PathId::from_rep(path)));
  }
  std::sort(ids.begin(), ids.end());
  for (std::size_t i = 1; i < ids.size(); ++i) WPF_CHECK(!(ids[i] == ids[i - 1]));
}

WPF_TEST(gcd_and_canonicalization) {
  WPF_CHECK_EQ(gcd_of_weights({100, 200, 300}), 100ull);
  WPF_CHECK_EQ(gcd_of_weights({7, 11, 13}), 1ull);
  WPF_CHECK_EQ(gcd_of_weights({0, 0, 0}), 0ull);
  WPF_CHECK_EQ(gcd_of_weights({5, 0, 0}), 5ull);
  WPF_CHECK_EQ(gcd_of_weights({0, 4}), 4ull);

  const Result<CanonicalRatio> primitive = canonicalize_weights({1, 2, 3});
  WPF_CHECK_OK(primitive);
  WPF_CHECK_EQ(primitive.value().total, 6ull);
  WPF_CHECK_EQ(primitive.value().divisor, 1ull);

  const Result<CanonicalRatio> scaled = canonicalize_weights({1000, 2000, 3000});
  WPF_CHECK_OK(scaled);
  WPF_CHECK_EQ(scaled.value().total, 6ull);
  WPF_CHECK_EQ(scaled.value().divisor, 1000ull);
  WPF_CHECK(scaled.value().weights == primitive.value().weights);

  const Result<CanonicalRatio> single = canonicalize_weights({0, 7, 0});
  WPF_CHECK_OK(single);
  WPF_CHECK_EQ(single.value().total, 1ull);
  WPF_CHECK(single.value().weights[1] == 1ull);

  const Result<CanonicalRatio> zero = canonicalize_weights({0, 0});
  WPF_EXPECT_CODE(OutcomeCode::AllZeroWeight, zero.error());

  // 2^63, 2^63, 1 has gcd 1 and a canonical total above 64 bits.
  const WeightValue huge = 1ull << 63;
  const Result<CanonicalRatio> overflow = canonicalize_weights({huge, huge, 1});
  WPF_EXPECT_CODE(OutcomeCode::WeightOverflow, overflow.error());

  // 2^63, 2^63 reduces to 1:1 and fits comfortably.
  const Result<CanonicalRatio> reducible = canonicalize_weights({huge, huge});
  WPF_CHECK_OK(reducible);
  WPF_CHECK_EQ(reducible.value().total, 2ull);

  const Result<CanonicalRatio> coprime = canonicalize_weights({7, 11, 13});
  WPF_CHECK_OK(coprime);
  WPF_CHECK_EQ(coprime.value().total, 31ull);

  const Result<WeightValue> sum = checked_weight_sum({huge, huge});
  WPF_EXPECT_CODE(OutcomeCode::WeightOverflow, sum.error());
  WPF_CHECK_EQ(checked_weight_sum({1, 2, 3}).value(), 6ull);
}

WPF_TEST(normalized_shares_sum_exactly) {
  const Result<CanonicalRatio> ratio = canonicalize_weights({50, 30, 20});
  WPF_CHECK_OK(ratio);
  const std::vector<NormalizedShare> shares = normalized_shares(ratio.value());
  WPF_CHECK_EQ(shares.size(), std::size_t(3));
  WPF_CHECK_EQ(shares[0].numerator, 5ull);
  WPF_CHECK_EQ(shares[0].denominator, 10ull);
  WPF_CHECK(sum_share_numerators(shares) == UInt128::from_u64(shares[0].denominator));

  const Result<CanonicalRatio> with_zero = canonicalize_weights({1, 0, 3});
  WPF_CHECK_OK(with_zero);
  const std::vector<NormalizedShare> zero_shares = normalized_shares(with_zero.value());
  WPF_CHECK_EQ(zero_shares.size(), std::size_t(3));
  WPF_CHECK_EQ(zero_shares[1].numerator, 0ull);
  WPF_CHECK_EQ(zero_shares[1].denominator, 4ull);
  WPF_CHECK(zero_shares[1].is_zero());
  WPF_CHECK(sum_share_numerators(zero_shares) == UInt128::from_u64(4));
}

WPF_TEST(weight_bounds_enforced) {
  WeightBounds bounds;
  bounds.minimum_positive = 10;
  bounds.maximum = 100;
  WPF_CHECK(bounds.validate(10, true).ok());
  WPF_CHECK(bounds.validate(100, true).ok());
  WPF_CHECK(bounds.validate(0, true).ok());
  WPF_EXPECT_CODE(OutcomeCode::InvalidWeight, bounds.validate(0, false));
  WPF_EXPECT_CODE(OutcomeCode::InvalidWeight, bounds.validate(9, true));
  WPF_EXPECT_CODE(OutcomeCode::InvalidWeight, bounds.validate(101, true));
  WeightBounds broken;
  broken.minimum_positive = 5;
  broken.maximum = 1;
  WPF_CHECK(!broken.valid());
}

WPF_TEST(percent_rendering_is_exact_enough) {
  NormalizedShare third;
  third.numerator = 1;
  third.denominator = 3;
  WPF_CHECK_EQ(third.percent_string(2), std::string("33.33%"));
  NormalizedShare half;
  half.numerator = 1;
  half.denominator = 2;
  WPF_CHECK_EQ(half.percent_string(0), std::string("50%"));
  WPF_CHECK_EQ(half.to_string(), std::string("1/2"));
  NormalizedShare two_thirds;
  two_thirds.numerator = 2;
  two_thirds.denominator = 3;
  WPF_CHECK_EQ(two_thirds.percent_string(2), std::string("66.67%"));
}

WPF_TEST(version_contract_is_coherent) {
  WPF_CHECK_EQ(std::string(kVersionString), std::string("1.0.0"));
  WPF_CHECK_EQ(kVersionMajor, 1);
  WPF_CHECK_EQ(kVersionMinor, 0);
  WPF_CHECK_EQ(kVersionPatch, 0);
  const std::string report = version_report();
  WPF_CHECK(report.find("1.0.0") != std::string::npos);
  WPF_CHECK(report.find(kApportionmentAlgorithmName) != std::string::npos);

  // The build system's idea of every versioned contract must match the headers.
  WPF_CHECK_EQ(std::string(WPF_CMAKE_PRODUCT_VERSION), std::string(kVersionString));
  WPF_CHECK_EQ(static_cast<std::uint16_t>(WPF_CMAKE_WIRE_PROTOCOL_VERSION), kWireProtocolVersion);
  WPF_CHECK_EQ(static_cast<std::uint16_t>(WPF_CMAKE_PERSISTENCE_FORMAT_VERSION),
               kPersistenceFormatVersion);
}

WPF_TEST(outcome_codes_are_stable) {
  WPF_CHECK(is_success(OutcomeCode::Ok));
  WPF_CHECK(is_success(OutcomeCode::Created));
  WPF_CHECK(!is_success(OutcomeCode::StaleEpoch));
  WPF_CHECK_EQ(std::string(to_string(OutcomeCode::AllZeroWeight)), std::string("ALL_ZERO_WEIGHT"));
  WPF_CHECK_EQ(std::string(to_string(OutcomeCode::StalePathAuthority)),
               std::string("STALE_PATH_AUTHORITY"));
  Outcome failure(OutcomeCode::InvalidWeight, "too small");
  WPF_CHECK(!failure.ok());
  WPF_CHECK_EQ(failure.to_string(), std::string("INVALID_WEIGHT: too small"));
}

WPF_TEST_MAIN("types")
