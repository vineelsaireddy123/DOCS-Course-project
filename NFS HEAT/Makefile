CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -pthread

TARGETS = naming_server storage_server client

all: $(TARGETS)

naming_server: naming_server.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

storage_server: storage_server.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

client: client.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c common.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o $(TARGETS) *.log

.PHONY: all clean