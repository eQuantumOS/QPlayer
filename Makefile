#*******************************************************************************
#
#       Electronics and Telecommunications Research Institute
#
#  Copyright(c) 2021 Electronics and Telecommunications Research Institute,
#  161, Gajeong-Dong, Yuseong-Gu, Daejeon, Korea.
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

all: QPLAYER CIRCUIT

QPLAYER:
	@ $(MAKE) -C qplayer boost=yes

CIRCUIT:
	@ $(MAKE) -C circuit boost=yes

clean:
	@ $(MAKE) -C circuit clean
	@ $(MAKE) -C qplayer clean
	@ $(RM) build/include/*
	@ $(RM) build/lib/*
