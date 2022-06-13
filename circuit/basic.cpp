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

	X(QReg, 0);
	X(QReg, 1);
	X(QReg, 2);
	H(QReg, 4);
	H(QReg, 3);
#if 1
	CSWAP(QReg, 4, 1, 0);
	CSWAP(QReg, 4, 2, 1);
	CSWAP(QReg, 4, 3, 2);
#endif

	dump(QReg);
}
