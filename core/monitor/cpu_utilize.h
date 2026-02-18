#ifndef CPU_MONITOR_HPP
#define CPU_MONITOR_HPP
#include <vector>

struct cpu_util_data{
    unsigned long long int user; //time spent running normal user space processes
    unsigned long long int nice; //time spent running user processes with adjusted nice valu
    unsigned long long int system; //time spent running kernel code
    unsigned long long int idle; //idle time
    unsigned long long int iowait; //time spent waiting for I/O
    unsigned long long int irq;//time spent servicing hardware interrupt
    unsigned long long int softirq;//time spent servicing software interrupt;
    unsigned long long int steal;//time stolen by hyperviser (relevent only when running vm. hyperviser is basically host machine)
};
class CpuUtilizationMonitor{
private:
cpu_util_data full;
std::vector<cpu_util_data>core;
int readFullCpuUtil(cpu_util_data *);
int readCoreCpuUtil(std::vector<cpu_util_data>&data);
public:
    unsigned long long full_total;
    unsigned long long full_active;
    std::vector<unsigned long long>core_total;
    std::vector<unsigned long long>core_active;
    void monitorFullCpuUtil();
    void monitorCoreCpuUtil();
};

#endif