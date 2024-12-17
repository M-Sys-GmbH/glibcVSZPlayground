CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c99 -pthread
TARGET = glibcVSZPlayground

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

clean:
	rm -f $(TARGET)
