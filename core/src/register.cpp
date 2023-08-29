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

	for(int i=0; i<MAX_QUBITS; i++) {
		qubitTypes[i] = KET_ZERO;
	}

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
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(auto entry : qstore[i]) {
			delete entry.second;
		}
		qstore[i].erase(qstore[i].begin(), qstore[i].end());
	}
}
