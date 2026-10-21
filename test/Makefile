CXX ?= g++
CXXFLAGS ?= -O2 -std=c++14 -Wall -Wextra -Wno-unused-result

harness: harness.cpp ../core/horizon_engine.h ../core/horizon_modes.h
	$(CXX) $(CXXFLAGS) -o $@ harness.cpp -lm

test: harness
	./harness out

clean:
	rm -rf harness out

.PHONY: test clean
