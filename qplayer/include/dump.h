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

/* These are formal dump functions */
extern void showQState(QRegister *QReg, char *msg, int begin, int end, struct qubit_delimiter *qd);
extern void showQState(QRegister *QReg, char *msg, int begin, int end, struct qubit_delimiter qd);
extern void showQState(QRegister *QReg, char *msg, struct qubit_delimiter qd);
extern void showQState(QRegister *QReg, struct qubit_delimiter qd);
extern void showQState(QRegister *QReg, int begin, int end);
extern void showQState(QRegister *QReg);

/* These are temporal dump functions for surface code debugging */
extern void showQState(QRegister *QReg, int begin11, int end11, int begin21, int end21, int begin31, int end31, int begin12, int end12, int begin22, int end22, int begin32, int end32, int begin13, int end13, int begin23, int end23, int begin33, int end33);
extern void showQState1(QRegister *QReg, int begin11, int end11, int begin21, int end21, int begin31, int end31, int begin12, int end12, int begin22, int end22, int begin32, int end32, int begin13, int end13, int begin23, int end23, int begin33, int end33);
extern void showQState(QRegister *QReg, int begin11, int end11, int begin21, int end21, int begin31, int end31, int begin12, int end12, int begin22, int end22, int begin32, int end32);
extern void showQState(QRegister *QReg, int begin1, int end1, int begin2, int end2, int begin3, int end3); 
extern void showQState(QRegister *QReg, int q1, int q2, int q3, int q4, int q5, int q6, int q7, int q8, int q9);
	
#endif
