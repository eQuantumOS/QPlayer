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
 * @file    gate.h
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @brief       
 */

#ifndef _EXPERIMENTAL_H
#define _EXPERIMENTAL_H

#include "register.h"

/* These are experimental functions */
extern double getQubitProb(QRegister *QReg, int qubit, int state);
extern int QType(QRegister *QReg, int qubit);
extern char *QTypeStr(QRegister *QReg, int qubit);
extern void getEntanglements(QRegister *QReg);
extern void getEntanglements(QRegister *QReg, std::vector<list<int>>& eGroups);

#endif
