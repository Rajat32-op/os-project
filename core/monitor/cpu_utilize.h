#ifndef CPU_MONITOR_HPP
#define CPU_MONITOR_HPP

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
    cpu_util_data prev;
    cpu_util_data cur;
    int initialised=0;
    int read_full_cpu_util(cpu_util_data *);
public:
    double monitor_full_cpu_util();
};

#endif