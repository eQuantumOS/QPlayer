QPLAYER_RELEASE=../../release

CXX = g++
CXXLD = g++

CXXFLAGS = -g -Wall -O2 -std=c++11 -MMD -MP -fopenmp
CXXFLAGS += -I. -I$(QPLAYER_RELEASE)/include
CXXFLAGS += -Wno-reorder -Wno-sign-compare -Wno-parentheses -Wno-pessimizing-move
CXXFLAGS += -Wno-unused-function -Wno-unused-variable -Wno-write-strings
CXXFLAGS += -Wno-class-conversion -Wno-unused-result
CXXFLAGS += -DLARGE_SCALE

LIBS = -L$(QPLAYER_RELEASE)/lib
LIBS += -lm -lpthread -lgomp -lqplayer -fopenmp -lboost_system

Q=@echo
E=@

MLQ_SRC :=
MLQ_HDR :=
include ./module.mk

# OBJECT FILES
MLQ_OBJS=$(patsubst %.cpp,%.o,$(MLQ_SRC))
MLQ_HDRS=$(patsubst %.h,%.h,$(MLQ_HDR))

MLQ_EXEC=mlq
MLQ_DEPS=./*.d

.PHONY: all clean

.DEFAULT_GOAL := all

all: mlq 

mlq: $(MLQ_OBJS) 
	$(CXXLD) $(LDFLAGS) $(MLQ_OBJS) -o $(MLQ_EXEC) $(LIBS) 

%.o : %.cpp $(MLQ_HDRS) 
	$(Q) "CC	$(@)"
	$(E) $(CXX) $(CXXFLAGS) -Wno-deprecated-declarations $(LIB_CXX_FLAGS) -c $< -o $@

clean:
	@echo "Clean Objects, Deps, Executions"
	@rm -rf logs
	@rm -fv core.* 
	@rm -fv $(MLQ_OBJS)
	@rm -fv $(MLQ_EXEC)
	@rm -fv $(MLQ_DEPS)
