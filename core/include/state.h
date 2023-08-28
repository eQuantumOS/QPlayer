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
 * @file    state.h
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @brief       
 */

#ifndef _QSTATE_H
#define _QSTATE_H

#include <iostream>
#include <cstring>
#include <bitset>
#include <vector>
#include <map>
#include <mutex>

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <omp.h>

#include "precision.h"
#include "timer.h"

using namespace std;

class QState {
private:
	qsize_t index;
	complex_t amplitude;
#ifndef ENABLE_NMC
	size_t stage;
#endif

public:
	QState(void);
	QState(qsize_t i, complex_t a);
	QState(QState *Q);
	~QState(void);

	complex_t getAmplitude() { return amplitude; }
	qsize_t getIndex() { return index; }

	void setIndex(qsize_t i) { index = i; }
	void setAmplitude(complex_t a) { amplitude = a; }
	void resizeAmplitude(complex_t a) { amplitude /= a; }
#ifndef ENABLE_NMC
	void setStage(size_t s) { stage = s; }
	size_t getStage(void) { return stage; }
#endif
};

extern QState *getQState(void);
extern QState *getQState(qsize_t qidx, complex_t amp);
extern void putQState(QState *Q);

#endif
