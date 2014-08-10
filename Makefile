

sanitize = -fsanitize=address -fsanitize=leak -fsanitize=undefined
CXXFLAGS = -std=gnu++1y -g -Wall -O0 -MD -MT $@ -MP -flto $(sanitize)

tests = test-reactor

all: kassandra $(tests)

clean:
	rm kassandra $(tests) *.o

kassandra: main.o reactor.o
	$(CXX) $(CXXFLAGS) -o $@ $^

test-reactor: test-reactor.o reactor.o
	$(CXX) $(CXXFLAGS) -o $@ $^


-include *.d
