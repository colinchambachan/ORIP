CXX = g++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -Wpedantic -Ithird_party

BIN = replay
SRC = src/replay_main.cpp
SERVE_BIN = serve
SERVE_SRC = src/serve_main.cpp

all: $(BIN) $(SERVE_BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

$(SERVE_BIN): $(SERVE_SRC)
	$(CXX) $(CXXFLAGS) $(SERVE_SRC) -o $(SERVE_BIN)

clean:
	rm -f $(BIN) $(SERVE_BIN)
