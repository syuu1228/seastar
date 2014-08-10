

CXXFLAGS = -std=gnu++1y -g -Wall -O2 -MD -MT $@ -MP -flto

tests = test-reactor

all: kassandra $(tests)

kassandra: main.o reactor.o
	$(CXX) $(CXXFLAGS) -o $@ $^

test-reactor: test-reactor.o reactor.o
	$(CXX) $(CXXFLAGS) -o $@ $^


-include *.d
