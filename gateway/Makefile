CC=gcc
CFLAGS=-Wall -Wextra -O2 -g -pthread
TARGET_GATEWAY=gatewayd
TARGET_PUB=publisher_sim

all: $(TARGET_GATEWAY) $(TARGET_PUB)

$(TARGET_GATEWAY): gateway.c
	$(CC) $(CFLAGS) gateway.c -o $(TARGET_GATEWAY)

$(TARGET_PUB): publisher_sim.c
	$(CC) $(CFLAGS) publisher_sim.c -o $(TARGET_PUB)

clean:
	rm -f $(TARGET_GATEWAY) $(TARGET_PUB)
