#*******************************************************************************
#
#       Electronics and Telecommunications Research Institute
#
#  Copyright(c) 2021 Electronics and Telecommunications Research Institute,
#  218 Gajeong-ro, Yuseong-gu, Daejeon, 34129, Korea.
#  All rights are reserved. No part of this work covered by the copyright
#  hereon may be reproduced, stored in retrieval systems, in any form or by
#  any means, electronic, mechanical, photocopying, recording or otherwise,
#  without the prior permission of ETRI.
#
#  FILE NAME    : Makefile
#
#  AUTHOR   :
#  DESCRIPTION  : Makefile for QPlayer
#
#*******************************************************************************

all: CORE QASM

CORE:
	@ $(MAKE) -C core 

QASM:
	@ $(MAKE) -C qasm 

TEST:
	@ $(MAKE) -C test/general 
	@ $(MAKE) -C test/surfacecode 
	@ $(MAKE) -C test/MLQ 

clean:
	@ $(MAKE) -C core clean
	@ $(MAKE) -C qasm clean
	@ $(MAKE) -C test/general clean
	@ $(MAKE) -C test/surfacecode clean
	@ $(MAKE) -C test/MLQ clean
	@ $(RM) -rf release
