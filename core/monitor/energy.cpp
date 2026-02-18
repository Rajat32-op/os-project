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

void EnergyMonitor::monitorEnergy(){
    
    int res=readEnergy(&energy);
    if(res==-1){
        exit(0);
    }
    energy/=1e6; //to convert from micro J to J
}