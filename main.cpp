#include<iostream>
#include"core/monitor/monitor_manager.h"
#include<stdlib.h>
#include<unistd.h>

int main(){

    std::cout<<"Main started\n";
    MonitorManager manager;
    std::cout << "Starting monitor...\n";

    manager.start();

    // For now simulate workload duration
    std::this_thread::sleep_for(std::chrono::seconds(10));

    manager.stop();

    std::cout << "Monitoring stopped.\n";
    return 0;
}