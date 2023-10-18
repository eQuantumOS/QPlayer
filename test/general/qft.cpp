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
 * @file    qft.cpp
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @date    10-09-21
 * @brief       
 */

#include <stdio.h>
#include <getopt.h>

#include "qplayer.h"

using namespace std;

void qft(QRegister *QReg, int qubits) {
	QTimer timer;

	timer.start();

	X(QReg, qubits-1);

	for(int i=0; i<qubits; i++) {
		double angle = Q_PI;

		H(QReg, i);
		for(int j=i+1; j<qubits; j++) {
			angle /= 2;
			CRZ(QReg, j, i, angle);
		}
	}

	for(int i=0; i<qubits/2; i++) {
		SWAP(QReg, i, qubits-i-1);
	}

	timer.end();
	printf("%s\n", timer.getTime());
}

int main(int argc, char **argv)
{
	int qubits = 0;
	int c;

	while ((c = getopt_long(argc, argv, "q:", NULL, NULL)) != -1) {
		switch(c) {
		case 'q':
			qubits = atoi(optarg);
			break;
		default:
			break;
		}
	}

	if(qubits == 0) {
		printf("<USAGE> : %s -q <qubits>\n", argv[0]);
		exit(0);
	}

	QRegister *QReg = new QRegister(qubits);

	qft(QReg, qubits);
}
