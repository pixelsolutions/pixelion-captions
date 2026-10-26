// Feature: macos-platform-support
//
// Tiny self-contained property-based / unit test harness for the host-runnable
// test target (Task 8.1). No external dependency: the design scopes these tests
// to pure, in-process logic and avoids adding a third-party test framework.
//
// A "property" test runs its body for a fixed iteration count (>= 100, per the
// design) driven by a seeded PRNG so failures are reproducible. A "unit" test
// runs once. Both record pass/fail into a global registry; main() returns
// non-zero if any check failed.

#pragma once

#include <cstdint>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace pbt {

struct Stats {
	int checks = 0;
	int failures = 0;
	std::vector<std::string> failureMessages;
};

inline Stats &stats()
{
	static Stats s;
	return s;
}

// A deterministic per-test PRNG. Seed is fixed so a failing property is
// reproducible; callers derive all random inputs from it.
class Rng {
public:
	explicit Rng(uint64_t seed) : engine_(seed) {}

	uint32_t next() { return static_cast<uint32_t>(engine_()); }
	int intInRange(int lo, int hi) // inclusive
	{
		std::uniform_int_distribution<int> dist(lo, hi);
		return dist(engine_);
	}
	// Random byte string of a random length in [minLen, maxLen].
	std::string bytes(int minLen, int maxLen)
	{
		const int len = intInRange(minLen, maxLen);
		std::string out;
		out.reserve(static_cast<size_t>(len));
		for (int i = 0; i < len; ++i)
			out.push_back(static_cast<char>(intInRange(0, 255)));
		return out;
	}

private:
	std::mt19937_64 engine_;
};

// Report one check within a running test. `ok` true = pass. On failure the
// message is recorded and the enclosing test is considered failed.
inline bool check(bool ok, const std::string &context)
{
	stats().checks++;
	if (!ok) {
		stats().failures++;
		stats().failureMessages.push_back(context);
		std::cerr << "  FAIL: " << context << "\n";
	}
	return ok;
}

// Run a property test `iterations` times (default 100 per the design). The body
// receives an Rng seeded deterministically for reproducibility. The body should
// call check() for each invariant; if any check fails the property fails but
// remaining iterations still run (so the full failure surface is reported).
inline void property(const std::string &name, int iterations, const std::function<void(Rng &)> &body)
{
	std::cout << "[property] " << name << " (" << iterations << " iterations)\n";
	const int before = stats().failures;
	for (int i = 0; i < iterations; ++i) {
		Rng rng(0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(i) * 0xBF58476D1CE4E5B9ull));
		body(rng);
	}
	if (stats().failures == before)
		std::cout << "  ok\n";
}

inline void unit(const std::string &name, const std::function<void()> &body)
{
	std::cout << "[unit] " << name << "\n";
	const int before = stats().failures;
	body();
	if (stats().failures == before)
		std::cout << "  ok\n";
}

inline int summary()
{
	std::cout << "\n" << stats().checks << " checks, " << stats().failures << " failures\n";
	return stats().failures == 0 ? 0 : 1;
}

} // namespace pbt
