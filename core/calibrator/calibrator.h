#pragma once
#include <string>

struct CalibrationData {
    double baseline_util   = 5.0;
    double baseline_ctxsw  = 3000.0;
    double baseline_ipc    = 1.0;
};

class Calibrator {
public:
    static constexpr const char* CALIB_PATH = "config/calibration.json";
    static constexpr int         SAMPLE_S   = 10;

    // Run 10-second idle measurement, save to CALIB_PATH.
    // Returns false if something went wrong.
    static bool runAndSave();

    // Load calibration from CALIB_PATH into out.
    // Returns false if file missing — caller should use defaults.
    static bool load(CalibrationData &out);
};