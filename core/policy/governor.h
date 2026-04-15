#pragma once
#include <string>
#include <set>
#include "../classifier/classifier.h"

class Governor {
public:
    Governor();

    // Read available governors from /sys, build internal fallback map.
    // Call once at startup.
    void init();

    // Apply the best available governor for the given mode.
    // No-op if init() was not called or write fails.
    void apply(CpuMode mode);

    // Returns currently active governor string (empty if unknown)
    std::string current() const { return current_gov; }

private:
    int         num_cpus;
    std::set<std::string> available;
    std::string current_gov;

    // Pick the best governor for a mode from the available set
    std::string pickGov(CpuMode mode) const;

    // Write governor string to all online CPUs
    bool writeGov(const std::string &gov);
};