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
 * @file    qregister.h
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @brief       
 */

#ifndef _QREGISTER_H
#define _QREGISTER_H

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
#include "state.h"
#include "misc.h"

using namespace std;

#define QMAP 		std::map<qsize_t, QState*>
#define QMAPITER 	std::map<qsize_t, QState*>::iterator

class QRegister {
private:
	int numQubit;
	int cpuCores;
	qsize_t maxStates;
	struct qregister_stat qstat;

public:
	QMAP qstore[QSTORE_PARTITION];
	QMAPITER qiter[QSTORE_PARTITION];
	int qubitTypes[MAX_QUBITS];

public:
	QRegister(int n);
	QRegister(QRegister *src);
	~QRegister(void);

	/* clear all quantum states */
	void clear(void) {
		#pragma omp parallel for
		for(int i=0; i<QSTORE_PARTITION; i++) {
			for(auto entry : qstore[i]) {
				putQState(entry.second);
			}
			qstore[i].erase(qstore[i].begin(), qstore[i].end());
		}
	}

	/* initialize quantum state to |00...00> */
	void init(void) {
		QState *q = getQState(0, complex_t(1, 0));
		qstore[0][0] = q;
		init_strides();
	}

	/* reset quantum state to |00...00> */
	void reset(void) {
		clear();
		init();
	}

	/* replace quantum state by using input QRegister */
	void replace(QRegister *qreg) {
		QState *Q;

		clear();

		qreg->setOrderedQState();
		while((Q = qreg->getOrderedQState()) != NULL) {
			QState *newQ = getQState(Q->getIndex(), Q->getAmplitude());
			addQState(newQ->getIndex(), newQ);
		}
	}

	/* return the number of qubits for this Q-register */
	int getNumQubits(void) { return numQubit; }

	/* return the number of CPU cores */
	int getCPUCores(void) { return cpuCores; }

	/* return the total number of non-zero amplitude states */
	qsize_t getNumStates() { 
		qsize_t states = 0;

		for(int i=0; i<QSTORE_PARTITION; i++) {
			states += qstore[i].size();
		}

		return states; 
	}

	/* return qstore partition id according to state index */
	int getPartId(qsize_t index) { return (int)(index % QSTORE_PARTITION); } 

public:
	/* add new state to the quantum register */
	void addQState(qsize_t index, QState *state) {
		QMAP *part = &qstore[getPartId(index)];
		(*part)[index] = state;
	}

	/*
	 * Removes the state from the quantum register.
	 * This function is called in two cases. 
	 *
	 * (1) Remove one of the superposed states after the measurement
	 * (2) Remove zero state after Gate operations
	 */
	void delQState(qsize_t index) {
		QMAP *part = &qstore[getPartId(index)];
		QMAPITER it;

		it = part->find(index);
		if(it != part->end()) {
			putQState(it->second);
			part->erase(it);
		}
	}

	/* search the quantum state corresponding to state index */
	QState *findQState(qsize_t index) {
		QMAP *part = &qstore[getPartId(index)];
		QMAPITER it;
		QState *Q = NULL;

		it = part->find(index);
		if(it != part->end()) {
			Q = it->second;
		}

		return Q;
	}

	/* Normalize amplitudes */
	void normalize(void) {
		double lengthm[QSTORE_PARTITION];
		double length = 0;

		for(int i=0; i<QSTORE_PARTITION; i++) {
			lengthm[i] = 0;
		}

		#pragma omp parallel for
		for(int i=0; i<QSTORE_PARTITION; i++) {
			QMAPITER it;
			for(it = qstore[i].begin(); it != qstore[i].end(); it++) {
				QState *Q = it->second;
				lengthm[i] += norm(Q->getAmplitude());
			}
		}

		for(int i=0; i<QSTORE_PARTITION; i++) {
			length += lengthm[i];
		}
		length = std::sqrt(length);

		#pragma omp parallel for
		for(int i=0; i<QSTORE_PARTITION; i++) {
			QMAPITER it;
			for(it = qstore[i].begin(); it != qstore[i].end(); it++) {
				QState *Q = it->second;
				Q->resizeAmplitude(length);
			}
		}
	}

	/* set all qstore iterators to the starting position */
	void setOrderedQState() {
		for(int i =0; i<QSTORE_PARTITION; i++) {
			qiter[i] = qstore[i].begin();
		}
	}

	/* returns the quantum state in state index order */
	QState *getOrderedQState() {
		QState *Q = NULL;
		qsize_t minState = maxStates + 1;
		int iterPos = -1;

		for(int i =0; i<QSTORE_PARTITION; i++) {
			if(qiter[i] == qstore[i].end()) {
				continue;
			}
			
			if(qiter[i]->second->getIndex() < minState) {
				Q = qiter[i]->second;
				iterPos = i;
				minState = qiter[i]->second->getIndex();
			}
		}

		if(iterPos != -1) {
			qiter[iterPos]++;
		}

		return Q;
	}

	/* cleanup all zero amplitude states */
	void clearZeroStates() {
		#pragma omp parallel for
		for(int i=0; i<QSTORE_PARTITION; i++) {
			QMAPITER it = qstore[i].begin();
			while(it != qstore[i].end()) {
				QState *Q = it->second;
				if(abs(Q->getAmplitude()) == 0) {
					putQState(Q);
					qstore[i].erase(it++);
				} else {
					++it;
				}
			}
		}
	}

public:
	void updateQRegStat(int gate, QTimer timer);
	struct qregister_stat getQRegStat(void);
	void showQRegStat(void);
	void checkMemory(void);
};

#endif
