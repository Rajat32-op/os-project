#include "classifier.h"
#include "../calibrator/calibrator.h"
#include <cmath>
#include <algorithm>

// ── String helpers ────────────────────────────────────────────────────────
std::string modeToString(CpuMode m) {
    switch (m) {
        case CpuMode::IDLE:             return "IDLE";
        case CpuMode::INTERACTIVE:      return "INTERACTIVE";
        case CpuMode::COMPUTE:          return "COMPUTE";
        case CpuMode::LATENCY_CRITICAL: return "LATENCY_CRITICAL";
    }
    return "IDLE";
}

CpuMode modeFromString(const std::string& s) {
    if (s == "IDLE")             return CpuMode::IDLE;
    if (s == "INTERACTIVE")      return CpuMode::INTERACTIVE;
    if (s == "COMPUTE")          return CpuMode::COMPUTE;
    if (s == "LATENCY_CRITICAL") return CpuMode::LATENCY_CRITICAL;
    return CpuMode::IDLE;
}

// ── Constructor ───────────────────────────────────────────────────────────
Classifier::Classifier()
    : committed_mode(CpuMode::IDLE),
      candidate_mode(CpuMode::IDLE),
      streak(0) {}


void Classifier::loadCalibration(const std::string &path) {
    CalibrationData calib;
    if (!Calibrator::load(calib)) {
        printf("[classifier] No calibration found — using defaults\n");
        return;
    }

    IDLE_UTIL_MAX     = std::max(15.0,  calib.baseline_util  * 3.0);
    COMPUTE_CTXSW_MAX = std::max(8000.0, calib.baseline_ctxsw * 2.5);
    LC_CTXSW_MIN      = std::max(15000.0, calib.baseline_ctxsw * 4.0);
    LC_VARIANCE_MIN   = std::max(150.0,  calib.baseline_util  * 25.0);

    // COMPUTE_UTIL_MIN and LC_UTIL_MIN stay fixed — these are workload
    // thresholds, not machine-baseline-dependent

    printf("[classifier] Thresholds from calibration:\n");
    printf("  IDLE_UTIL_MAX     = %.1f%%\n", IDLE_UTIL_MAX);
    printf("  COMPUTE_CTXSW_MAX = %.0f/s\n",  COMPUTE_CTXSW_MAX);
    printf("  LC_CTXSW_MIN      = %.0f/s\n",  LC_CTXSW_MIN);
    printf("  LC_VARIANCE_MIN   = %.1f\n",    LC_VARIANCE_MIN);
}

// ── utilVariance ──────────────────────────────────────────────────────────
double Classifier::utilVariance() const {
    if (util_history.size() < 2) return 0.0;
    double sum = 0.0;
    for (double v : util_history) sum += v;
    double mean = sum / util_history.size();
    double var  = 0.0;
    for (double v : util_history) var += (v - mean) * (v - mean);
    return var / util_history.size();
}

// ── rawClassify ───────────────────────────────────────────────────────────
CpuMode Classifier::rawClassify(double util, double ipc,
                                 double ctxsw, double iowait) {
    (void)iowait;

    // 1. IDLE
    if (util < IDLE_UTIL_MAX)
        return CpuMode::IDLE;

    // 2. LATENCY_CRITICAL — checked before COMPUTE because gaming
    //    has high util AND high ctxsw (would fail COMPUTE's ctxsw check)
    double var = utilVariance();
    if (util  >= LC_UTIL_MIN   &&
        ctxsw >= LC_CTXSW_MIN  &&
        var   >= LC_VARIANCE_MIN)
        return CpuMode::LATENCY_CRITICAL;

    // 3. COMPUTE — sustained CPU, decent IPC, low thread churn
    if (util  >= COMPUTE_UTIL_MIN  &&
        ipc   >= COMPUTE_IPC_MIN   &&
        ctxsw <= COMPUTE_CTXSW_MAX)
        return CpuMode::COMPUTE;

    // 4. INTERACTIVE — catch-all
    return CpuMode::INTERACTIVE;
}

// ── update ────────────────────────────────────────────────────────────────
CpuMode Classifier::update(double util, double ipc,
                             double ctxsw, double iowait) {
    util_history.push_back(util);
    if ((int)util_history.size() > HISTORY_LEN)
        util_history.pop_front();

    CpuMode raw = rawClassify(util, ipc, ctxsw, iowait);

    if (raw == candidate_mode) {
        streak++;
    } else {
        candidate_mode = raw;
        streak = 1;
    }

    if (streak >= CONFIDENCE_N)
        committed_mode = candidate_mode;

    return committed_mode;
}