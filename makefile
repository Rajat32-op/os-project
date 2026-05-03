CC     = g++
CFLAGS = -Wall -Wextra -O2 -std=c++17
LFLAGS = -lm

SRC = main.cpp \
      core/monitor/monitor_manager.cpp \
      core/controllers/event_controller.cpp \
      core/classifier/classifier.cpp \
      core/calibrator/calibrator.cpp \
      core/policy/governor.cpp \
      core/policy/adaptive_policy_engine.cpp

OBJ = $(SRC:.cpp=.o)

TARGET       = os_manager
FIFO_MONITOR = config/monitor_pipe
FIFO_CONTROL = config/event_pipe

HEADERS = core/monitor/monitor_manager.h \
	  core/controllers/event_controller.h \
	  core/classifier/classifier.h \
	  core/calibrator/calibrator.h \
	  core/policy/governor.h \
	  core/policy/adaptive_policy_engine.h

.PHONY: all clean run setup install reset-rl

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ) $(LFLAGS)

%.o: %.cpp $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

setup:
	mkdir -p config results results/raw
	[ -p $(FIFO_MONITOR) ] || mkfifo $(FIFO_MONITOR)
	[ -p $(FIFO_CONTROL) ] || mkfifo $(FIFO_CONTROL)
	[ -p /tmp/state_fifo ] || mkfifo /tmp/state_fifo
	[ -p /tmp/action_fifo ] || mkfifo /tmp/action_fifo

	@echo "--- Setting up sudoers entry ---"
	echo "$$USER ALL=(ALL) NOPASSWD: $(CURDIR)/$(TARGET)" | \
	sudo tee /etc/sudoers.d/os_manager
	sudo chmod 440 /etc/sudoers.d/os_manager
	@echo "--- Running baseline calibration (keep system IDLE for 10s) ---"
	if [ ! -f config/calibration.json ]; then sudo ./$(TARGET) --calibrate; fi

run: all setup
	@echo "Starting Python GUI..."
	@python3 interface/monitor.py & \
	PYTHON_PID=$$!; \
	echo "Starting RL Agent..."; \
	python3 rl_agent.py & \
	RL_PID=$$!; \
	sleep 2; \
	echo "Starting C++ monitor..."; \
	sudo env OSMGR_MODE=RL OSMGR_CSV=results/raw/rl.csv OSMGR_SUMMARY=results/raw/rl.summary.csv ./$(TARGET); \
	echo "C++ exited, cleaning up..."; \
	kill $$PYTHON_PID 2>/dev/null || true; \
	kill $$RL_PID 2>/dev/null || true
	rm -f $(OBJ) $(TARGET)

run-baseline: all setup
	@echo "Starting Python GUI..."
	@python3 interface/monitor.py & \
	PYTHON_PID=$$!; \
	sleep 2; \
	echo "Starting C++ monitor in baseline mode..."; \
	sudo env OSMGR_MODE=BASELINE OSMGR_CSV=results/raw/baseline.csv OSMGR_SUMMARY=results/raw/baseline.summary.csv ./$(TARGET); \
	echo "C++ exited, cleaning up..."; \
	kill $$PYTHON_PID 2>/dev/null || true

run-rl: all setup
	@echo "Starting Python GUI..."
	@python3 interface/monitor.py & \
	PYTHON_PID=$$!; \
	echo "Starting RL Agent..."; \
	python3 rl_agent.py & \
	RL_PID=$$!; \
	sleep 2; \
	echo "Starting C++ monitor in RL mode..."; \
	sudo env OSMGR_MODE=RL OSMGR_CSV=results/raw/rl.csv OSMGR_SUMMARY=results/raw/rl.summary.csv ./$(TARGET); \
	echo "C++ exited, cleaning up..."; \
	kill $$PYTHON_PID 2>/dev/null || true; \
	kill $$RL_PID 2>/dev/null || true

reset-rl:
	@echo "Resetting RL Agent Q-Table..."
	rm -f q_table.json
	@echo "Done. RL Agent will start fresh next run."
