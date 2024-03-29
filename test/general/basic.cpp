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
	QRegister *QReg = new QRegister(4);
	QTimer timer;

	timer.start();
	for(int i=0; i<1000; i++) {
		X(QReg, 0);
		H(QReg, 1);
		CX(QReg, 1, 2);
		M(QReg, 2);
	};
	timer.end();
	
	// dump(QReg);

	QReg->showQRegStat();
	printf("%s\n", timer.getTime());
}
