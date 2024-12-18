CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c99 -pthread -g
TARGET = glibcVSZPlayground
TARGET_2 = pmap-poc

all: $(TARGET) $(TARGET_2)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

$(TARGET_2): read-pmap-poc.c
	$(CC) $(CFLAGS) -o $(TARGET_2) read-pmap-poc.c

clean:
	rm -f $(TARGET)
	rm -f $(TARGET_2)
