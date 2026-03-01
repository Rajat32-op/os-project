#include<pthread.h>
#include<thread>
#include<atomic>
#include<string>
#include<iostream>

class MonitorManager{
private:
    pid_t turbostat_pid;
    std::atomic<bool> running;   
    std::thread reader_thread;
    int fifo_fd;
    int pipefd[2];
    
    void readerLoop();
public:
    MonitorManager();
    ~MonitorManager();
    void start();
    void stop();
};