CC=gcc
CFLAGS=-Wall -Wextra -O2 -g -pthread
LDFLAGS=
SRCS=src/main.c src/broker.c src/proto.c
OBJS=$(SRCS:.c=.o)
TARGET=brokerd

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o $(TARGET)
