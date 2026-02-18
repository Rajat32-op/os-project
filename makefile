CC = g++
CFLAGS = -Wall -Wextra -O2 -std=c++17

SRC = main.cpp \
      core/monitor/cpu_utilize.cpp \
      core/monitor/energy.cpp \
      core/monitor/monitor_manager.cpp

OBJ = $(SRC:.cpp=.o)

TARGET = os_manager
PIPE = config/monitor_pipe

.PHONY: all clean run setup python

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

%.o: %.cpp
	$(CC) $(CFLAGS) -c $< -o $@

setup:
	@if [ ! -p $(PIPE) ]; then \
		mkfifo $(PIPE); \
		echo "Created FIFO at $(PIPE)"; \
	else \
		echo "FIFO already exists"; \
	fi

run: all setup
	@echo "Starting Python GUI..."
	@python3 interface/monitor.py & \
	sleep 1; \
	echo "Starting C++ monitor..."; \
	sudo -E ./$(TARGET)


clean:
	rm -f $(OBJ) $(TARGET)
	rm -f $(PIPE)
