CXX = g++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -Wpedantic -Ithird_party

BIN = replay
SRC = src/replay_main.cpp

all: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

clean:
	rm -f $(BIN)
