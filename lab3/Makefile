CXX = g++
CXXFLAGS = -std=c++17 -Wall -pthread

all: server client

server: server.cpp
	$(CXX) $(CXXFLAGS) -o server server.cpp

client: client.cpp
	$(CXX) $(CXXFLAGS) -o client client.cpp

clean:
	rm -f server client

run-server: server
	./server

run-client: client
	./client

.PHONY: all clean run-server run-client
