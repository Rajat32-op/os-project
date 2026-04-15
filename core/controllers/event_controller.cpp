#include "event_controller.h"
#include "../monitor/monitor_manager.h"
#include "../classifier/classifier.h"
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <string>

EventController::EventController() : pipe_fd(-1) {}

EventController::~EventController() {
    running = false;
    if (listen_thread.joinable()) listen_thread.join();
}

void EventController::listenLoop() {
    int fd = open("config/event_pipe", O_RDONLY);
    if (fd < 0) {
        printf("[event] Couldn't open event_pipe\n");
        return;
    }

    MonitorManager monitor_manager;
    char buffer[512];

    while (running) {
        int n = read(fd, buffer, sizeof(buffer) - 1);
        if (n <= 0) continue;

        buffer[n] = '\0';
        std::string msg(buffer);

        // ── START / STOP / QUIT ──────────────────────────────────────────────
        if (msg.find("\"START\"") != std::string::npos) {
            monitor_manager.start();
        }
        else if (msg.find("\"STOP\"") != std::string::npos) {
            monitor_manager.stop();
        }
        else if (msg.find("\"QUIT\"") != std::string::npos) {
            printf("[event] QUIT received — shutting down\n");
            monitor_manager.stop();
            exit(0);
        }

        // ── SET_MODE ─────────────────────────────────────────────────────────
        // Expected JSON: {"cmd":"SET_MODE","mode":"COMPUTE"}
        //                {"cmd":"SET_MODE","mode":"AUTO"}
        else if (msg.find("\"SET_MODE\"") != std::string::npos) {
            // Extract "mode":"<value>" using simple string search
            // No external JSON lib needed — format is fixed and small
            auto extract_mode = [&]() -> std::string {
                const std::string key = "\"mode\":\"";
                size_t pos = msg.find(key);
                if (pos == std::string::npos) return "";
                pos += key.size();
                size_t end = msg.find('"', pos);
                if (end == std::string::npos) return "";
                return msg.substr(pos, end - pos);
            };

            std::string mode_str = extract_mode();
            printf("[event] SET_MODE received: %s\n", mode_str.c_str());

            if (mode_str == "AUTO") {
                monitor_manager.setAuto();
            } else {
                // Try to parse as a known mode name
                CpuMode m = modeFromString(mode_str);
                // modeFromString returns IDLE as fallback — only pin if
                // the string was actually a valid mode name
                if (mode_str == "IDLE"             ||
                    mode_str == "INTERACTIVE"       ||
                    mode_str == "COMPUTE"           ||
                    mode_str == "LATENCY_CRITICAL") {
                    monitor_manager.setPinnedMode(m);
                } else {
                    printf("[event] Unknown mode: %s\n", mode_str.c_str());
                }
            }
        }

        // ── SET_GOV (placeholder — will be used in Step 3) ──────────────────
        else if (msg.find("\"SET_GOV\"") != std::string::npos) {
            printf("[event] SET_GOV received (not yet implemented)\n");
        }
    }

    close(fd);
}

void EventController::listen() {
    running      = true;
    listen_thread = std::thread(&EventController::listenLoop, this);
}