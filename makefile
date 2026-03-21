CC      = g++
CFLAGS  = -Wall -Wextra -O2 -std=c++17

SRC = main.cpp \
      core/monitor/monitor_manager.cpp \
      core/controllers/event_controller.cpp

OBJ    = $(SRC:.cpp=.o)
TARGET = os_manager
FIFO_MONITOR = config/monitor_pipe
FIFO_CONTROL = config/event_pipe
SUDOERS_FILE = /etc/sudoers.d/os_manager

.PHONY: all clean run setup install uninstall

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

%.o: %.cpp
	$(CC) $(CFLAGS) -c $< -o $@

setup:
	mkdir -p config
	[ -p $(FIFO_MONITOR) ] || mkfifo $(FIFO_MONITOR)
	[ -p $(FIFO_CONTROL) ] || mkfifo $(FIFO_CONTROL)

install:
	@echo "Installing sudoers entry for $(TARGET)..."
	@echo "$$USER ALL=(ALL) NOPASSWD: $(CURDIR)/$(TARGET)" | \
		sudo tee $(SUDOERS_FILE) > /dev/null
	@sudo chmod 440 $(SUDOERS_FILE)
	@echo "Done. sudo will no longer prompt for $(TARGET)."

uninstall:
	@echo "Removing sudoers entry..."
	@sudo rm -f $(SUDOERS_FILE)
	@echo "Done."

run: all setup install
	@echo "Starting Python GUI in background..."
	@python3 interface/monitor.py &\
	PYTHON_PID=$$!; \
	echo "Starting C++ backend (sudo)..."; \
	sudo $(CURDIR)/$(TARGET); \
	echo "C++ exited. Stopping Python GUI..."; \
	kill $$PYTHON_PID 2>/dev/null || true; \
	wait $$PYTHON_PID 2>/dev/null || true

clean:
	rm -f $(OBJ) $(TARGET)
	rm -f $(FIFO_MONITOR) $(FIFO_CONTROL)