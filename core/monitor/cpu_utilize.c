#include<stdlib.h>
#include<stdio.h>
#include<unistd.h>
#include"cpu_utilize.h"

static cpu_util_data prev;
static cpu_util_data cur;
static int initialised=0;

int read_cpu_util(cpu_util_data *data){
    FILE *fd=fopen("/proc/stat","r");
    if(!fd){
        printf("Could not open /proc/stat file\n");
        return -1;
    }
    int returnfscanf=fscanf(fd,"cpu %llu %llu %llu %llu %llu %llu %llu %llu",
    &data->user,&data->nice,&data->system,&data->idle,&data->iowait,&data->irq,&data->softirq,&data->steal);
    if(returnfscanf!=8){
        printf("Unexpected behaviour\n");
        return -1.0;
    }
    fclose(fd);
    return 0;
}

int monitor_cpu_util_init(){
    int res=read_cpu_util(&prev);
    if(res!=-1)initialised=1;
    return res;
}

double monitor_cpu_util(){
    if(initialised==0){
        printf("Call init function first\n");
        return -1.0;
    }
    int res=read_cpu_util(&cur);
    if(res==-1){
        printf("Check if init\n");
        return -1.0;
    }

    unsigned long long cur_total=cur.user+cur.system+cur.nice+cur.idle+
                                 cur.iowait+cur.irq+cur.softirq+cur.steal;
    unsigned long long prev_total=prev.user+prev.system+prev.nice+prev.idle+
                                 prev.iowait+prev.irq+prev.softirq+prev.steal;
    unsigned long long cur_active=cur.user+cur.nice+cur.system+cur.irq+cur.softirq+cur.steal;
    unsigned long long prev_active=prev.user+prev.nice+prev.system+prev.irq+prev.softirq+prev.steal;

    unsigned long long total_delta=cur_total-prev_total;
    unsigned long long active_delta=cur_active-prev_active;

    if(total_delta==0)return 0.0;

    double usage=(double)active_delta/total_delta*100;

    return usage;
}
