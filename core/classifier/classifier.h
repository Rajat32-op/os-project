#pragma once
#include <string>
#include <deque>

// ── Mode enum ─────────────────────────────────────────────────────────────
enum class CpuMode {
    IDLE,
    INTERACTIVE,
    COMPUTE,
    LATENCY_CRITICAL
};

std::string modeToString(CpuMode m);
CpuMode     modeFromString(const std::string& s);

// ── Classifier ────────────────────────────────────────────────────────────
class Classifier {
public:
    // Thresholds — set directly or via loadCalibration()
    double IDLE_UTIL_MAX      =  15.0;
    double COMPUTE_UTIL_MIN   =  55.0;
    double COMPUTE_IPC_MIN    =   0.5;
    double COMPUTE_CTXSW_MAX  = 20000.0;
    double LC_UTIL_MIN        =  35.0;
    double LC_CTXSW_MIN       = 30000.0;
    double LC_VARIANCE_MIN    =  200.0;
    int    CONFIDENCE_N       =   5;

    Classifier();

    // Load calibration.json and derive thresholds from baseline multiples.
    // Safe to call even if file doesn't exist — keeps defaults.
    void loadCalibration(const std::string &path);

    // Feed one sample → returns committed mode
    CpuMode update(double util, double ipc, double ctxsw, double iowait);

    CpuMode getCommitted() const { return committed_mode; }
    CpuMode getCandidate() const { return candidate_mode; }
    int     getStreak()    const { return streak; }

private:
    CpuMode committed_mode;
    CpuMode candidate_mode;
    int     streak;

    std::deque<double> util_history;
    static constexpr int HISTORY_LEN = 20;

    CpuMode rawClassify(double util, double ipc, double ctxsw, double iowait);
    double  utilVariance() const;
};