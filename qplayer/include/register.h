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

class QRegister {
private:
	int numQubit;
	short curStage;
	qsize_t maxStates;

public:
	std::map<qsize_t, QState*> qstore[QSTORE_PARTITION];
	std::map<qsize_t, QState*>::iterator qiter[QSTORE_PARTITION];
	std::mutex qlock[QSTORE_PARTITION];
	std::mutex slock;

public:
	QRegister(int n);
	~QRegister(void);

	/* clear all quantum states */
	void clear(void) {
		for(int i=0; i<QSTORE_PARTITION; i++) {
			for(auto entry : qstore[i]) {
				delete entry.second;
			}
			qstore[i].erase(qstore[i].begin(), qstore[i].end());
		}
	}

	/* initialize quantum state to |00...00> */
	void init(void) {
		QState *q = new QState(0, 1.0);
		qstore[0][0] = q;
		curStage = 0;
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
			QState *newQ = new QState(Q->getIndex(), Q->getAmplitude());
			setQState(newQ->getIndex(), newQ);
		}
	}

	/* return the number of qubits for this Q-register */
	int getNumQubits(void) { return numQubit; }

	/* return the total number of non-zero amplitude states */
	qsize_t getNumStates() { 
		qsize_t states = 0;

		for(int i=0; i<QSTORE_PARTITION; i++) {
			states += qstore[i].size();
		}

		return states; 
	}

	/* return qstore partition id according to state index */
	int getPartId(qsize_t index) { 
		return (int)(index % QSTORE_PARTITION); 
	} 

	/* increase operation stage */
	short incStage(void) { return ++curStage; }

	/* qstore lock & unlock */
	void QLock(int index) { qlock[index].lock(); }
	void QUnlock(int index) { qlock[index].unlock(); } 
	void QLock(qsize_t index) { qlock[getPartId(index)].lock(); }
	void QUnlock(qsize_t index) { qlock[getPartId(index)].unlock(); } 

public:
	/* set check & set operation state */
	int checkStage(QState *s0, QState *s1, qsize_t lower_idx, short stage) {
		int ret = 0;

		QLock(lower_idx);

		if(s0 != NULL) {
			if(s0->getStage() >= stage) {
				ret = -1;
			} else {
				s0->setStage(stage);
			}
		}

		if(s1 != NULL) {
			if(s1->getStage() >= stage) {
				ret = -1;
			} else {
				s1->setStage(stage);
			}
		}

		QUnlock(lower_idx);

		return ret;
	}
	

	/* add new state to the quantum register */
	void setQState(qsize_t index, QState *state) {
		std::map<qsize_t, QState*> *part = &qstore[getPartId(index)];

		QLock(index);

		state->setStage(curStage);
		(*part)[index] = state;

		QUnlock(index);
	}

	/*
	 * Removes the state from the quantum register.
	 * This function is called in two cases. 
	 *
	 * (1) Remove one of the superposed states after the measurement
	 * (2) Remove zero state after Gate operations
	 */
	void eraseQState(qsize_t index) {
		std::map<qsize_t, QState*>::iterator it;
		std::map<qsize_t, QState*> *part = &qstore[getPartId(index)];

		QLock(index);

		it = part->find(index);
		if(it != part->end()) {
			delete it->second;
			part->erase(it);
		}

		QUnlock(index);
	}

	/* search the quantum state corresponding to state index */
	QState *findQState(qsize_t index) {
		std::map<qsize_t, QState*>::iterator it;
		std::map<qsize_t, QState*> *part = &qstore[getPartId(index)];
		QState *Q = NULL;

		QLock(index);

		it = part->find(index);
		if(it != part->end()) {
			Q = it->second;
		}

		QUnlock(index);

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
			std::map<qsize_t, QState*>::iterator it;
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
			std::map<qsize_t, QState*>::iterator it;
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
			std::map<qsize_t, QState*>::iterator it = qstore[i].begin();
			while(it != qstore[i].end()) {
				QState *Q = it->second;
				if(abs(Q->getAmplitude()) == 0) {
					delete Q;
					qstore[i].erase(it++);
				} else {
					++it;
				}
			}
		}
	}
};

#endif
