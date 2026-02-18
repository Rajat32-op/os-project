#include"energy.h"
#include"cpu_utilize.h"
#include<thread>
#include<atomic>
#include<vector>
#include<fstream>
#include<chrono>

struct snapShot{
    CpuUtilizationMonitor cpu_util_monitor;
    EnergyMonitor energy_monitor;
    int timestamp; //in ms
};

struct systemMetric{
    double cpu_util;
    std::vector<double>per_core_util;
    double energy;
    // add frequency also
};

class MonitorManager{
private:
    snapShot prev;
    snapShot cur;
    std::thread monitor_thread;
    std::atomic<bool>running;
    int interval=1000;  //in mili seconds
    std::chrono::steady_clock::time_point startTime;

    std::ofstream livestream;
    //need to declare results array and store whatever is needed for report

    void monitorLoop();
    void takeSnapShot(snapShot &snap);
    systemMetric getMetric();
public:
    void start();
    void stop();
};