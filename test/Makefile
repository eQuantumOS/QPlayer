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

all: GENERAL SURFACECODE VERIFICATION BENCHMARK

GENERAL:
	@ $(MAKE) -C general 

SURFACECODE:
	@ $(MAKE) -C surfacecode 

MLQ:
	@ $(MAKE) -C MLQ 

VERIFICATION:
	@ $(MAKE) -C verification 

BENCHMARK:
	@ $(MAKE) -C benchmark 

clean:
	@ $(MAKE) -C general clean
	@ $(MAKE) -C surfacecode clean
	@ $(MAKE) -C MLQ clean
	@ $(MAKE) -C verification clean
	@ $(MAKE) -C benchmark clean
