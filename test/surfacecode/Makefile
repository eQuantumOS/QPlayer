QPLAYER_RELEASE=../../release

CXX = g++
CXXLD = g++

CXXFLAGS = -g -Wall -O2 -std=c++11 -fopenmp
CXXFLAGS += -I. -I$(QPLAYER_RELEASE)/include
CXXFLAGS += -Wno-reorder -Wno-sign-compare -Wno-parentheses -Wno-pessimizing-move
CXXFLAGS += -Wno-unused-function -Wno-unused-variable -Wno-write-strings
CXXFLAGS += -Wno-class-conversion -Wno-unused-result

LOADLIBES = -L$(QPLAYER_RELEASE)/lib
LOADLIBES += -lm -lpthread -lgomp -lqplayer

SRCS=$(wildcard *.cpp)
TARGETS=$(patsubst %.cpp,%,$(SRCS))

Q=@echo
E=@

all: $(TARGETS) 

clean:
	@echo "Clean Objects, Deps, Executions"
	@rm -rf logs
	@rm -fv core.* 
	@rm -fv *.d
	@rm -fv $(TARGETS)
