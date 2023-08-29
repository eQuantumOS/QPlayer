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
	int cpuCores;
	qsize_t maxStates;
	struct qregister_stat qstat;

public:
	std::map<qsize_t, QState*> qstore[QSTORE_PARTITION];
	std::map<qsize_t, QState*>::iterator qiter[QSTORE_PARTITION];
	std::mutex qlock[QSTORE_PARTITION];
	std::mutex slock;

public:
	std::vector<QState*> addQList[QSTORE_PARTITION][MAX_CORES];
	std::vector<QState*> delQList[QSTORE_PARTITION][MAX_CORES];
	int qubitTypes[MAX_QUBITS];

public:
	QRegister(int n);
	QRegister(QRegister *src);
	~QRegister(void);

	/* clear all quantum states */
	void clear(void) {
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
			setQState(newQ->getIndex(), newQ);
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

	/* qstore lock & unlock */
	void QLock(int index) { qlock[index].lock(); }
	void QUnlock(int index) { qlock[index].unlock(); } 
	void QLock(qsize_t index) { qlock[getPartId(index)].lock(); }
	void QUnlock(qsize_t index) { qlock[getPartId(index)].unlock(); } 

	void checkMemory(void) {
		static uint64_t memTotal = 0;
		static uint64_t memAvail = 0;
		uint64_t memUsed = 0;

		if(memTotal == 0) {
			getTotalMem(&memTotal, &memAvail);
		}

		memUsed = getUsedMem();

		if((memUsed * 2) > memAvail) {
			char memTotalStr[32] = "";
			char memAvailStr[32] = "";
			char memUsedStr[32] = "";

			human_readable_size(memTotal, memTotalStr);
			human_readable_size(memAvail, memAvailStr);
			human_readable_size(memUsed, memAvailStr);

			printf("Memory space is insufficient!!\n");
			printf("Your quantum circuit may generate too many quantum states.\n");
			printf(" - Memory: Total=%s, Avail=%s --> Used=%s\n", memTotalStr, memAvailStr, memUsedStr);
			printf(" - Quantum states: %lu\n", (uint64_t)getNumStates());
			exit(0);
		}
	}

public:
	/* add new state to the quantum register */
	void __setQState(qsize_t index, QState *state, bool lck) {
		std::map<qsize_t, QState*> *part = &qstore[getPartId(index)];

		if(lck == true) QLock(index);

		(*part)[index] = state;

		if(lck == true) QUnlock(index);
	}

	/*
	 * Removes the state from the quantum register.
	 * This function is called in two cases. 
	 *
	 * (1) Remove one of the superposed states after the measurement
	 * (2) Remove zero state after Gate operations
	 */
	void __eraseQState(qsize_t index, bool lck) {
		std::map<qsize_t, QState*>::iterator it;
		std::map<qsize_t, QState*> *part = &qstore[getPartId(index)];

		if(lck == true) QLock(index);

		it = part->find(index);
		if(it != part->end()) {
			putQState(it->second);
			part->erase(it);
		}

		if(lck == true) QUnlock(index);
	}

	/* search the quantum state corresponding to state index */
	QState *__findQState(qsize_t index, bool lck) {
		std::map<qsize_t, QState*>::iterator it;
		std::map<qsize_t, QState*> *part = &qstore[getPartId(index)];
		QState *Q = NULL;

		if(lck == true) QLock(index);

		it = part->find(index);
		if(it != part->end()) {
			Q = it->second;
		}

		if(lck == true) QUnlock(index);

		return Q;
	}

	void setQState(qsize_t index, QState *state) {
		return __setQState(index, state, true);
	}

	void setQState_nolock(qsize_t index, QState *state) {
		return __setQState(index, state, false);
	}

	void eraseQState(qsize_t index) {
		return __eraseQState(index, true);
	}

	void eraseQState_nolock(qsize_t index) {
		return __eraseQState(index, false);
	}

	QState *findQState(qsize_t index) {
		QState *Q = __findQState(index, true);
		return Q;
	}
	
	QState *findQState_nolock(qsize_t index) {
		QState *Q = __findQState(index, false);
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
					putQState(Q);
					qstore[i].erase(it++);
				} else {
					++it;
				}
			}
		}
	}

public:
	void updateQRegStat(int gate, QTimer timer) {
		double tm = timer.getElapsedUSec();

		/* update max number of quantum states */
		qsize_t numStates = getNumStates();
		if(qstat.maxQStates < numStates) {
			qstat.maxQStates = numStates;
		}

		/* increase total gate calls */
		qstat.totalGateCalls++;

		/* increase each gate calls */ 
		qstat.gateCalls[gate]++;

		/* update execution time stats */ 
		if(qstat.tm_total == 0) {
			qstat.tm_total = tm;
		} else {
			qstat.tm_total += tm;
		}

		/* update execution time stats for each gate */ 
		if(qstat.tm_gates_total[gate] == 0) {
			qstat.tm_gates_total[gate] = tm;
			qstat.tm_gates_max[gate] = tm;
			qstat.tm_gates_min[gate] = tm;
			qstat.tm_gates_avg[gate] = tm;
		} else {
			qstat.tm_gates_total[gate] += tm;

			if(qstat.tm_gates_max[gate] < tm) {
				qstat.tm_gates_max[gate] = tm;
			}

			if(qstat.tm_gates_min[gate] > tm) {
				qstat.tm_gates_min[gate] = tm;
			}

			qstat.tm_gates_avg[gate] = qstat.tm_gates_total[gate] / (double)qstat.gateCalls[gate];
		}
	}

	struct qregister_stat getQRegStat(void) {
		qstat.finalQStates = getNumStates();

		getTotalMem(&qstat.memTotal, &qstat.memAvail);
		qstat.memUsed = getUsedMem();

		getOS(qstat.os_name, qstat.os_version);
		getCPU(qstat.cpu_model, &qstat.cpu_cores, qstat.cpu_herz);

		qstat.usedGates = 0;
		for(int i=0; i<MAX_GATES; i++) {
			if(qstat.gateCalls[i] != 0) {
				qstat.usedGates++;
			}
		}

		return qstat;
	}

	void showQRegStat(void) {
		struct qregister_stat stat = getQRegStat();

		char memTotalStr[32] = "";
		char memAvailStr[32] = "";
		char memUsedStr[32] = "";
		char totalTimeStr[32] = "";

		human_readable_size(stat.memTotal, memTotalStr);
		human_readable_size(stat.memAvail, memAvailStr);
		human_readable_size(stat.memUsed, memUsedStr);
		usec2str(stat.tm_total, totalTimeStr);

		printf("\n");
		printf("\033[1;34m1. Circuit Information\033[0;39m\n");
		printf("+--------------------------+-----------------+\n");
		printf("| 1. used qubits           |  %7d        |\n", stat.qubits);
		printf("+--------------------------+-----------------+\n");
		printf("| 2. used gates            |  %7d        |\n", stat.usedGates);
		printf("+--------------------------+-----------------+\n");
		printf("| 3. total gate calls      |  %7d        |\n", stat.totalGateCalls);
		printf("+--------------------------+-----------------+\n");
		printf("| 4. individual gate calls |                 |\n");
		printf("+--------------------------+                 |\n");
		for(int i=0; i<MAX_GATES; i++) {
			if(stat.gateCalls[i] != 0) {
				printf("|              %10s  |  %7d (%2d %%) |\n", 
						gateString(i), stat.gateCalls[i], 
						(stat.gateCalls[i] * 100) / stat.totalGateCalls);
			}
		}
		printf("+--------------------------+-----------------+\n");

		printf("\n");
		printf("\033[1;34m2. Runtime (micro seconds)\033[0;39m\n");
		printf("+-----------------------------+---------------------------------------------+\n");
		printf("| 1. total simulation time    |   %11.f %29s |\n", stat.tm_total, totalTimeStr);
		printf("+-----------------------------+---------------+---------+---------+---------+\n");
		printf("| 2. each gate execution time |     total     |   max   |   min   |   avg   |\n");
		printf("+-----------------------------+---------------+---------+---------+---------+\n");
		for(int i=0; i<MAX_GATES; i++) {
			if(stat.gateCalls[i] != 0) {
				printf("| %27s | %13.0f | %7.0f | %7.0f | %7.0f |\n", gateString(i), 
					stat.tm_gates_total[i],
					stat.tm_gates_max[i],
					stat.tm_gates_min[i],
					stat.tm_gates_avg[i]);
			}
		}
		printf("+-----------------------------+---------------+---------+---------+---------+\n");

		printf("\n");
		printf("\033[1;34m3. Simulation Jobs\033[0;39m\n");
		printf("+-----------------------------------+---------------+\n");
		printf("| 1. max number of quantum states   | %13ld |\n", (uint64_t)stat.maxQStates);
		printf("+-----------------------------------+---------------+\n");
		printf("| 2. final number of quantum states | %13ld |\n", (uint64_t)stat.finalQStates);
		printf("+-----------------------------------+---------------+\n");
		printf("| 3. used memory                    | %13s |\n", memUsedStr);
		printf("+-----------------------------------+---------------+\n");

		printf("\n");
		printf("\033[1;34m4. System Information\033[0;39m\n");
		printf("+-----------+---------+-------------------------------------------------+\n");
		printf("|           | name    | %47s | \n", stat.os_name);
		printf("|    OS     |---------+-------------------------------------------------+\n");
		printf("|           | version | %47s | \n", stat.os_version);
		printf("+-----------+---------+-------------------------------------------------+\n");
		printf("|           | model   | %47s | \n", stat.cpu_model);
		printf("|           |---------+-------------------------------------------------+\n");
		printf("|    CPU    | cores   | %47d | \n", stat.cpu_cores);
		printf("|           |---------+-------------------------------------------------+\n");
		printf("|           | herz    | %47s | \n", stat.cpu_herz);
		printf("+-----------+---------+-------------------------------------------------+\n");
		printf("|           | total   | %47s | \n", memTotalStr);
		printf("|    MEM    |---------+-------------------------------------------------+\n");
		printf("|           | avail   | %47s | \n", memAvailStr);
		printf("+-----------+---------+-------------------------------------------------+\n");
	}
};

#endif
