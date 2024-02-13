QPLAYER_RELEASE=../../release

CXX = g++
CXXLD = g++

CXXFLAGS = -g -Wall -O2 -std=c++11 -MMD -MP -fopenmp
CXXFLAGS += -I. -I./include -I$(QPLAYER_RELEASE)/include
CXXFLAGS += -Wno-reorder -Wno-sign-compare -Wno-parentheses -Wno-pessimizing-move
CXXFLAGS += -Wno-unused-function -Wno-unused-variable -Wno-write-strings
CXXFLAGS += -Wno-class-conversion -Wno-unused-result

LIBS = -L$(QPLAYER_RELEASE)/lib
LIBS += -lm -lpthread -lqplayer -lgomp

Q=@echo
E=@

QASM_SRC :=
QASM_HDR :=
include ./module.mk

# OBJECT FILES
QASM_OBJS=$(patsubst %.cpp,%.o,$(QASM_SRC))
QASM_HDRS=$(patsubst %.h,%.h,$(QASM_HDR))

QASM_EXEC=qplayer
QASM_DEPS=src/*.d ./*.d

.PHONY: all clean

.DEFAULT_GOAL := all

all: QPlayerQASM install

QPlayerQASM: $(QASM_OBJS) 
	$(CXXLD) $(LDFLAGS) $(QASM_OBJS) -o $(QASM_EXEC) $(LIBS) 

%.o : %.cpp $(QASM_HDRS) 
	$(Q) "CC	$(@)"
	$(E) $(CXX) $(CXXFLAGS) -Wno-deprecated-declarations $(LIB_CXX_FLAGS) -c $< -o $@

install:
	@mkdir -p $(QPLAYER_RELEASE)/bin
	@cp -f $(QASM_EXEC) $(QPLAYER_RELEASE)/bin
	@cp -rf ./examples $(QPLAYER_RELEASE)/bin
	@cp -f ./qelib1.inc $(QPLAYER_RELEASE)/bin

clean:
	@echo "Clean Objects, Deps, Executions"
	@rm -rf logs
	@rm -fv core.* 
	@rm -fv $(QASM_OBJS)
	@rm -fv $(QASM_EXEC)
	@rm -fv $(QASM_DEPS)
