#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include<string.h>
#include "cpu_utilize.h"

int CpuUtilizationMonitor::readFullCpuUtil(cpu_util_data *data)
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

int CpuUtilizationMonitor::readCoreCpuUtil(std::vector<cpu_util_data>&data){
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

        sscanf(buffer,"cpu%*d %llu %llu %llu %llu %llu %llu %llu %llu",
        &data[i].user, &data[i].nice, &data[i].system, &data[i].idle, &data[i].iowait, &data[i].irq, &data[i].softirq, &data[i].steal);
        i++;
    }
    fclose(fd);
    return 0;
}

void CpuUtilizationMonitor::monitorFullCpuUtil()
{
    int res = readFullCpuUtil(&full);
    if (res == -1)
    {
        printf("Check if init\n");
        exit(0);
    }
    full_active=full.nice+full.irq+full.softirq+full.user+full.system+full.steal;
    full_total=full.nice+full.irq+full.softirq+full.user+full.system+full.steal+full.idle+full.iowait;
}

void CpuUtilizationMonitor::monitorCoreCpuUtil(){
    if(core.size()==0){
        int cores = sysconf(_SC_NPROCESSORS_ONLN);   //only cores which aren't disabled
        core.resize(cores);
        core_active.resize(cores);
        core_total.resize(cores);
    }

    int res=readCoreCpuUtil(core);
    if(res==-1){
        exit(1);
    }
    for(int i=0;i<(int)core.size();i++){
        core_active[i]=core[i].nice+core[i].user+core[i].system+core[i].irq+core[i].softirq+core[i].steal;
        core_total[i]=core_active[i]+core[i].idle+core[i].iowait;
    }
}