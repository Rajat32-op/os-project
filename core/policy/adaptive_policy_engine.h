#pragma once
#include <map>
#include <string>
#include <vector>

// Continuous system state features
struct SystemState {
    double util;
    double ipc;
    double iowait;
    double disk_io;
    double mem_bw; // MPKI
    double mhz;
    double temp;
    double power;
    double ctxsw;
};

// System action state
struct SysAction {
    std::string name;
    int freq_cap_pct;
    std::string governor;
    int reserved_p_cores; // how many P cores to use
    bool migrate_to_e;
    int swappiness;
    std::string io_sched;
    std::string source = "BASELINE"; // identifies if action came from BASELINE or RL
};

class AdaptivePolicyEngine {
private:
    double ema_util;
    double ema_ipc;
    double ema_io;
    double ema_disk_io;
    double ema_mem;
    
    SysAction current_action;
    int cooldown_timer;
    
    int state_fifo_fd;
    int action_fifo_fd;
    double last_reward;
    
    // Smooth input metrics to EMA
    void updateEMA(double util, double ipc, double iowait, double disk_io, double mem_bw);
    
public:
    AdaptivePolicyEngine();
    
    // 1. Process continuous state variables
    SystemState processState(double util, double ipc, double iowait, double disk_io, double mem_bw, double mhz, double temp, double power, double ctxsw);
    
    // 2. Decide Policy -> Feedback Action
    SysAction decidePolicy(const SystemState& state);
    
    // 3. Apply Action (simulated atomic apply log)
    void applyAction(const SysAction& action);
    
    // 4. Compute reward
    double computeReward(double latency, double power_proxy);
    
    // 5. RL Interface
    void initFIFOs();
    void sendStateToRL(const SystemState& state, double reward);
    bool readActionFromRL(SysAction& out_action);
    bool isActionSafe(const SysAction& action, double util);
    
    ~AdaptivePolicyEngine();
};
