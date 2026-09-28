// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Deterministic pseudo random generator for property and state-machine tests.
// There is no random_device, no clock seeding and no hidden global state: the
// same seed always produces the same sequence on every platform.

#ifndef POWER_TOPOLOGY_TESTS_TEST_RNG_HPP
#define POWER_TOPOLOGY_TESTS_TEST_RNG_HPP

#include <cstdint>
#include <utility>
#include <vector>

namespace ptest {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
  }

  std::uint64_t uniform_below(std::uint64_t bound) noexcept { return bound == 0 ? 0 : next() % bound; }

  std::uint64_t uniform_range(std::uint64_t low, std::uint64_t high) noexcept {
    return high <= low ? low : low + uniform_below(high - low + 1);
  }

  bool chance(std::uint32_t numerator, std::uint32_t denominator) noexcept {
    if (denominator == 0) {
      return false;
    }
    return uniform_below(denominator) < numerator;
  }

  template <class T>
  void shuffle(std::vector<T>& items) {
    if (items.size() < 2) {
      return;
    }
    for (std::size_t index = items.size(); index > 1; --index) {
      const std::size_t other = static_cast<std::size_t>(uniform_below(index));
      std::swap(items[index - 1], items[other]);
    }
  }

  std::uint64_t state() const noexcept { return state_; }

 private:
  std::uint64_t state_;
};

}  // namespace ptest

#endif  // POWER_TOPOLOGY_TESTS_TEST_RNG_HPP
