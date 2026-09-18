CXX = g++
CXXFLAGS = -Wall -Wextra -g -Iinclude -std=c++17
LIBS = -libverbs -lrdmacm

SRCS = main.cpp src/mr_pool.cpp src/qp_pool.cpp src/transfer.cpp src/handshake.cpp
OBJS = $(SRCS:.cpp=.o) 
TARGET = mooncake_lite

BENCH_SRCS = bench/bench.cpp src/mr_pool.cpp src/qp_pool.cpp src/transfer.cpp src/handshake.cpp
BENCH_TARGET = mooncake_bench.o

$(BENCH_TARGET): $(BENCH_SRCS)
	$(CXX) $(CXXFLAGS) $(BENCH_SRCS) -o $@ $(LIBS)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)