CC = g++
CFLAGS = -Wall -Wextra -O2

SRC = main.c \
      core/monitor/cpu_utilize.cpp \
	  core/monitor/energy.cpp

OBJ = $(SRC:.c=.o)

TARGET = os_manager

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	sudo ./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)
