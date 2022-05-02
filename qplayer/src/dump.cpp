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

void showQState(QRegister *QReg, char *msg, int begin, int end, struct qubit_delimiter *qd)
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

	#if 0
		printf("[%15ld] ", (uint64_t) Q->getIndex());
	#endif

	#if 1
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
		printf("======== [%s] dump quantum states(%lu) ========\n\n", msg, (uint64_t) totalStates);
	} else {
		printf("======== dump quantum states(%lu) ========\n\n", (uint64_t) totalStates);
	}
}

void showQState(QRegister *QReg, int begin, int end)
{
	showQState(QReg, NULL, 0, QReg->getNumQubits() - 1, NULL);
}

void showQState(QRegister *QReg)
{
	showQState(QReg, NULL, 0, QReg->getNumQubits() - 1, NULL);
}

void showQState(QRegister *QReg, char *msg, int begin, int end, struct qubit_delimiter qd)
{
	showQState(QReg, msg, begin, end, &qd);
}

void showQState(QRegister *QReg, char *msg, struct qubit_delimiter qd)
{
	showQState(QReg, msg, 0, QReg->getNumQubits() - 1, &qd);
}

void showQState(QRegister *QReg, struct qubit_delimiter qd)
{
	showQState(QReg, NULL, 0, QReg->getNumQubits() - 1, &qd);
}

/* 
 * showQState() prints out all 2^N states in the quantum register.
 * However, the zero * amplitude state is not included.
 * - added by sanglee
 */
void showQState(QRegister *QReg, int begin11, int end11, int begin21, int end21, int begin31, int end31, int begin12, int end12,
		  int begin22, int end22, int begin32, int end32, int begin13, int end13, int begin23, int end23,
		  int begin33, int end33)
{
	QState *Q;
	qsize_t totalStates = QReg->getNumStates();
	int numQubits = QReg->getNumQubits();

	for (int i = 0; i < QSTORE_PARTITION; i++) {
		totalStates += QReg->qstore[i].size();
	}

	printf("======== showQState quantum states(%lu) : 3 LQs ========\n", (uint64_t) totalStates);
#if 1
#if 0
	if (totalStates > LIMIT_STATES)
		return;
#else
	qsize_t limitation = 1024;
	if (totalStates < limitation)
		limitation = totalStates;
#endif
	QReg->setOrderedQState();
	while ((Q = QReg->getOrderedQState()) != NULL) {
		double real = Q->getAmplitude().real();
		double imag = Q->getAmplitude().imag();
		double p = norm(Q->getAmplitude());
		char qstring[1024] = "";

		// printf("[%5ld] ", (uint64_t)Q->getIndex());
#ifdef COEFF
		printf("[P=%f] ", p);
		if (real >= 0 && imag >= 0) {
			printf("[+%.6f, +%.6f] ", real, imag);
		} else if (real >= 0 && imag < 0) {
			printf("[+%.6f, %.6f] ", real, imag);
		} else if (real < 0 && imag >= 0) {
			printf("[%.6f, +%.6f] ", real, imag);
		} else {
			printf("[%.6f, %.6f] ", real, imag);
		}
		to_binary(Q->getIndex(), qubits, qstring);
		int ones = 0, ones2 = 0, ones3 = 0;
		printf("|");
		for (int i = end11; i >= begin11; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end21; i >= begin21; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end31; i >= begin31; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");

		for (int i = end12; i >= begin12; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones2++;
		}
		printf(" ");
		for (int i = end22; i >= begin22; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones3++;
		}
		printf(" ");
		for (int i = end32; i >= begin32; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones2++;
		}
		printf(" ");

		for (int i = end13; i >= begin13; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones3++;
		}
		printf(" ");
		for (int i = end23; i >= begin23; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones2++;
		}
		printf(" ");
		for (int i = end33; i >= begin33; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones3++;
		}
		if (ones % 2 == 0)
			printf(" [E] ");
		else
			printf(" [O] ");

		if (ones2 % 2 == 0)
			printf(" [E] ");
		else
			printf(" [O] ");

		if (ones3 % 2 == 0)
			printf(" [E]");
		else
			printf(" [O]");

		printf(">\n");
#else
		to_binary(Q->getIndex(), numQubits, qstring);
		for (int i = end11; i >= begin11; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end21; i >= begin21; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end31; i >= begin31; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end12; i >= begin12; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end22; i >= begin22; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end32; i >= begin32; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end13; i >= begin13; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end23; i >= begin23; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end33; i >= begin33; i--)
			printf("%c", qstring[numQubits - i - 1]);
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
 * showQState1() prints out all 2^N states in the quantum register.
 * However, the zero * amplitude state is not included.
 * - added by sanglee
 */
void showQState1(QRegister *QReg, int begin11, int end11, int begin21, int end21, int begin31, int end31, int begin12, int end12,
		   int begin22, int end22, int begin32, int end32, int begin13, int end13, int begin23, int end23,
		   int begin33, int end33)
{
	QState *Q;
	qsize_t totalStates = QReg->getNumStates();
	int numQubits = QReg->getNumQubits();

	for (int i = 0; i < QSTORE_PARTITION; i++) {
		totalStates += QReg->qstore[i].size();
	}

	printf("======== dump quantum states(%lu) : 2 LQs ========\n", (uint64_t) totalStates);
#if 1
#if 0
	if (totalStates > LIMIT_STATES)
		return;
#else
	qsize_t limitation = 1024;
	if (totalStates < limitation)
		limitation = totalStates;
#endif
	QReg->setOrderedQState();
	while ((Q = QReg->getOrderedQState()) != NULL) {
		double real = Q->getAmplitude().real();
		double imag = Q->getAmplitude().imag();
		double p = norm(Q->getAmplitude());
		char qstring[1024] = "";

		// printf("[%5ld] ", (uint64_t)Q->getIndex());
#ifdef COEFF
		printf("[P=%f] ", p);
		if (real >= 0 && imag >= 0) {
			printf("[+%.6f, +%.6f] ", real, imag);
		} else if (real >= 0 && imag < 0) {
			printf("[+%.6f, %.6f] ", real, imag);
		} else if (real < 0 && imag >= 0) {
			printf("[%.6f, +%.6f] ", real, imag);
		} else {
			printf("[%.6f, %.6f] ", real, imag);
		}
		to_binary(Q->getIndex(), qubits, qstring);
		int ones = 0, ones2 = 0;
		printf("|");
		for (int i = end11; i >= begin11; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end21; i >= begin21; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end31; i >= begin31; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");

		for (int i = end12; i >= begin12; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end22; i >= begin22; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones2++;
		}
		printf(" ");
		for (int i = end32; i >= begin32; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");

		for (int i = end13; i >= begin13; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones2++;
		}
		printf(" ");
		for (int i = end23; i >= begin23; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end33; i >= begin33; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones2++;
		}
		if (ones % 2 == 0)
			printf(" [E] ");
		else
			printf(" [O] ");

		if (ones2 % 2 == 0)
			printf(" [E] ");
		else
			printf(" [O] ");
		printf(">\n");
#else
		to_binary(Q->getIndex(), numQubits, qstring);
		for (int i = end11; i >= begin11; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end21; i >= begin21; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end31; i >= begin31; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end12; i >= begin12; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end22; i >= begin22; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end32; i >= begin32; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end13; i >= begin13; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end23; i >= begin23; i--)
			printf("%c", qstring[numQubits - i - 1]);
		printf(" ");
		for (int i = end33; i >= begin33; i--)
			printf("%c", qstring[numQubits - i - 1]);
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
	 * showQState() prints out all 2^N states in the quantum register.
	 * However, the zero * amplitude state is not included.
	 * - added by sanglee
	 */
void showQState(QRegister *QReg, int begin11, int end11, int begin21, int end21, int begin31, int end31, int begin12, int end12,
		  int begin22, int end22, int begin32, int end32)
{
	QState *Q;
	qsize_t totalStates = QReg->getNumStates();
	int numQubits = QReg->getNumQubits();

	for (int i = 0; i < QSTORE_PARTITION; i++) {
		totalStates += QReg->qstore[i].size();
	}

	printf("======== dump quantum states(%lu) : 2 LQs ========\n", (uint64_t) totalStates);
#if 1
#if 0
	if (totalStates > LIMIT_STATES)
		return;
#else
	qsize_t limitation = 1024;
	if (totalStates < limitation)
		limitation = totalStates;
#endif
	QReg->setOrderedQState();
	while ((Q = QReg->getOrderedQState()) != NULL) {
		double real = Q->getAmplitude().real();
		double imag = Q->getAmplitude().imag();
		double p = norm(Q->getAmplitude());
		char qstring[1024] = "";

		// printf("[%5ld] ", (uint64_t)Q->getIndex());
#ifdef COEFF
		printf("[P=%f] ", p);
		if (real >= 0 && imag >= 0) {
			printf("[+%.6f, +%.6f] ", real, imag);
		} else if (real >= 0 && imag < 0) {
			printf("[+%.6f, %.6f] ", real, imag);
		} else if (real < 0 && imag >= 0) {
			printf("[%.6f, +%.6f] ", real, imag);
		} else {
			printf("[%.6f, %.6f] ", real, imag);
		}
		to_binary(Q->getIndex(), qubits, qstring);
		int ones = 0, ones2 = 0;
		printf("|");
		for (int i = end11; i >= begin11; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end21; i >= begin21; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end31; i >= begin31; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		if (ones % 2 == 0)
			printf(" [E] ");
		else
			printf(" [O] ");

		for (int i = end12; i >= begin12; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones2++;
		}
		printf(" ");
		for (int i = end22; i >= begin22; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones2++;
		}
		printf(" ");
		for (int i = end32; i >= begin32; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones2++;
		}
		if (ones2 % 2 == 0)
			printf(" [E]");
		else
			printf(" [O]");

		printf(">\n");
#else
		to_binary(Q->getIndex(), numQubits, qstring);
		int ones = 0;
		int ones2 = 0;
		for (int i = end11; i >= begin11; i--) {
			printf("%c", qstring[numQubits - i - 1]);
			if (qstring[numQubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end21; i >= begin21; i--) {
			printf("%c", qstring[numQubits - i - 1]);
			if (qstring[numQubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end31; i >= begin31; i--) {
			printf("%c", qstring[numQubits - i - 1]);
			if (qstring[numQubits - i - 1] == '1')
				ones++;
		}
		if (ones % 2 == 0)
			printf(" [E]");
		else
			printf(" [O]");
		printf(" ");
		for (int i = end12; i >= begin12; i--) {
			printf("%c", qstring[numQubits - i - 1]);
			if (qstring[numQubits - i - 1] == '1')
				ones2++;
		}
		printf(" ");
		for (int i = end22; i >= begin22; i--) {
			printf("%c", qstring[numQubits - i - 1]);
			if (qstring[numQubits - i - 1] == '1')
				ones2++;
		}
		printf(" ");
		for (int i = end32; i >= begin32; i--) {
			printf("%c", qstring[numQubits - i - 1]);
			if (qstring[numQubits - i - 1] == '1')
				ones2++;
		}
		if (ones2 % 2 == 0)
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
	 * showQState() prints out all 2^N states in the quantum register.
	 * However, the zero * amplitude state is not included.
	 * - added by sanglee
	 */
void showQState(QRegister *QReg, int begin1, int end1, int begin2, int end2, int begin3, int end3)
{
	QState *Q;
	qsize_t totalStates = QReg->getNumStates();
	int numQubits = QReg->getNumQubits();

	for (int i = 0; i < QSTORE_PARTITION; i++) {
		totalStates += QReg->qstore[i].size();
	}

	printf("======== dump quantum states(%lu) ========\n", (uint64_t) totalStates);
#if 1
#if 0
	if (totalStates > LIMIT_STATES)
		return;
#else
	qsize_t limitation = 1024;
	if (totalStates < limitation)
		limitation = totalStates;
#endif
	QReg->setOrderedQState();
	while ((Q = QReg->getOrderedQState()) != NULL) {
		double real = Q->getAmplitude().real();
		double imag = Q->getAmplitude().imag();
		double p = norm(Q->getAmplitude());
		char qstring[1024] = "";

		// printf("[%5ld] ", (uint64_t)Q->getIndex());
#ifdef COEFF
		printf("[P=%f] ", p);
		if (real >= 0 && imag >= 0) {
			printf("[+%.6f, +%.6f] ", real, imag);
		} else if (real >= 0 && imag < 0) {
			printf("[+%.6f, %.6f] ", real, imag);
		} else if (real < 0 && imag >= 0) {
			printf("[%.6f, +%.6f] ", real, imag);
		} else {
			printf("[%.6f, %.6f] ", real, imag);
		}
		to_binary(Q->getIndex(), qubits, qstring);
		int ones = 0;
		printf("|");
		for (int i = end1; i >= begin1; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end2; i >= begin2; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end3; i >= begin3; i--) {
			printf("%c", qstring[qubits - i - 1]);
			if (qstring[qubits - i - 1] == '1')
				ones++;
		}
		if (ones % 2 == 0)
			printf(" [E]");
		else
			printf(" [O]");
		printf(">\n");
#else
		to_binary(Q->getIndex(), numQubits, qstring);
		int ones = 0;
		for (int i = end1; i >= begin1; i--) {
			printf("%c", qstring[numQubits - i - 1]);
			if (qstring[numQubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end2; i >= begin2; i--) {
			printf("%c", qstring[numQubits - i - 1]);
			if (qstring[numQubits - i - 1] == '1')
				ones++;
		}
		printf(" ");
		for (int i = end3; i >= begin3; i--) {
			printf("%c", qstring[numQubits - i - 1]);
			if (qstring[numQubits - i - 1] == '1')
				ones++;
		}
		if (ones % 2 == 0)
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

void showQState(QRegister *QReg, int q1, int q2, int q3, int q4, int q5, int q6, int q7, int q8, int q9)
{
	QState *Q;
	qsize_t totalStates = QReg->getNumStates();
	int numQubits = QReg->getNumQubits();

	for (int i = 0; i < QSTORE_PARTITION; i++) {
		totalStates += QReg->qstore[i].size();
	}

	printf("======== dump quantum states(%lu) ========\n", (uint64_t) totalStates);
#if 1
#if 0
	if (totalStates > LIMIT_STATES)
		return;
#else
	qsize_t limitation = 64;
	if (totalStates < limitation)
		limitation = totalStates;
#endif
	QReg->setOrderedQState();
	while ((Q = QReg->getOrderedQState()) != NULL) {
		double real = Q->getAmplitude().real();
		double imag = Q->getAmplitude().imag();
		double p = norm(Q->getAmplitude());
		char qstring[1024] = "";

		// printf("[%5ld] ", (uint64_t)Q->getIndex());
#ifdef COEFF
		printf("[P=%f] ", p);

		if (real >= 0 && imag >= 0) {
			printf("[+%.6f, +%.6f] ", real, imag);
		} else if (real >= 0 && imag < 0) {
			printf("[+%.6f, %.6f] ", real, imag);
		} else if (real < 0 && imag >= 0) {
			printf("[%.6f, +%.6f] ", real, imag);
		} else {
			printf("[%.6f, %.6f] ", real, imag);
		}

		to_binary(Q->getIndex(), qubits, qstring);
		int ones = 0;
		printf("|");
		printf("%c", qstring[qubits - q1 - 1]);
		if (qstring[qubits - q1 - 1] == '1')
			ones++;
		printf("%c", qstring[qubits - q2 - 1]);
		if (qstring[qubits - q2 - 1] == '1')
			ones++;
		printf("%c", qstring[qubits - q3 - 1]);
		if (qstring[qubits - q3 - 1] == '1')
			ones++;
		printf(" %c", qstring[qubits - q4 - 1]);
		if (qstring[qubits - q4 - 1] == '1')
			ones++;
		printf("%c", qstring[qubits - q5 - 1]);
		if (qstring[qubits - q5 - 1] == '1')
			ones++;
		printf("%c", qstring[qubits - q6 - 1]);
		if (qstring[qubits - q6 - 1] == '1')
			ones++;
		printf(" %c", qstring[qubits - q7 - 1]);
		if (qstring[qubits - q7 - 1] == '1')
			ones++;
		printf("%c", qstring[qubits - q8 - 1]);
		if (qstring[qubits - q8 - 1] == '1')
			ones++;
		printf("%c", qstring[qubits - q9 - 1]);
		if (qstring[qubits - q9 - 1] == '1')
			ones++;

		if (ones % 2 == 0)
			printf(" [E]");
		else
			printf(" [O]");
		printf(">\n");
#else
		to_binary(Q->getIndex(), numQubits, qstring);
		printf("%c", qstring[numQubits - q1 - 1]);
		printf("%c", qstring[numQubits - q2 - 1]);
		printf("%c", qstring[numQubits - q3 - 1]);
		printf(" %c", qstring[numQubits - q4 - 1]);
		printf("%c", qstring[numQubits - q5 - 1]);
		printf("%c", qstring[numQubits - q6 - 1]);
		printf(" %c", qstring[numQubits - q7 - 1]);
		printf("%c", qstring[numQubits - q8 - 1]);
		printf("%c", qstring[numQubits - q9 - 1]);
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
