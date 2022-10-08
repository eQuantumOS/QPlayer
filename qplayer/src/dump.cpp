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
 * @file    dump.cpp
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @date    10-09-21
 * @brief       
 */

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
#include "register.h"
#include "state.h"
#include "misc.h"

// #define FLAG_QINDEX
#define FLAG_QPROB

void dump(QRegister *QReg, char *msg, int begin, int end, struct qubit_delimiter *qd)
{
	QState *Q;
	qsize_t totalStates = QReg->getNumStates();
	int numQubits = QReg->getNumQubits();

	QReg->setOrderedQState();
	while ((Q = QReg->getOrderedQState()) != NULL) {
		double real = Q->getAmplitude().real();
		double imag = Q->getAmplitude().imag();
		double p = norm(Q->getAmplitude());
		char qstring[1024] = "";

	#ifdef FLAG_QINDEX
		printf("[%15ld] ", (uint64_t) Q->getIndex());
	#endif

	#ifdef FLAG_QPROB
		printf("[P=%f] ", p);
		if (real >= 0 && imag >= 0) {
			printf("[+%.6f, +%.6f] ", abs(real), abs(imag));
		} else if (real >= 0 && imag < 0) {
			printf("[+%.6f, %.6f] ", abs(real), imag);
		} else if (real < 0 && imag >= 0) {
			printf("[%.6f, +%.6f] ", real, abs(imag));
		} else {
			printf("[%.6f, %.6f] ", real, imag);
		}
	#else
		if (real >= 0 && imag >= 0) {
			printf("+");
		} else if (real >= 0 && imag < 0) {
			printf("+");
		} else if (real < 0 && imag >= 0) {
			printf("-");
		} else {
			printf("-");
		}
	#endif

		printf("|");
		to_binary(Q->getIndex(), numQubits, qstring);
		for (int i = end; i >= begin; i--) {
			if (qd != NULL) {
				for (int j = 0; j < qd->size; j++) {
					if (i == qd->qubits[j]) {
						printf(" ");
						break;
					}
				}
			}
			printf("%c", qstring[numQubits - i - 1]);
		}
		printf(">\n");
	}

	if (msg != NULL) {
		printf("======== [%s] dump quantum states(%lu) ========\n", msg, (uint64_t) totalStates);
	} else {
		printf("======== dump quantum states(%lu) ========\n", (uint64_t) totalStates);
	}
}

void dump(QRegister *QReg, char *msg, int begin, int end, struct qubit_delimiter qd)
{
	dump(QReg, msg, begin, end, &qd);
}

void dump(QRegister *QReg, char *msg, struct qubit_delimiter *qd)
{
	dump(QReg, msg, 0, QReg->getNumQubits() - 1, qd);
}

void dump(QRegister *QReg, char *msg, struct qubit_delimiter qd)
{
	dump(QReg, msg, 0, QReg->getNumQubits() - 1, &qd);
}

void dump(QRegister *QReg, struct qubit_delimiter *qd)
{
	dump(QReg, NULL, 0, QReg->getNumQubits() - 1, qd);
}

void dump(QRegister *QReg, struct qubit_delimiter qd)
{
	dump(QReg, NULL, 0, QReg->getNumQubits() - 1, &qd);
}

void dump(QRegister *QReg, int begin, int end)
{
	dump(QReg, NULL, 0, QReg->getNumQubits() - 1, NULL);
}

void dump(QRegister *QReg, char *msg)
{
	dump(QReg, msg, 0, QReg->getNumQubits() - 1, NULL);
}

void dump(QRegister *QReg)
{
	dump(QReg, NULL, 0, QReg->getNumQubits() - 1, NULL);
}

void dumpln(QRegister *QReg)
{
	dump(QReg, NULL, 0, QReg->getNumQubits() - 1, NULL);
	printf("\n");
}
