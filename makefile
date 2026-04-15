CC     = g++
CFLAGS = -Wall -Wextra -O2 -std=c++17
LFLAGS = -lm

SRC = main.cpp \
      core/monitor/monitor_manager.cpp \
      core/controllers/event_controller.cpp \
      core/classifier/classifier.cpp \
      core/calibrator/calibrator.cpp \
      core/policy/governor.cpp

OBJ = $(SRC:.cpp=.o)

TARGET       = os_manager
FIFO_MONITOR = config/monitor_pipe
FIFO_CONTROL = config/event_pipe

HEADERS = core/monitor/monitor_manager.h \
          core/controllers/event_controller.h \
          core/classifier/classifier.h \
          core/calibrator/calibrator.h \
          core/policy/governor.h

.PHONY: all clean run setup install

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ) $(LFLAGS)

%.o: %.cpp $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

setup:
	mkdir -p config results

	[ -p $(FIFO_MONITOR) ] || mkfifo $(FIFO_MONITOR)
	[ -p $(FIFO_CONTROL) ] || mkfifo $(FIFO_CONTROL)

install: all setup
	@echo "--- Setting up sudoers entry ---"
	echo "$$USER ALL=(ALL) NOPASSWD: $(CURDIR)/$(TARGET)" | \
	sudo tee /etc/sudoers.d/os_manager
	sudo chmod 440 /etc/sudoers.d/os_manager
	@echo "--- Running baseline calibration (keep system IDLE for 10s) ---"
	sudo ./$(TARGET) --calibrate

run: all setup
	@echo "Starting Python GUI..."
	@python3 interface/monitor.py & \
	PYTHON_PID=$$!; \
	sleep 2; \
	echo "Starting C++ monitor..."; \
	sudo ./$(TARGET); \
	echo "C++ exited, closing GUI..."; \
	kill $$PYTHON_PID 2>/dev/null || true

clean:
	rm -f $(OBJ) $(TARGET)