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
	if(n > MAX_QUBITS) {
		printf("the number of allowed qubit is %d\n", MAX_QUBITS);
		exit(0);
	}

	numQubit = n;
	curStage = 0;
	maxStates = quantum_shiftL(1, numQubit);
	init();

	/* set random seed */
	srand(time(NULL));
}

QRegister::QRegister(QRegister *src) {
	numQubit = src->getNumQubits();
	curStage = 0;
	maxStates = quantum_shiftL(1, numQubit);
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
