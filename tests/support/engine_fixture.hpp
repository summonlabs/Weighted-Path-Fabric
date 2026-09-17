// Weighted Path Fabric - shared engine test fixture.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_harness.hpp"
#include "wpf/engine.hpp"

namespace wpftest {

/// One engine with one registered publisher. The engine is non-movable, so the
/// fixture is always constructed in place and never copied.
struct Fixture {
  wpf::WeightedFabricEngine engine;
  wpf::PublisherId publisher = wpf::PublisherId::from_rep(1);
  wpf::WorkerBootId boot = wpf::WorkerBootId::from_rep(1000);
  wpf::AuthorityScope scope = *wpf::AuthorityScope::parse("fabric:prod");
  std::uint64_t counter = 1;

  explicit Fixture(wpf::EngineConfig config = wpf::EngineConfig{}) : engine(std::move(config)) {
    wpf::RegisterPublisherRequest request;
    request.publisher = publisher;
    request.boot = boot;
    request.epoch = engine.epoch();
    request.scope = scope;
    request.attempt = wpf::MutationAttemptId::from_seed(counter++);
    const wpf::Result<wpf::PublisherAuthority> registered = engine.register_publisher(request);
    if (!registered.ok()) throw std::runtime_error("fixture registration failed");
  }

  wpf::MutationAttemptId attempt() { return wpf::MutationAttemptId::from_seed(counter++); }

  wpf::MutationContext context() {
    wpf::MutationContext ctx;
    ctx.epoch = engine.epoch();
    ctx.publisher = publisher;
    ctx.boot = boot;
    ctx.scope = scope;
    ctx.attempt = attempt();
    return ctx;
  }

  wpf::IntegrationContext integration() {
    wpf::IntegrationContext ctx;
    ctx.epoch = engine.epoch();
    ctx.publisher = publisher;
    ctx.boot = boot;
    ctx.attempt = attempt();
    return ctx;
  }

  void reregister(wpf::WorkerBootId fresh, wpf::AuthorityScope new_scope) {
    boot = fresh;
    scope = new_scope;
    wpf::RegisterPublisherRequest request;
    request.publisher = publisher;
    request.boot = boot;
    request.epoch = engine.epoch();
    request.scope = scope;
    request.attempt = attempt();
    const wpf::Result<wpf::PublisherAuthority> registered = engine.register_publisher(request);
    if (!registered.ok()) throw std::runtime_error("re-registration failed");
  }

  static wpf::SetKey key(const char* name, const char* fabric = "prod", const char* ns = "edge") {
    wpf::SetKey value;
    value.fabric = *wpf::FabricId::parse(fabric);
    value.routing_namespace = *wpf::RoutingNamespaceId::parse(ns);
    value.policy_name = *wpf::PolicyName::parse(name);
    return value;
  }

  static wpf::MemberSpec member(std::uint64_t path, wpf::WeightValue weight,
                                std::uint64_t authority = 1) {
    wpf::MemberSpec spec;
    spec.path = wpf::PathId::from_rep(path);
    spec.path_authority = wpf::PathAuthorityGeneration::from_rep(authority);
    spec.declared_weight = weight;
    return spec;
  }

  wpf::WeightedPathSetId create(const wpf::SetKey& set_key,
                                const std::vector<wpf::MemberSpec>& members,
                                std::uint32_t space = 64, std::uint32_t minimum = 1,
                                wpf::WeightBounds bounds = wpf::WeightBounds{}) {
    wpf::CreateSetRequest request;
    request.key = set_key;
    request.space = *wpf::SelectionSpaceSize::make(space);
    request.bounds = bounds;
    request.min_effective_members = minimum;
    request.members = members;
    request.context = context();
    const wpf::Result<wpf::MutationReport> report = engine.create_set(request);
    if (!report.ok()) throw std::runtime_error("create failed: " + report.error().to_string());
    return report.value().set;
  }
};

/// A=50 B=30 C=20 over the requested selection space on paths 101, 202 and 303.
struct Established {
  Fixture fixture;
  wpf::WeightedPathSetId set;

  explicit Established(std::uint32_t space = 64, std::uint32_t minimum = 1) {
    set = fixture.create(Fixture::key("primary"),
                         {Fixture::member(101, 50), Fixture::member(202, 30),
                          Fixture::member(303, 20)},
                         space, minimum);
  }
};

inline wpf::WeightedMemberId member_for(const wpf::SetSnapshot& snapshot, std::uint64_t path) {
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    if (member.path.value() == path) return member.id;
  }
  throw std::runtime_error("member not found");
}

inline std::vector<std::uint32_t> seats_of(const wpf::SetSnapshot& snapshot) {
  std::vector<std::uint32_t> seats;
  for (const wpf::MemberSnapshot& member : snapshot.members) seats.push_back(member.seats);
  return seats;
}

inline wpf::SetSnapshot require_set(wpf::WeightedFabricEngine& engine, wpf::WeightedPathSetId id) {
  const std::optional<wpf::SetSnapshot> snapshot = engine.get_set(id);
  if (!snapshot.has_value()) throw std::runtime_error("set not found");
  return *snapshot;
}

}  // namespace wpftest
