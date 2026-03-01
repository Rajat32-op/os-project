#include"monitor_manager.h"
#include<unistd.h>
#include<sys/wait.h>
#include<fcntl.h>
#include<signal.h>
#include<vector>
#include<sstream>

MonitorManager::MonitorManager(){
    turbostat_pid=-1;
    running=false;
    fifo_fd=-1;
    pipefd[0]=-1;
    pipefd[1]=-1;
}

MonitorManager::~MonitorManager(){
    stop();
}

void MonitorManager::readerLoop(){
    FILE* fd = fdopen(pipefd[0], "r");
    if (!fd) {
        printf("fdopen failed\n");
        return;
    }

    char buffer[4096];
    bool skipped_header=false;
    while (running && fgets(buffer, sizeof(buffer), fd)) {

        std::string line(buffer);
        if(!skipped_header){
            skipped_header=true;
            continue;
        }

        std::stringstream ss(line);
        std::vector<std::string> columns;
        std::string token;

        while (ss >> token) {
            columns.push_back(token);
        }

        if (columns.size() < 7)continue;

        if (columns[0] != "-")continue;

        try{

            double avg_mhz  = std::stod(columns[2]);
            double busy_pct = std::stod(columns[3]);
            double ipc      = std::stod(columns[6]);
            
            std::cout << "Util: " << busy_pct<< "% | Avg MHz: " << avg_mhz<< " | IPC: " << ipc << std::endl;
            std::string json = "{";
            json += "\"util\":" + std::to_string(busy_pct) + ",";
            json += "\"mhz\":" + std::to_string(avg_mhz) + ",";
            json += "\"ipc\":" + std::to_string(ipc);
            json += "}\n";

            if (fifo_fd >= 0) {
                ssize_t bytes_written = write(fifo_fd, json.c_str(), json.size());
                if (bytes_written < 0) {
                    printf("Error writing to FIFO\n");
                    running = false;
                    break;
                }
            }
        }
        catch(const std::exception& e){
            continue;
        }
    }

    fclose(fd); 
}

void MonitorManager::start(){
    if(running)return;

    if(pipe(pipefd)==-1){
        printf("Couldn't create pipe\n");
        return;
    }
    
    fifo_fd = open("config/monitor_pipe", O_WRONLY);
    if (fifo_fd < 0) {
        perror("FIFO open failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    turbostat_pid=fork();

    if(turbostat_pid==0){
        dup2(pipefd[1],STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("turbostat","turbostat","--interval", "1","--quiet",NULL);
        printf("Exec failed\n");
        exit(1);
    }
    else{
        close(pipefd[1]); // Parent doesn't write to pipe
        running=true;
        reader_thread=std::thread(&MonitorManager::readerLoop,this);
    }
}

void MonitorManager::stop(){
    if(!running)return;
    running=false;

    if(turbostat_pid>0){
        kill(turbostat_pid,SIGTERM);
        waitpid(turbostat_pid,NULL,0);
        turbostat_pid=-1;
    }
    if(reader_thread.joinable())reader_thread.join();
    
    if(pipefd[0] >= 0) {
        close(pipefd[0]);
        pipefd[0] = -1;
    }
    if(pipefd[1] >= 0) {
        close(pipefd[1]);
        pipefd[1] = -1;
    }
    if (fifo_fd >= 0) {
        close(fifo_fd);
        fifo_fd = -1;
    }
}