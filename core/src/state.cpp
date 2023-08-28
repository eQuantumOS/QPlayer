/*
 * Copyright (c) 2021 Electronics and Telecommunications Research Institute 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/**
 * @file    state.cpp
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @date    10-09-21
 * @brief       
 */

#include "state.h"
#include "spool.h"

using namespace std;

#if 0
#define ENABLE_SPOOL
#endif

static SPool *spool = NULL;

QState::QState(void) 
{
	index = 0;
	amplitude = complex_t(0, 0);
#ifndef ENABLE_NMC
	stage = 0;
#endif
}	

QState::QState(qsize_t i, complex_t a) 
{
	index = i;
	amplitude = a;
#ifndef ENABLE_NMC
	stage = 0;
#endif
}	

QState::QState(QState *Q) 
{
	index = Q->getIndex();
	amplitude = Q->getAmplitude();
#ifndef ENABLE_NMC
	stage = 0;
#endif
}	

QState::~QState(void) 
{
}

/****************************************************/
/*      external APIs for alloc/dealloc QState      */
/****************************************************/
QState *getQState(void) 
{
	QState *Q = NULL;

#ifdef ENABLE_SPOOL
	/* if first request, allocate state pool */
	if(spool == NULL) {
		spool = new SPool();
	}

	Q = spool->deque();
#else
	Q = new QState();
#endif

	return Q;
}

QState *getQState(qsize_t qidx, complex_t amp) 
{
	QState *Q = NULL;

#ifdef ENABLE_SPOOL
	/* if first request, allocate state pool */
	if(spool == NULL) {
		spool = new SPool();
	}

	Q = spool->deque(qidx);
	Q->setIndex(qidx);
	Q->setAmplitude(amp);
#else
	Q = new QState(qidx, amp);
#endif

	return Q;
}

void putQState(QState *Q) 
{
#ifdef ENABLE_SPOOL
	/* if first request, allocate state pool */
	if(spool == NULL) {
		spool = new SPool();
	}

	spool->enque(Q);
#else
	delete Q;
#endif
}
