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
	QRegister *QReg = new QRegister(5);

#if 0
	H(QReg, 0);
	CX(QReg, 0, 1);
	CX(QReg, 0, 2);
#else
	RX(QReg, 0, Q_PI/4);
	RY(QReg, 0, Q_PI/4);
	RX(QReg, 1, Q_PI/4);
	RY(QReg, 1, Q_PI/4);
	RX(QReg, 2, Q_PI/4);
	RY(QReg, 2, Q_PI/4);
	CX(QReg, 0, 3);
	RX(QReg, 3, Q_PI/5);
	RY(QReg, 3, Q_PI/5);
#endif

	dump(QReg);
	showQubitRelation(QReg);
}
