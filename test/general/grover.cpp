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

void grover(QRegister *QReg, int qubits) {
	int target = qubits / 2;
	int repeat = (int)(sqrt(pow(4, qubits)));

	/* prepare grover algorithm */
	X(QReg, target);
	for(int i=0; i<target; i++) 
		H(QReg, i);
	H(QReg, target);

	/* run iteration */
	for(int i=0; i<repeat; i++) {
		/********************************/
		/* STEP1 : oracle               */ 
		/********************************/
		for(int j=0; j<target; j++) {
			X(QReg, j);
		}

		CCX(QReg, 0, 1, target+1);
		for(int j=1; j<target; j++) {
			if((target+j+1) >= qubits) {
				break;
			}
			CCX(QReg, j, target+j, target+j+1);
		}

		CX(QReg, qubits-1, target);

		for(int j=0; j<target-1; j++) {
			CCX(QReg, target-j-1, qubits-2-j, qubits-1-j);
		}
		CCX(QReg, 0, 1, target+1);

		for(int j=0; j<target; j++) {
			X(QReg, j);
		}

		for(int j=0; j<target; j++) {
			H(QReg, j);
		}

		/********************************/
		/* STEP2: inversion             */
		/********************************/
		for(int j=0; j<target; j++) {
			X(QReg, j);
		}

		H(QReg, target-1);

		CCX(QReg, 0, 1, target+1);
		for(int j=1; j<target-1; j++) {
			CCX(QReg, j, target+j, target+j+1);
		}

		CX(QReg, qubits-2, target-1);

		for(int j=1; j<target-1; j++) {
			CCX(QReg, target-1-j, qubits-2-j, qubits-1-j);
		}
		CCX(QReg, 0, 1, target+1);

		H(QReg, target-1);

		for(int j=0; j<target; j++) {
			X(QReg, j);
		}

		for(int j=0; j<target; j++) {
			H(QReg, j);
		}
	}

	/* measure target qubit */
	printf("measure target qubit(%d)\n", target);
	H(QReg, target);
	M(QReg, target);
	dump(QReg);

	/* measure all qubits */
	printf("\nmeasure remaining qubits=%d\n", target);
	for(int i=0; i<qubits; i++) {
		M(QReg, i);
	}
	dump(QReg);
}

int main(int argc, char **argv)
{
	int qubits = 0;
	int c;

	while ((c = getopt_long(argc, argv, "q:", NULL, NULL)) != -1) {
		switch(c) {
		case 'q':
			qubits = atoi(optarg);
		default:
			break;
		}
	}

	if(qubits == 0) {
		printf("<USAGE> : %s -q <qubits>\n", argv[0]);
		exit(0);
	} else if(qubits < 3) {
		printf("we recommend that the qubia number is 7 or more.\n");
		exit(0);
	}

	QRegister *QReg = new QRegister(qubits);

	grover(QReg, qubits);
}

