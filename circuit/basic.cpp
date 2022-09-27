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
 * @file    basic.cpp
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @date    10-09-21
 * @brief       
 */

#include <stdio.h>

#include "qplayer.h"

int main(int argc, char **argv)
{
#if 1
	QRegister *QReg = new QRegister(2);

	H(QReg, 0);
	X(QReg, 1);
	Z(QReg, 1);

	iSWAP(QReg, 0, 1);

	dump(QReg);
#else
	QRegister *QReg = new QRegister(16);
	QTimer timer1;
	QTimer timer2;

	timer1.start();
	H(QReg, 0);
	CX(QReg, 0, 1);
	CX(QReg, 0, 2);
	CX(QReg, 0, 3);
	H(QReg, 0);
	H(QReg, 1);
	RX(QReg, 2, M_PI/8);
	RX(QReg, 3, M_PI/8);

	H(QReg, 4);
	CX(QReg, 4, 5);

	int end = 10;
	for(int i=6; i<=end; i++) 
		H(QReg, i);
	CX(QReg, end, end+1);

	timer1.end();

	std::vector<list<int>> eGroups;

	timer2.start();
	getEntanglements(QReg, eGroups);
	timer2.end();
	printf("time1: %s\n", timer1.getTime());
	printf("time2: %s\n", timer2.getTime());
	printf("states = %ld\n", (uint64_t) QReg->getNumStates());

	for(auto group : eGroups) {
		for(auto qubit : group) {
			printf("%d ", qubit);
		}
		printf("\n");
	}
#endif
}
