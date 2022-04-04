big_number=no

CXX = g++
CXXLD = g++

QPLAYER_RELEASE=../../build

CXXFLAGS = -g -Wall -O2 -std=c++11 -MMD -MP -fopenmp
CXXFLAGS += -I. -I$(QPLAYER_RELEASE)/include
CXXFLAGS += -Wno-reorder -Wno-sign-compare -Wno-parentheses -Wno-pessimizing-move
CXXFLAGS += -Wno-unused-function -Wno-unused-variable -Wno-write-strings
CXXFLAGS += -Wno-class-conversion -Wno-unused-result
ifeq ($(big_number),yes)
    CXXFLAGS += -I/opt/boost_1.61/include
    CXXFLAGS += -DBIG_NUMBER
endif

LOADLIBES = -L$(QPLAYER_RELEASE)/lib
LOADLIBES += -lm -lpthread -lgomp -lqplayer
ifeq ($(big_number),yes)
	LDFLAGS = -L/opt/boost_1.61/lib
	LIBS += -lboost_serialization-mt -lboost_filesystem-mt -lboost_system-mt
endif

SRCS=$(wildcard *.cpp)
TARGETS=$(patsubst %.cpp,%,$(SRCS))

all: $(TARGETS) 

clean:
	@echo "Clean Objects, Deps, Executions"
	@rm -rf logs
	@rm -fv core.* 
	@rm -fv *.d
	@rm -fv $(TARGETS)
