#include<stdio.h>
#include"core/monitor/cpu_utilize.h"
#include<stdlib.h>
#include<unistd.h>

int main(){
    printf("hello from main\n");
    CpuUtilizationMonitor cpu_util_monitor ;
    // while(1){
    //     double res=cpu_util_monitor.monitor_full_cpu_util();
    //     if(res==-1.0){
    //         return 1;
    //     }
    //     printf("%lf percentage of cpu is being utilised\n",res);
    //     sleep(1);
    // }
    while (1) {
    std::vector<double> res = cpu_util_monitor.monitor_core_cpu_util();
    if (res.empty() || res[0] == -1.0) {
        return 1;
    }
    printf("--------------------------------------------------\n");
    printf("Per-Core CPU Utilization:\n");
    for (size_t i = 0; i < res.size(); ++i) {
        printf("Core %-2zu : %6.2f %%\n", i, res[i]);
    }
    printf("--------------------------------------------------\n\n");
    sleep(1); 
    }
    return 0;
}