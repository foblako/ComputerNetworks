CC = gcc
CFLAGS = -Wall -std=c11
LIBS = -lpthread

all: server client

server: server.c utils.c message.h
	$(CC) $(CFLAGS) -o server.exe server.c utils.c $(LIBS)

client: client.c utils.c message.h
	$(CC) $(CFLAGS) -o client.exe client.c utils.c $(LIBS)

clean:
	rm -f server client *.o

.PHONY: all clean