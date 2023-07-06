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

#include <stdio.h>

#include "qplayer.h"

static QRegister *QReg = NULL;
static int qubits = 10;

void test_gates(void)
{
	H(QReg, 0);
	CX(QReg, 0, 1);
	CX(QReg, 0, 2);
	CX(QReg, 0, 3);
	CX(QReg, 0, 4);
	H(QReg, 0);

	H(QReg, 5);
	CX(QReg, 5, 6);
	CX(QReg, 5, 7);
	CX(QReg, 5, 8);
	CX(QReg, 5, 9);
	H(QReg, 5);

	MF(QReg, 0, 0);
	MF(QReg, 5, 1);

	dump(QReg);
}

int main(int argc, char **argv)
{
 	QReg = new QRegister(16);

	test_gates();
}
