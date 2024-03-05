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
 * @file    qregister.cpp
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @date    10-09-21
 * @brief       
 */

#include "register.h"

using namespace std;

/* 
 * QRegister constructor 
 */
QRegister::QRegister(int n) {
	char cpu[64] = "";
	char herz[64] = "";

	if(n > MAX_QUBITS) {
		printf("the number of allowed qubit is %d\n", MAX_QUBITS);
		exit(0);
	}

	memset(&qstat, 0, sizeof(struct qregister_stat));

	numQubit = n;
	maxStates = quantum_shiftL(1, numQubit);
	qstat.qubits = numQubit;
	getCPU(cpu, &cpuCores, herz);

	init();

	/* set random seed */
	srand(time(NULL));
}

QRegister::QRegister(QRegister *src) {
	memset(&qstat, 0, sizeof(struct qregister_stat));

	for(int i=0; i<MAX_QUBITS; i++) {
		qubitTypes[i] = src->qubitTypes[i];
	}

	numQubit = src->getNumQubits();
	maxStates = quantum_shiftL(1, numQubit);
	qstat.qubits = numQubit;
	cpuCores = src->getCPUCores();
	replace(src);
}

/* 
 * QRegister constructor 
 */
QRegister::~QRegister(void) {
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(auto entry : qstore[i]) {
			delete entry.second;
		}
		qstore[i].erase(qstore[i].begin(), qstore[i].end());
	}
}

void QRegister::checkMemory(void) 
{
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

void QRegister::updateQRegStat(int gate, QTimer timer) {
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

struct qregister_stat QRegister::getQRegStat(void) {
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

void QRegister::showQRegStat(void) {
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
