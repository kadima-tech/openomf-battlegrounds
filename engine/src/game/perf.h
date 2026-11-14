// Diagnostic zone timer (OPENBG_DEBUG_MEMORY): PERF_ZONE("name") adds the scope's wall time and a
// call to that zone; perf::report() prints every zone's ms per frame and calls per frame, then resets.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace perf {

struct Zone {
	double ms = 0;
	unsigned long long calls = 0;
};

inline bool enabled() {
	static const bool on = getenv("OPENBG_DEBUG_MEMORY") != nullptr;
	return on;
}

inline std::map<std::string, Zone> &zones() {
	static std::map<std::string, Zone> z;
	return z;
}

struct Scope {
	Zone *z = nullptr;
	std::chrono::steady_clock::time_point t;
	explicit Scope(const char *name) {
		if (!enabled()) return;
		static thread_local std::map<const char *, Zone *> cache;
		auto &slot = cache[name];
		if (!slot) slot = &zones()[name];
		z = slot;
		t = std::chrono::steady_clock::now();
	}
	~Scope() {
		if (!z) return;
		z->ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
		++z->calls;
	}
};

struct Frames {
	double worst = 0;
	int over = 0;  // frames over 20 ms
};

inline Frames &frames() {
	static Frames f;
	return f;
}

// A frame over OPENBG_SPIKE_MS (default 50 ms) prints the zones that took more than a tenth of
// that (at least 1 ms) of it, and how many times each ran.
inline void frame_time(int frame, double ms) {
	Frames &f = frames();
	f.worst = std::max(f.worst, ms);
	f.over += ms > 20;
	static const double spike = getenv("OPENBG_SPIKE_MS") ? std::max(1.0, atof(getenv("OPENBG_SPIKE_MS"))) : 50.0;
	static const double zone_min = std::max(1.0, spike / 10);
	static std::map<std::string, double> before;
	static std::map<std::string, unsigned long long> calls_before;
	if (ms > spike) {
		fprintf(stderr, "[spike %d] %.1f ms:", frame, ms);
		for (auto &[name, z] : zones()) {
			if (z.ms - before[name] > zone_min) fprintf(stderr, " %s %.1f x%llu", name.c_str(), z.ms - before[name], z.calls - calls_before[name]);
		}
		fprintf(stderr, "\n");
	}
	for (auto &[name, z] : zones()) before[name] = z.ms, calls_before[name] = z.calls;
}

inline void report(int frame, int frames, double wall_ms) {
	if (!enabled() || frames <= 0) return;
	fprintf(stderr, "[perf %d] %.1f fps (%.2f ms a frame, worst %.1f, %d over 20)", frame, wall_ms > 0 ? frames * 1000.0 / wall_ms : 0.0, wall_ms / frames,
	        perf::frames().worst, perf::frames().over);
	perf::frames() = Frames{};
	for (auto &[name, z] : zones()) {
		fprintf(stderr, ", %s %.3f ms x%.0f", name.c_str(), z.ms / frames, double(z.calls) / frames);
	}
	for (auto &[name, z] : zones()) z = Zone{};
	frame_time(frame, 0);
	fprintf(stderr, "\n");
	fflush(stderr);
}

}  // namespace perf

#define PERF_CAT2(a, b) a##b
#define PERF_CAT(a, b) PERF_CAT2(a, b)
#define PERF_ZONE(name) ::perf::Scope PERF_CAT(perf_scope_, __LINE__)(name)
