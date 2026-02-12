CC = gcc
CFLAGS = -Wall -Wextra -O2

SRC = main.c \
      core/monitor/cpu_utilize.c

OBJ = $(SRC:.c=.o)

TARGET = os_manager

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)
