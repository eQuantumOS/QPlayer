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
 * @file    dump.h
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @brief       
 */

#ifndef _DUMP_H_
#define _DUMP_H_

#include "precision.h"
#include "register.h"
#include "state.h"
#include "misc.h"
#include "timer.h"

extern void dump(QRegister *QReg, char *msg, int begin, int end, struct qubit_delimiter *qd);
extern void dump(QRegister *QReg, char *msg, int begin, int end, struct qubit_delimiter qd);
extern void dump(QRegister *QReg, char *msg, struct qubit_delimiter *qd);
extern void dump(QRegister *QReg, char *msg, struct qubit_delimiter qd);
extern void dump(QRegister *QReg, struct qubit_delimiter *qd);
extern void dump(QRegister *QReg, struct qubit_delimiter qd);
extern void dump(QRegister *QReg, int begin, int end);
extern void dump(QRegister *QReg, char *msg);
extern void dump(QRegister *QReg);
extern void dumpln(QRegister *QReg);

#endif
