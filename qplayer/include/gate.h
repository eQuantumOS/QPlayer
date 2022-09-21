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

#ifndef _QGATE_H
#define _QGATE_H

#include "register.h"

using namespace std;

extern void initZ(QRegister *QReg, int qubit);
extern void initX(QRegister *QReg, int qubit);
extern void I(QRegister *QReg, int qubit);
extern void X(QRegister *QReg, int qubit);
extern void Z(QRegister *QReg, int qubit);
extern void Y(QRegister *QReg, int qubit);
extern void H(QRegister *QReg, int qubit);
extern void U1(QRegister *QReg, int qubit, double angle);
extern void U2(QRegister *QReg, int qubit, double phi, double lambda);
extern void U3(QRegister *QReg, int qubit, double theta, double pie, double lambda);
extern void S(QRegister *QReg, int qubit);
extern void T(QRegister *QReg, int qubit);
extern void SDG(QRegister *QReg, int qubit);
extern void TDG(QRegister *QReg, int qubit);
extern void RX(QRegister *QReg, int qubit, double angle);
extern void RY(QRegister *QReg, int qubit, double angle);
extern void RZ(QRegister *QReg, int qubit, double angle);
extern void P(QRegister *QReg, int qubit, double angle);
extern void CU1(QRegister *QReg, int control, int target, double lambda);
extern void CU2(QRegister *QReg, int control, int target, double phi, double lambda);
extern void CU3(QRegister *QReg, int control, int target, double theta, double pie, double lambda);
extern void CH(QRegister *QReg, int control, int target);
extern void CX(QRegister *QReg, int control, int target);
extern void CY(QRegister *QReg, int control, int target);
extern void CZ(QRegister *QReg, int control, int target);
extern void CRZ(QRegister *QReg, int control, int target, double angle);
extern void CCX(QRegister *QReg, int control1, int control2, int target);
extern void SWAP(QRegister *QReg, int qubit1, int qubit2);
extern void CSWAP(QRegister *QReg, int control, int qubit1, int qubit2);
extern void SX(QRegister *QReg, int qubit);
extern int M(QRegister *QReg, int qubit);
extern int MV(QRegister *QReg, int qubit);
extern int MF(QRegister *QReg, int qubit, int collapse);
extern int MNOP(QRegister *QReg, int qubit);

/* These are experimental functions */
extern double showQubitProb(QRegister *QReg, int qubit, int state);
extern void showQubitRelation(QRegister *QReg);
extern void showQubitRelation(QRegister *QReg, std::vector<std::vector<int>>& entangleList);
extern int QType(QRegister *QReg, int qubit);
extern char *QTypeStr(QRegister *QReg, int qubit);

#endif
