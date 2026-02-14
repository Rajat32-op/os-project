#include"energy.h"
#include<iostream>
#include<unistd.h>

int EnergyMonitor::readEnergy(unsigned long long *data){
    FILE *fd=fopen("/sys/class/powercap/intel-rapl:0/energy_uj","r");
    if(!fd){
        printf("Could not monitor energy. Check if you have Intel chip or not\n");
        return -1;
    }
    
    if(fscanf(fd,"%llu",data)!=1){
        printf("Some error occured\n");
        return -1;
    }
    fclose(fd);
    return 0;
}

int EnergyMonitor::init(){
    FILE *fd=fopen("/sys/class/powercap/intel-rapl:0/max_energy_range_uj","r");
    if(!fd){
        printf("Could not monitor energy. Check if you have Intel chip or not\n");
        return -1;
    }
    if(fscanf(fd,"%llu",&max_energy)!=1){
        printf("Some error occured\n");
        return -1;
    }    fclose(fd);
    int res=readEnergy(&prev_energy);
    if(res==-1)return -1;
    initialised=1;
    return 0;
}

double EnergyMonitor::monitorEnergy(){
    if(initialised==0){
        int res=init();
        if(res==-1)return -1.0;
        initialised=1;
        sleep(1);
    }
    
    int res=readEnergy(&cur_energy);
    if(res==-1){
        return -1.0;
    }
    unsigned long long delta;

    if (cur_energy >= prev_energy) {
        delta = cur_energy - prev_energy;
    } else {
        // wraparound happened
        delta = (max_energy - prev_energy) + cur_energy;
    }
    double energy=delta/1e6;

    return energy;
}