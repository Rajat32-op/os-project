#include<thread>
#include<atomic>

class EventController{
private:
    int pipe_fd;
    std::thread listen_thread;
    std::atomic<bool>running;
    void listenLoop();
public:
    EventController();
    ~EventController();
    void listen();
};