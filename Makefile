CXX ?= g++
CXXFLAGS = -g -DDEBUG -fPIC
target = myServer
binPath = ./bin/
server: main.cpp http_conn.cpp sqlconnpool.cpp sqlconnRAII.cpp
	$(CXX) -o $(binPath)$(target) $^ $(CXXFLAGS) -lpthread -lmysqlclient
clean:
	rm  -r $(binPath)$(target)

