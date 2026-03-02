#include<iostream>
#include"core/controllers/event_controller.h"
#include<stdlib.h>
#include<unistd.h>
#include<signal.h>

static volatile bool g_running = true;

void handle_signal(int) {
    g_running = false;
}

int main(){

    std::cout<<"Main started\n";
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    EventController event_controller;
    std::cout << "Starting monitor...\n";

    event_controller.listen();

    // Block until a shutdown signal is received
    while (g_running) {
        pause();
    }

    std::cout << "Monitoring stopped.\n";
    return 0;
}