#include "monitor_manager.h"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <fcntl.h>

void MonitorManager::takeSnapShot(snapShot &snap){
    snap.cpu_util_monitor.monitorCoreCpuUtil();
    snap.cpu_util_monitor.monitorFullCpuUtil();
    snap.energy_monitor.monitorEnergy();
    auto now = std::chrono::steady_clock::now();

    int ms =std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
    snap.timestamp = ms;
}

systemMetric MonitorManager::getMetric(){
    systemMetric s;

    if(cur.energy_monitor.energy<prev.energy_monitor.energy){
        s.energy=(cur.energy_monitor.max_energy-prev.energy_monitor.energy)+cur.energy_monitor.energy;
    }else{
        s.energy=cur.energy_monitor.energy-prev.energy_monitor.energy;
    }

    unsigned long long delta_active=cur.cpu_util_monitor.full_active-prev.cpu_util_monitor.full_active;
    unsigned long long delta_total=cur.cpu_util_monitor.full_total-prev.cpu_util_monitor.full_total;

    if(delta_total==0.0){
        s.cpu_util=0.0;
    }
    else{
        s.cpu_util=(double)delta_active/delta_total*100;
    }
    s.per_core_util.resize(cur.cpu_util_monitor.core_active.size());
    for(int i=0;i<(int)cur.cpu_util_monitor.core_active.size();i++){
        delta_active=cur.cpu_util_monitor.core_active[i]-prev.cpu_util_monitor.core_active[i];
        delta_total=cur.cpu_util_monitor.core_total[i]-prev.cpu_util_monitor.core_total[i];
        if(delta_total==0.0){
            s.per_core_util[i]=0.0;
        }
        else{
            s.per_core_util[i]=(double)delta_active/delta_total*100;
        }
    }
    return s;
}

void MonitorManager::monitorLoop(){
    takeSnapShot(prev);
    auto next = std::chrono::steady_clock::now();
    while(running){
        next += std::chrono::milliseconds(interval);
        takeSnapShot(cur);
        systemMetric s=getMetric();
        
        if(livestream.is_open()){
            livestream<<"{";
            livestream<<"\"timestamp\":"<<cur.timestamp<<",";
            livestream<<"\"total_cpu\":"<<s.cpu_util<<",";
            // for(int i=0;i<s.per_core_util.size();i++){
            //     livestream<<"\"core"<<i+1<<"\""<<s.per_core_util[i]<<",";
            // }
            livestream<<"\"energy\":"<<s.energy;
            livestream<<"}";
            livestream<<std::endl;
        }
        prev=cur;
        std::this_thread::sleep_until(next);
    }
}

void MonitorManager::start() {
    startTime = std::chrono::steady_clock::now();
    running = true;
    livestream.open("config/monitor_pipe");
    if (!livestream.is_open()) {
        std::cerr << "Failed to open FIFO\n";
    }
    else{
        std::cout<<"Fifo opened\n";
    }

    monitor_thread = std::thread(&MonitorManager::monitorLoop, this);
}

void MonitorManager::stop() {

    running = false;

    if (monitor_thread.joinable())
        monitor_thread.join();

    if (livestream.is_open())
        livestream.close();
}

