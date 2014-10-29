DEPTH = ../..

ifndef CXX
CXX = g++
endif

OS_ARCH := $(shell uname)

ifeq ($(OS_ARCH),Darwin)
PP_CXXFLAGS = \
	-I/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/include/c++/v1 \
	-I/Applications/Xcode.app/Contents/Developer/usr/lib/llvm-gcc/4.2.1/include \
  -I$(ASIO_ROOT)/include -DASIO_STANDALONE

CXXFLAGS = -I$(ASIO_ROOT)/include -DASIO_STANDALONE
endif

EXAMPLES = $(wildcard *.cpp)
EXAMPLES_PP = $(EXAMPLES:%.cpp=.pp.%.cpp)
EXAMPLE_EXES = $(EXAMPLES:%.cpp=%)

.PHONY: all
all: $(EXAMPLE_EXES)

$(EXAMPLES_PP): .pp.%.cpp: %.cpp
	$(DEPTH)/bin/resumable-pp $< $(PP_CXXFLAGS) > $@

$(EXAMPLE_EXES): %: .pp.%.cpp
	$(CXX) -std=c++1y -Wall -Wno-return-type $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(EXAMPLE_EXES) $(EXAMPLES_PP)
