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
 * @file    spool.h
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @brief       
 */

#ifndef _QPOOL_H
#define _QPOOL_H

#include <queue>
#include <mutex>

#include "precision.h"

using namespace std;

#define MAX_POOLS			127
#define MAX_POOL_SIZE		1000
#define MAX_POOL_UNITS 		10

class SPool {
private:
	queue<QState *> pool[MAX_POOLS];
	mutex locks[MAX_POOLS];

public:
	SPool(void);
	~SPool(void);

	QState *deque(void);
	QState *deque(qsize_t qidx);
	void enque(QState *Q);

	void lock(int sidx) { locks[sidx].lock(); }
	void unlock(int sidx) { locks[sidx].unlock(); }

	void show(void) {
		int count = 0;

		for(int i=0; i<MAX_POOLS; i++) {
			count += (int)pool[i].size();
		}

		printf("Total Free Entries : %d\n", count);
	}
};

#endif
