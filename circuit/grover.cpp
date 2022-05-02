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
 * @file    grover.cpp
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @date    10-09-21
 * @brief       
 */

#include <stdio.h>
#include <getopt.h>

#include "qplayer.h"

using namespace std;

#define MAX_ROUND 15

void grover(QRegister *QReg, int qubits) {
	int mid = qubits / 2;

	/* 
	 * Initialize 
	 */
	X(QReg, mid);
	for(int i=0; i<mid; i++) {
		H(QReg, i);
	}
	H(QReg, mid);

	/* 
	 * Repeat 
	 */
	for(int repeat=0; repeat<qubits*MAX_ROUND; repeat++) {
		/*
		 * Oracle
		 */
		for(int i=0; i<mid; i++) {
			X(QReg, i);
		}

		CCX(QReg, 0, 1, mid+1);
		for(int i=1; i<mid; i++) {
			if((mid+i+1) >= qubits) {
				break;
			}
			CCX(QReg, i, mid+i, mid+i+1);
		}

		CX(QReg, qubits-1, mid);

		for(int i=0; i<mid-1; i++) {
			CCX(QReg, mid-i-1, qubits-2-i, qubits-1-i);
		}
		CCX(QReg, 0, 1, mid+1);

		for(int i=0; i<mid; i++) {
			X(QReg, i);
		}

		for(int i=0; i<mid; i++) {
			H(QReg, i);
		}

		/*
		 * Inversion
		 */
		for(int i=0; i<mid; i++) {
			X(QReg, i);
		}

		H(QReg, mid-1);

		CCX(QReg, 0, 1, mid+1);
		for(int i=1; i<mid-1; i++) {
			CCX(QReg, i, mid+i, mid+i+1);
		}

		CX(QReg, qubits-2, mid-1);

		for(int i=1; i<mid-1; i++) {
			CCX(QReg, mid-1-i, qubits-2-i, qubits-1-i);
		}
		CCX(QReg, 0, 1, mid+1);

		H(QReg, mid-1);

		for(int i=0; i<mid; i++) {
			X(QReg, i);
		}

		for(int i=0; i<mid; i++) {
			H(QReg, i);
		}
	}

	/*
	 * Measurement
 	 */
	H(QReg, mid);
	M(QReg, mid);
	showQState(QReg);

	for(int i=0; i<qubits; i++) {
		M(QReg, i);
	}
	showQState(QReg);
	printf("States: %ld\n", (size_t)QReg->getNumStates());
}

int main(int argc, char **argv)
{
	int qubits = 7;
	int c;

	while ((c = getopt_long(argc, argv, "q:", NULL, NULL)) != -1) {
		switch(c) {
		case 'q':
			qubits = atoi(optarg);
		default:
			break;
		}
	}

	QRegister *QReg = new QRegister(qubits);

	grover(QReg, qubits);
}

