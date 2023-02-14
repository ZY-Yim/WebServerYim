CXX ?= g++
CXXFLAGS = -g -DDEBUG -fPIC
target = myServer
binPath = ./bin/
server: main.cpp http_conn.cpp 
	$(CXX) -o $(binPath)$(target) $^ $(CXXFLAGS) -lpthread 
clean:
	rm  -r $(binPath)$(target)

