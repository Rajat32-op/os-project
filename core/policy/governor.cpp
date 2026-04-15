#include "governor.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sstream>
#include <vector>

Governor::Governor() : num_cpus(1) {}

// ── init ──────────────────────────────────────────────────────────────────
void Governor::init() {
    num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cpus < 1) num_cpus = 1;

    available.clear();

    // Read from cpu0 — available governors are the same for all CPUs
    FILE *f = fopen(
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors", "r");
    if (!f) {
        printf("[governor] Cannot read available governors — will try performance/powersave\n");
        available.insert("performance");
        available.insert("powersave");
        return;
    }

    char buf[256] = {};
    if (fgets(buf, sizeof(buf), f)) {
        std::istringstream ss(buf);
        std::string token;
        while (ss >> token) available.insert(token);
    }
    fclose(f);

    printf("[governor] Available:");
    for (const auto &g : available) printf(" %s", g.c_str());
    printf("\n");
}

// ── pickGov ───────────────────────────────────────────────────────────────
// Priority lists per mode — first entry that exists in `available` wins.
std::string Governor::pickGov(CpuMode mode) const {
    std::vector<std::string> priority;

    switch (mode) {
        case CpuMode::IDLE:
            priority = {"powersave", "conservative", "schedutil", "performance"};
            break;
        case CpuMode::INTERACTIVE:
            priority = {"schedutil", "ondemand", "conservative", "powersave", "performance"};
            break;
        case CpuMode::COMPUTE:
        case CpuMode::LATENCY_CRITICAL:
            priority = {"performance", "schedutil", "ondemand", "powersave"};
            break;
    }

    for (const auto &g : priority) {
        if (available.count(g)) return g;
    }

    // Absolute fallback — return whatever is first in the available set
    if (!available.empty()) return *available.begin();
    return "performance";
}

// ── writeGov ──────────────────────────────────────────────────────────────
bool Governor::writeGov(const std::string &gov) {
    bool all_ok = true;
    for (int cpu = 0; cpu < num_cpus; ++cpu) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
        FILE *f = fopen(path, "w");
        if (!f) {
            // CPU may be offline — skip silently
            continue;
        }
        if (fprintf(f, "%s\n", gov.c_str()) < 0) {
            printf("[governor] Write failed for cpu%d\n", cpu);
            all_ok = false;
        }
        fclose(f);
    }
    return all_ok;
}

// ── apply ─────────────────────────────────────────────────────────────────
void Governor::apply(CpuMode mode) {
    std::string gov = pickGov(mode);

    if (gov == current_gov) return;   // already set — no write needed

    printf("[governor] %s → %s\n", current_gov.empty() ? "(none)" : current_gov.c_str(),
           gov.c_str());

    if (writeGov(gov)) {
        current_gov = gov;
    } else {
        printf("[governor] Failed to set %s\n", gov.c_str());
    }
}