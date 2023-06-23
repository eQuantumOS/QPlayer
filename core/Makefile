CXX = g++
CXXLD = g++

CXXFLAGS = -g -Wall -O2 -std=c++11 -MMD -MP -fopenmp
CXXFLAGS += -I. -I./include
CXXFLAGS += -Wno-reorder -Wno-sign-compare -Wno-parentheses -Wno-pessimizing-move
CXXFLAGS += -Wno-unused-function -Wno-unused-variable -Wno-write-strings
CXXFLAGS += -Wno-class-conversion -Wno-unused-result -Wno-class-memaccess
CXXFLAGS += -DLARGE_SCALE

LIBS = -lm -lpthread -lgomp
LIBS += -lboost_system-mt

Q=@echo
E=@

BUILD_DIR = ../release

QPLAYER_SRC :=
QPLAYER_HDR :=
CIRCUIT_HDR :=
include ./module.mk

# OBJECT FILES
QPLAYER_OBJS=$(patsubst %.cpp,%.o,$(QPLAYER_SRC))
QPLAYER_HDRS=$(patsubst %.h,%.h,$(QPLAYER_HDR))

QPLAYER_LIB=libqplayer.a

.PHONY: all clean

.DEFAULT_GOAL := all

all: qplayer install

qplayer: $(QPLAYER_OBJS) 
	ar ruv $(QPLAYER_LIB) $(QPLAYER_OBJS)

%.o : %.cpp $(QPLAYER_HDRS) $(CIRCUIT_HDRS)
	$(Q) "CC	$(@)"
	$(E) $(CXX) $(CXXFLAGS) -Wno-deprecated-declarations $(LIB_CXX_FLAGS) -c $< -o $@

install:
	$(Q) "$(@)"
	@mkdir -p $(BUILD_DIR)/include
	@mkdir -p $(BUILD_DIR)/lib
	@mkdir -p $(BUILD_DIR)/bin
	@cp -f include/* $(BUILD_DIR)/include
	@mv -f $(QPLAYER_LIB) $(BUILD_DIR)/lib

clean:
	@echo "Clean Objects, Deps, Executions"
	@rm -fv src/*.d
	@rm -fv $(QPLAYER_OBJS)
	@rm -fv $(QPLAYER_EXEC)
	@rm -fv $(QPLAYER_DEPS)
	@rm -fv $(QPLAYER_LIB)
