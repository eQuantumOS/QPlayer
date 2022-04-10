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
 * @date    10-09-21
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
#include "timer.h"

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

	/* 
	 * Reset quantum state to |00...00> 
	 */
	void reset(void) {
		clear();
		init();
	}

	/* 
	 * Initialize quantum state to |00...00> 
	 */
	void init(void) {
		QState *q = new QState(0, 1.0);
		qstore[0][0] = q;
		curStage = 0;
	}

	/* 
	 * Replace quantum state by using input QRegister 
	 */
	void replace(QRegister *qreg) {
		QState *Q;

		/* clear all states of this register */
		clear();

		qreg->setOrderedQState();
		while((Q = qreg->getOrderedQState()) != NULL) {
			QState *newQ = new QState(Q->getIndex(), Q->getAmplitude());
			setQState(newQ->getIndex(), newQ);
		}
	}

	/* 
	 * QRegister clear all states 
	 */
	void clear(void) {
		for(int i=0; i<QSTORE_PARTITION; i++) {
			for(auto entry : qstore[i]) {
				delete entry.second;
			}
			qstore[i].erase(qstore[i].begin(), qstore[i].end());
		}
	}

	/* 
	 * Returns the number of qubits for this Q-register 
	 */
	int getNumQubits(void) { return numQubit; }

	/* 
	 * Returns the total number of non-zero amplitude states 
	 */
	qsize_t numStates() { 
		qsize_t states = 0;

		for(int i=0; i<QSTORE_PARTITION; i++) {
			states += qstore[i].size();
		}

		return states; 
	}

	/* 
	 * Returns qstore partition id according to state index 
	 */
	int getPartId(qsize_t index) { return (int)(index % QSTORE_PARTITION); } 

	/* 
	 * qstore lock & unlock 
	 */
	void QLock(int index) { qlock[index].lock(); }
	void QUnlock(int index) { qlock[index].unlock(); } 
	void QLock(qsize_t index) { qlock[getPartId(index)].lock(); }
	void QUnlock(qsize_t index) { qlock[getPartId(index)].unlock(); } 

	void SLock(void) { slock.lock(); }
	void SUnlock(void) { slock.unlock(); } 

	short incStage(void) { return ++curStage; }

	/* 
	 * Returns maximum allocated physical memory size. QPlayer uses VmHWM 
	 * values because the memory size changes dynamically depending on the 
	 * number of non-zero amplitude states. 
	 */
	void checkMemory(void) {
		FILE *fp = NULL;
		char fname[256] = "";
		char cmd[256] = "";
		char size[32] = "";

		sprintf(fname, "/proc/%d/status", getpid());

		fp = fopen(fname, "r");
		if(fp == NULL) {
			return;
		}

		while(fgets(cmd, 256, fp) != NULL) {
			if(strstr(cmd, "VmHWM")) {
				char t[32] = "";
				sscanf(cmd, "%s%s", t, size);
				break;
			}
		}

		printf("allocated memory = %s KB\n", size);

		fclose(fp);

		return;
	}

public:
	/*
	 * Set check & set operation state 
	 */
	int checkStage(QState *s0, QState *s1, qsize_t lower_idx, short stage) {
		int ret = 0;

	#if 0
		SLock();
	#else
		QLock(lower_idx);
	#endif

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

	#if 0
		SUnlock();
	#else
		QUnlock(lower_idx);
	#endif

		return ret;
	}
	

	/*
	 * Add new state to the quantum register 
	 */
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

	/* 
	 * Search the quantum state corresponding to state index 
	 */
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

	/* 
	 * Normalize amplitudes 
	 */
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
		length = sqrt(length);

		#pragma omp parallel for
		for(int i=0; i<QSTORE_PARTITION; i++) {
			std::map<qsize_t, QState*>::iterator it;
			for(it = qstore[i].begin(); it != qstore[i].end(); it++) {
				QState *Q = it->second;
				Q->resizeAmplitude(length);
			}
		}
	}

	/* 
	 * Set all qstore iterators to the starting position
	 */
	void setOrderedQState() {
		for(int i =0; i<QSTORE_PARTITION; i++) {
			qiter[i] = qstore[i].begin();
		}
	}

	/* 
	 * Returns the quantum state in state index order
	 */
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

	/*
	 * Cleanup all zero amplitude states
	 */
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

public:
	/* 
	 * Displays quantum state. Each qubit is printed in reverse order.
	 *
	 * param state : index of quantum states
	 * param nq    : total number of qubits
  	 *
	 * Let's suppose a mixed state of two qubits as follows. 
	 *  - number of qubit : 2
	 *  - quantum states  : 4 2^2
	 *  - outputs are...
	 *    if(state == 0)      --> |00>
	 *    else if(state == 1) --> |01>   --> q0=1, q1=0
	 *    else if(state == 2) --> |10>   --> q0=0, q1=1
	 *    else if(state == 3) --> |11>
	*/
	void to_binary(qsize_t state, qsize_t nq, char *qstring)
	{
		int pos = 0;
		while (nq--) {
			qstring[pos++] = ((quantum_shiftR(state, nq)) & 1) ? '1' : '0';
		}
	}

	/* 
	 * dump() prints out all 2^N states in the quantum register.
	 * However, the zero * amplitude state is not included.
	 */
	void dump(int begin, int end, struct qubit_delimiter *qd) 
	{
		QState *Q;
		qsize_t totalStates = 0;
		for(int i=0; i<QSTORE_PARTITION; i++) {
			totalStates += qstore[i].size();
		}
	
		setOrderedQState();
		while((Q = getOrderedQState()) != NULL) {
			double real = Q->getAmplitude().real();
			double imag = Q->getAmplitude().imag();
			double p = norm(Q->getAmplitude());
			char qstring[1024] = "";

			printf("[%15ld] ", (uint64_t)Q->getIndex());
			printf("[P=%f] ", p);

			if(real >= 0 && imag >= 0) {
				printf("[+%.6f, +%.6f] ", abs(real), abs(imag));
			} else if(real >= 0 && imag < 0) {
				printf("[+%.6f, %.6f] ", abs(real), imag);
			} else if(real < 0 && imag >= 0) {
				printf("[%.6f, +%.6f] ", real, abs(imag));
			} else {
				printf("[%.6f, %.6f] ", real, imag);
			}

			to_binary(Q->getIndex(), numQubit, qstring);
			printf("|");
			for(int i=end; i>=begin; i--) { 
				if(qd != NULL) {
					for(int j=0; j<qd->size; j++) {
						if(i == qd->qubits[j]) {
							printf(" ");
							break;
						}
					}
				}
				printf("%c", qstring[numQubit-i-1]);
			}
			printf(">\n");
		} 
		printf("======== dump quantum states(%lu) ========\n\n", (uint64_t)totalStates);
	}

	void dump(int begin, int end) {
		dump(0, numQubit-1, NULL);
	}

	void dump(void) {
		dump(0, numQubit-1, NULL);
	}

	void dump(struct qubit_delimiter qd) {
		dump(0, numQubit-1, &qd);
	}

	/* 
	 * dump() prints out all 2^N states in the quantum register.
	 * However, the zero * amplitude state is not included.
	 * - added by sanglee
	 */
	void dump(int begin11, int end11, int begin21, int end21, int begin31, int end31, int begin12, int end12, int begin22, int end22, int begin32, int end32, int begin13, int end13, int begin23, int end23, int begin33, int end33)  
	{
		QState *Q;
		qsize_t totalStates = 0;
		for(int i=0; i<QSTORE_PARTITION; i++) {
			totalStates += qstore[i].size();
		}
	
		printf("======== dump quantum states(%lu) : 3 LQs ========\n", (uint64_t)totalStates);
#if 1
#if 0
		if (totalStates > LIMIT_STATES)
			return; 
#else
		qsize_t limitation = 1024;
		if (totalStates < limitation)
			limitation = totalStates;
#endif
		setOrderedQState();
		while((Q = getOrderedQState()) != NULL) {
			double real = Q->getAmplitude().real();
			double imag = Q->getAmplitude().imag();
			double p = norm(Q->getAmplitude());
			char qstring[1024] = "";

			// printf("[%5ld] ", (uint64_t)Q->getIndex());
#ifdef COEFF
			printf("[P=%f] ", p);
			if(real >= 0 && imag >= 0) {
				printf("[+%.6f, +%.6f] ", real, imag);
			} else if(real >= 0 && imag < 0) {
				printf("[+%.6f, %.6f] ", real, imag);
			} else if(real < 0 && imag >= 0) {
				printf("[%.6f, +%.6f] ", real, imag);
			} else {
				printf("[%.6f, %.6f] ", real, imag);
			}
			to_binary(Q->getIndex(), qubits, qstring);
			int ones = 0, ones2 = 0, ones3 = 0;
			printf("|");
			for(int i=end11; i>=begin11; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end21; i>=begin21; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end31; i>=begin31; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");

			for(int i=end12; i>=begin12; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones2++;
			}
			printf(" ");
			for(int i=end22; i>=begin22; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones3++;
			}
			printf(" ");
			for(int i=end32; i>=begin32; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones2++;
			}
			printf(" ");

			for(int i=end13; i>=begin13; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones3++;
			}
			printf(" ");
			for(int i=end23; i>=begin23; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones2++;
			}
			printf(" ");
			for(int i=end33; i>=begin33; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones3++;
			}
			if (ones%2 == 0)
				printf(" [E] ");
			else
				printf(" [O] ");

			if (ones2%2 == 0)
				printf(" [E] ");
			else
				printf(" [O] ");

			if (ones3%2 == 0)
				printf(" [E]");
			else
				printf(" [O]");

			printf(">\n");
#else
			to_binary(Q->getIndex(), numQubit, qstring);
			for(int i=end11; i>=begin11; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end21; i>=begin21; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end31; i>=begin31; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end12; i>=begin12; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end22; i>=begin22; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end32; i>=begin32; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end13; i>=begin13; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end23; i>=begin23; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end33; i>=begin33; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf("\n");
#endif
#if 0
			if (--limitation == 0)
				break;
#endif
		} 
		printf("\n");
#endif
	}

	/* 
	 * dump1() prints out all 2^N states in the quantum register.
	 * However, the zero * amplitude state is not included.
	 * - added by sanglee
	 */
	void dump1(int begin11, int end11, int begin21, int end21, int begin31, int end31, int begin12, int end12, int begin22, int end22, int begin32, int end32, int begin13, int end13, int begin23, int end23, int begin33, int end33)  
	{
		QState *Q;
		qsize_t totalStates = 0;
		for(int i=0; i<QSTORE_PARTITION; i++) {
			totalStates += qstore[i].size();
		}
	
		printf("======== dump quantum states(%lu) : 2 LQs ========\n", (uint64_t)totalStates);
#if 1
#if 0
		if (totalStates > LIMIT_STATES)
			return; 
#else
		qsize_t limitation = 1024;
		if (totalStates < limitation)
			limitation = totalStates;
#endif
		setOrderedQState();
		while((Q = getOrderedQState()) != NULL) {
			double real = Q->getAmplitude().real();
			double imag = Q->getAmplitude().imag();
			double p = norm(Q->getAmplitude());
			char qstring[1024] = "";

			// printf("[%5ld] ", (uint64_t)Q->getIndex());
#ifdef COEFF
			printf("[P=%f] ", p);
			if(real >= 0 && imag >= 0) {
				printf("[+%.6f, +%.6f] ", real, imag);
			} else if(real >= 0 && imag < 0) {
				printf("[+%.6f, %.6f] ", real, imag);
			} else if(real < 0 && imag >= 0) {
				printf("[%.6f, +%.6f] ", real, imag);
			} else {
				printf("[%.6f, %.6f] ", real, imag);
			}
			to_binary(Q->getIndex(), qubits, qstring);
			int ones = 0, ones2 = 0;
			printf("|");
			for(int i=end11; i>=begin11; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end21; i>=begin21; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end31; i>=begin31; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");

			for(int i=end12; i>=begin12; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end22; i>=begin22; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones2++;
			}
			printf(" ");
			for(int i=end32; i>=begin32; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");

			for(int i=end13; i>=begin13; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones2++;
			}
			printf(" ");
			for(int i=end23; i>=begin23; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end33; i>=begin33; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones2++;
			}
			if (ones%2 == 0)
				printf(" [E] ");
			else
				printf(" [O] ");

			if (ones2%2 == 0)
				printf(" [E] ");
			else
				printf(" [O] ");
			printf(">\n");
#else
			to_binary(Q->getIndex(), numQubit, qstring);
			for(int i=end11; i>=begin11; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end21; i>=begin21; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end31; i>=begin31; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end12; i>=begin12; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end22; i>=begin22; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end32; i>=begin32; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end13; i>=begin13; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end23; i>=begin23; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf(" ");
			for(int i=end33; i>=begin33; i--)
				printf("%c", qstring[numQubit-i-1]);
			printf("\n");
#endif
#if 0
			if (--limitation == 0)
				break;
#endif
		} 
		printf("\n");
#endif
	}


	/* 
	 * dump() prints out all 2^N states in the quantum register.
	 * However, the zero * amplitude state is not included.
	 * - added by sanglee
	 */
	void dump(int begin11, int end11, int begin21, int end21, int begin31, int end31, int begin12, int end12, int begin22, int end22, int begin32, int end32)  
	{
		QState *Q;
		qsize_t totalStates = 0;
		for(int i=0; i<QSTORE_PARTITION; i++) {
			totalStates += qstore[i].size();
		}
	
		printf("======== dump quantum states(%lu) : 2 LQs ========\n", (uint64_t)totalStates);
#if 1
#if 0
		if (totalStates > LIMIT_STATES)
			return; 
#else
		qsize_t limitation = 1024;
		if (totalStates < limitation)
			limitation = totalStates;
#endif
		setOrderedQState();
		while((Q = getOrderedQState()) != NULL) {
			double real = Q->getAmplitude().real();
			double imag = Q->getAmplitude().imag();
			double p = norm(Q->getAmplitude());
			char qstring[1024] = "";

			// printf("[%5ld] ", (uint64_t)Q->getIndex());
#ifdef COEFF
			printf("[P=%f] ", p);
			if(real >= 0 && imag >= 0) {
				printf("[+%.6f, +%.6f] ", real, imag);
			} else if(real >= 0 && imag < 0) {
				printf("[+%.6f, %.6f] ", real, imag);
			} else if(real < 0 && imag >= 0) {
				printf("[%.6f, +%.6f] ", real, imag);
			} else {
				printf("[%.6f, %.6f] ", real, imag);
			}
			to_binary(Q->getIndex(), qubits, qstring);
			int ones = 0, ones2 = 0;
			printf("|");
			for(int i=end11; i>=begin11; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end21; i>=begin21; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end31; i>=begin31; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			if (ones%2 == 0)
				printf(" [E] ");
			else
				printf(" [O] ");

			for(int i=end12; i>=begin12; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones2++;
			}
			printf(" ");
			for(int i=end22; i>=begin22; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones2++;
			}
			printf(" ");
			for(int i=end32; i>=begin32; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones2++;
			}
			if (ones2%2 == 0)
				printf(" [E]");
			else
				printf(" [O]");

			printf(">\n");
#else
			to_binary(Q->getIndex(), numQubit, qstring);
			int ones = 0;
			int ones2 = 0;
			for(int i=end11; i>=begin11; i--) {
				printf("%c", qstring[numQubit-i-1]);
				if (qstring[numQubit-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end21; i>=begin21; i--) {
				printf("%c", qstring[numQubit-i-1]);
				if (qstring[numQubit-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end31; i>=begin31; i--) {
				printf("%c", qstring[numQubit-i-1]);
				if (qstring[numQubit-i-1] == '1')
					ones++;
			}
			if (ones%2 == 0)
				printf(" [E]");
			else
				printf(" [O]");
			printf(" ");
			for(int i=end12; i>=begin12; i--) {
				printf("%c", qstring[numQubit-i-1]);
				if (qstring[numQubit-i-1] == '1')
					ones2++;
			}
			printf(" ");
			for(int i=end22; i>=begin22; i--) {
				printf("%c", qstring[numQubit-i-1]);
				if (qstring[numQubit-i-1] == '1')
					ones2++;
			}
			printf(" ");
			for(int i=end32; i>=begin32; i--) {
				printf("%c", qstring[numQubit-i-1]);
				if (qstring[numQubit-i-1] == '1')
					ones2++;
			}
			if (ones2%2 == 0)
				printf(" [E]");
			else
				printf(" [O]");
			printf("\n");
#endif
#if 0
			if (--limitation == 0)
				break;
#endif
		} 
		printf("\n");
#endif
	}

	/* 
	 * dump() prints out all 2^N states in the quantum register.
	 * However, the zero * amplitude state is not included.
	 * - added by sanglee
	 */
	void dump(int begin1, int end1, int begin2, int end2, int begin3, int end3) 
	{
		QState *Q;
		qsize_t totalStates = 0;
		for(int i=0; i<QSTORE_PARTITION; i++) {
			totalStates += qstore[i].size();
		}
	
		printf("======== dump quantum states(%lu) ========\n", (uint64_t)totalStates);
#if 1
#if 0
		if (totalStates > LIMIT_STATES)
			return; 
#else
		qsize_t limitation = 1024;
		if (totalStates < limitation)
			limitation = totalStates;
#endif
		setOrderedQState();
		while((Q = getOrderedQState()) != NULL) {
			double real = Q->getAmplitude().real();
			double imag = Q->getAmplitude().imag();
			double p = norm(Q->getAmplitude());
			char qstring[1024] = "";

			// printf("[%5ld] ", (uint64_t)Q->getIndex());
#ifdef COEFF
			printf("[P=%f] ", p);
			if(real >= 0 && imag >= 0) {
				printf("[+%.6f, +%.6f] ", real, imag);
			} else if(real >= 0 && imag < 0) {
				printf("[+%.6f, %.6f] ", real, imag);
			} else if(real < 0 && imag >= 0) {
				printf("[%.6f, +%.6f] ", real, imag);
			} else {
				printf("[%.6f, %.6f] ", real, imag);
			}
			to_binary(Q->getIndex(), qubits, qstring);
			int ones = 0;
			printf("|");
			for(int i=end1; i>=begin1; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end2; i>=begin2; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end3; i>=begin3; i--) {
				printf("%c", qstring[qubits-i-1]);
				if (qstring[qubits-i-1] == '1')
					ones++;
			}
			if (ones%2 == 0)
				printf(" [E]");
			else
				printf(" [O]");
			printf(">\n");
#else
			to_binary(Q->getIndex(), numQubit, qstring);
			int ones = 0;
			for(int i=end1; i>=begin1; i--) {
				printf("%c", qstring[numQubit-i-1]);
				if (qstring[numQubit-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end2; i>=begin2; i--) {
				printf("%c", qstring[numQubit-i-1]);
				if (qstring[numQubit-i-1] == '1')
					ones++;
			}
			printf(" ");
			for(int i=end3; i>=begin3; i--) {
				printf("%c", qstring[numQubit-i-1]);
				if (qstring[numQubit-i-1] == '1')
					ones++;
			}
			if (ones%2 == 0)
				printf(" [E]");
			else
				printf(" [O]");
			printf("\n");
#endif
#if 0
			if (--limitation == 0)
				break;
#endif
		} 
		printf("\n");
#endif
	}

	void dump(int q1, int q2, int q3, int q4, int q5, int q6, int q7, int q8, int q9) 
	{
		QState *Q;
		qsize_t totalStates = 0;
		for(int i=0; i<QSTORE_PARTITION; i++) {
			totalStates += qstore[i].size();
		}
	
		printf("======== dump quantum states(%lu) ========\n", (uint64_t)totalStates);
#if 1
#if 0
		if (totalStates > LIMIT_STATES)
			return; 
#else
		qsize_t limitation = 64;
		if (totalStates < limitation)
			limitation = totalStates;
#endif
		setOrderedQState();
		while((Q = getOrderedQState()) != NULL) {
			double real = Q->getAmplitude().real();
			double imag = Q->getAmplitude().imag();
			double p = norm(Q->getAmplitude());
			char qstring[1024] = "";

			// printf("[%5ld] ", (uint64_t)Q->getIndex());
#ifdef COEFF
			printf("[P=%f] ", p);

			if(real >= 0 && imag >= 0) {
				printf("[+%.6f, +%.6f] ", real, imag);
			} else if(real >= 0 && imag < 0) {
				printf("[+%.6f, %.6f] ", real, imag);
			} else if(real < 0 && imag >= 0) {
				printf("[%.6f, +%.6f] ", real, imag);
			} else {
				printf("[%.6f, %.6f] ", real, imag);
			}

			to_binary(Q->getIndex(), qubits, qstring);
			int ones = 0;
			printf("|");
			printf("%c", qstring[qubits-q1-1]);
			if (qstring[qubits-q1-1] == '1')
				ones++;
			printf("%c", qstring[qubits-q2-1]);
			if (qstring[qubits-q2-1] == '1')
				ones++;
			printf("%c", qstring[qubits-q3-1]);
			if (qstring[qubits-q3-1] == '1')
				ones++;
			printf(" %c", qstring[qubits-q4-1]);
			if (qstring[qubits-q4-1] == '1')
				ones++;
			printf("%c", qstring[qubits-q5-1]);
			if (qstring[qubits-q5-1] == '1')
				ones++;
			printf("%c", qstring[qubits-q6-1]);
			if (qstring[qubits-q6-1] == '1')
				ones++;
			printf(" %c", qstring[qubits-q7-1]);
			if (qstring[qubits-q7-1] == '1')
				ones++;
			printf("%c", qstring[qubits-q8-1]);
			if (qstring[qubits-q8-1] == '1')
				ones++;
			printf("%c", qstring[qubits-q9-1]);
			if (qstring[qubits-q9-1] == '1')
				ones++;

			if (ones%2 == 0)
				printf(" [E]");
			else
				printf(" [O]");
			printf(">\n");
#else
			to_binary(Q->getIndex(), numQubit, qstring);
			printf("%c", qstring[numQubit-q1-1]);
			printf("%c", qstring[numQubit-q2-1]);
			printf("%c", qstring[numQubit-q3-1]);
			printf(" %c", qstring[numQubit-q4-1]);
			printf("%c", qstring[numQubit-q5-1]);
			printf("%c", qstring[numQubit-q6-1]);
			printf(" %c", qstring[numQubit-q7-1]);
			printf("%c", qstring[numQubit-q8-1]);
			printf("%c", qstring[numQubit-q9-1]);
			printf("\n");
#endif
#if 0
			if (--limitation == 0)
				break;
#endif
		} 
		printf("\n");
#endif
	}

};

#endif
