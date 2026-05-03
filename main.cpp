#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string>
#include "core/controllers/event_controller.h"
#include "core/calibrator/calibrator.h"

static volatile bool g_running = true;

void handle_signal(int) { g_running = false; }

int main(int argc, char *argv[]) {

    // ── Calibration mode ──────────────────────────────────────────────────
    if (argc > 1 && std::string(argv[1]) == "--calibrate") {
        return Calibrator::runAndSave() ? 0 : 1;
    }

    // ── Normal operation ──────────────────────────────────────────────────
    std::cout << "Main started\n";
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN); // Prevent crash if writing to pipe with no reader

    EventController event_controller;
    std::cout << "Starting monitor...\n";
    event_controller.listen();

    while (g_running) pause();

    std::cout << "Monitoring stopped.\n";
    return 0;
}