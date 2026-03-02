#include"event_controller.h"
#include"../monitor/monitor_manager.h"
#include<fcntl.h>
#include<iostream>
#include<unistd.h>

EventController::EventController(){
    pipe_fd=-1;
    
}
EventController::~EventController(){
    running=false;
    if(listen_thread.joinable())listen_thread.join();
    
}

void EventController::listenLoop(){
    int fd=open("config/event_pipe",O_RDONLY);
    char buffer[512];
    MonitorManager monitor_manager;
    if(fd<0){
        printf("Couldn't open pipe\n");
        return;
    }
    while(running){
        int n=read(fd,buffer,sizeof(buffer)-1);
        if(n>0){
            buffer[n]='\0';
            std::string msg(buffer);
            if(msg.find("\"START\"")!=std::string::npos){
                monitor_manager.start();
            }
            else if(msg.find("\"STOP\"")!=std::string::npos){
                monitor_manager.stop();
            }
            else if(msg.find("\"QUIT\"")!=std::string::npos){
                printf("Received QUIT from GUI, shutting down...\n");
                exit(0);
            }
        }
    }
}

void EventController::listen(){
    running=true;
    listen_thread=std::thread(&EventController::listenLoop,this);
}