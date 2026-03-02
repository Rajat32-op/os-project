CC = g++
CFLAGS = -Wall -Wextra -O2 -std=c++17

SRC = main.cpp \
      core/monitor/monitor_manager.cpp \
	  core/controllers/event_controller.cpp

OBJ = $(SRC:.cpp=.o)

TARGET = os_manager
FIFO_MONITOR = config/monitor_pipe
FIFO_CONTROL=config/event_pipe

.PHONY: all clean run setup python

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

%.o: %.cpp
	$(CC) $(CFLAGS) -c $< -o $@

setup:
	mkdir -p config
	[ -p $(FIFO_MONITOR) ] || mkfifo $(FIFO_MONITOR)
	[ -p $(FIFO_CONTROL) ] || mkfifo $(FIFO_CONTROL)

run: all setup
	@echo "Starting Python GUI..."
	@python3 interface/monitor.py & \
	PYTHON_PID=$$!; \
	sleep 2; \
	echo "Starting C++ monitor..."; \
	sudo -E ./$(TARGET); \
	echo "C++ exited, closing GUI..."; \
	kill $$PYTHON_PID 2>/dev/null || true


clean:
	rm -f $(OBJ) $(TARGET)
	rm -f $(PIPE)
