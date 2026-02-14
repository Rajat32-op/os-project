#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include<string.h>
#include "cpu_utilize.h"

int CpuUtilizationMonitor::read_full_cpu_util(cpu_util_data *data)
{
    FILE *fd = fopen("/proc/stat", "r");
    if (!fd)
    {
        printf("Could not open /proc/stat file\n");
        return -1;
    }
    int returnfscanf = fscanf(fd, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                              &data->user, &data->nice, &data->system, &data->idle, &data->iowait, &data->irq, &data->softirq, &data->steal);
    if (returnfscanf != 8)
    {
        printf("Unexpected behaviour\n");
        return -1.0;
    }
    fclose(fd);
    return 0;
}

int CpuUtilizationMonitor::read_core_cpu_util(std::vector<cpu_util_data>&data){
    FILE *fd=fopen("/proc/stat","r");
    if(!fd){
        printf("Could not open /proc/stat file\n");
        return -1;
    }

    char buffer[512];
    int i=0;

    while(fgets(buffer,sizeof(buffer),fd)){
        if(strncmp(buffer,"cpu",3)!=0)break;
        if(buffer[3]==' ')continue;

        cpu_util_data core;
        sscanf(buffer,"cpu%*d %llu %llu %llu %llu %llu %llu %llu %llu",
        &data[i].user, &data[i].nice, &data[i].system, &data[i].idle, &data[i].iowait, &data[i].irq, &data[i].softirq, &data[i].steal);
        i++;
    }
    fclose(fd);
    return 0;
}

double CpuUtilizationMonitor::monitor_full_cpu_util()
{
    if (initialised == 0)
    {
        int res=read_full_cpu_util(&full_prev);
        if(res!=-1){
            initialised=1;
            sleep(1);
        }
        else{
            return -1;
        }
    }
    int res = read_full_cpu_util(&full_cur);
    if (res == -1)
    {
        printf("Check if init\n");
        return -1.0;
    }

    unsigned long long full_cur_total = full_cur.user + full_cur.system + full_cur.nice + full_cur.idle +
                                   full_cur.iowait + full_cur.irq + full_cur.softirq + full_cur.steal;
    unsigned long long full_prev_total = full_prev.user + full_prev.system + full_prev.nice + full_prev.idle +
                                    full_prev.iowait + full_prev.irq + full_prev.softirq + full_prev.steal;
    unsigned long long full_cur_active = full_cur.user + full_cur.nice + full_cur.system + full_cur.irq + full_cur.softirq + full_cur.steal;
    unsigned long long full_prev_active = full_prev.user + full_prev.nice + full_prev.system + full_prev.irq + full_prev.softirq + full_prev.steal;

    unsigned long long total_delta = full_cur_total - full_prev_total;
    unsigned long long active_delta = full_cur_active - full_prev_active;

    if (total_delta == 0)
        return 0.0;

    double usage = (double)active_delta / total_delta * 100;
    full_prev=full_cur;

    return usage;
}

std::vector<double> CpuUtilizationMonitor::monitor_core_cpu_util(){
    if(core_cur.size()==0){
        int cores = sysconf(_SC_NPROCESSORS_ONLN);   //only cores which aren't disabled
        core_cur.resize(cores);
        core_prev.resize(cores);
        int res=read_core_cpu_util(core_prev);
        if(res!=-1){
            sleep(1);
        }
        else{
            return {-1.0};
        }

    }

    int res=read_core_cpu_util(core_cur);
    int n=core_cur.size();
    std::vector<double>usage(n);
    for(int i=0;i<n;i++){
        unsigned long long cur_total = core_cur[i].user + core_cur[i].system + core_cur[i].nice + core_cur[i].idle +
                                   core_cur[i].iowait + core_cur[i].irq + core_cur[i].softirq + core_cur[i].steal;
        unsigned long long prev_total = core_prev[i].user + core_prev[i].system + core_prev[i].nice + core_prev[i].idle +
                                        core_prev[i].iowait + core_prev[i].irq + core_prev[i].softirq + core_prev[i].steal;
        unsigned long long cur_active = core_cur[i].user + core_cur[i].nice + core_cur[i].system + core_cur[i].irq + core_cur[i].softirq + core_cur[i].steal;
        unsigned long long prev_active = core_prev[i].user + core_prev[i].nice + core_prev[i].system + core_prev[i].irq + core_prev[i].softirq + core_prev[i].steal;
        unsigned long long total_delta = cur_total - prev_total;
        unsigned long long active_delta = cur_active - prev_active;

        if(total_delta==0){
            usage[i]=0.0;
            continue;
        }
        usage[i]=(double)active_delta/total_delta*100;
    }
    core_prev=core_cur;
    return usage;
}