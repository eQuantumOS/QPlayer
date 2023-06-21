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

SPool::SPool(void)
{
	for(int i=0; i<MAX_POOLS; i++) {
		for(int j=0; j<MAX_POOL_UNITS; j++) {
			QState *Q = new QState(0, complex_t(0, 0));
			pool[i].push(Q);
		}
	}
}	

SPool::~SPool(void) 
{
	for(int i=0; i<MAX_POOLS; i++) {
		while(!pool[i].empty()) {
			QState *Q = pool[i].front();
			pool[i].pop();
			delete Q;
		}
	}
}

QState *SPool::deque(void) 
{
	QState *Q = NULL;
	int sidx = rand() % MAX_POOLS;

	lock(sidx);

	if(pool[sidx].empty()) {
		for(int i=0; i<MAX_POOL_UNITS; i++) {
			QState *Q = new QState(0, complex_t(0, 0));
			pool[sidx].push(Q);
		}
	} 

	Q = pool[sidx].front();
	pool[sidx].pop();

	unlock(sidx);

	return Q;
}

QState *SPool::deque(qsize_t qidx) 
{
	QState *Q = NULL;
	int sidx = (int)(qidx % (qsize_t)MAX_POOLS);

	lock(sidx);

	if(pool[sidx].empty()) {
		for(int i=0; i<MAX_POOL_UNITS; i++) {
			QState *Q = new QState(0, complex_t(0, 0));
			pool[sidx].push(Q);
		}
	} 

	Q = pool[sidx].front();
	pool[sidx].pop();

	unlock(sidx);

	return Q;
}

void SPool::enque(QState *Q) 
{
	int sidx = (int)(Q->getIndex() % (qsize_t)MAX_POOLS);

	lock(sidx);
#if 0
	if(pool[sidx].size() < MAX_POOL_SIZE) {
		pool[sidx].push(Q);
	} else {
		delete Q;
	}
#else
	pool[sidx].push(Q);
#endif
	unlock(sidx);
}
