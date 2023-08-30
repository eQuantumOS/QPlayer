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
 * @file    gate.cpp
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @date    10-09-21
 * @brief       
 */

#include "gate.h"
#include "dump.h"
#include "experimental.h"

using namespace std;

static void NMC_DiagonalGates(QRegister *QReg, int qubit, int gtype, complex_t M[]);
static void NMC_NoneDiagonalGates(QRegister *QReg, int qubit, int gtype, complex_t M[]);
static void NMC_DiagonalControlGates(QRegister *QReg, int control, int target, int gtype, complex_t M[]);
static void NMC_NoneDiagonalControlGates(QRegister *QReg, int control, int target, int gtype, complex_t M[]);
static int NMC_Measure(QRegister *QReg, int qubit);
static int NMC_MeasureF(QRegister *QReg, int qubit, int collapse);

/*
 * initZ() initialize the qubit to |0>.
 */
void initZ(QRegister *QReg, int qubit)
{
	if(M(QReg, qubit) == 1) {
		X(QReg, qubit);
	}
}

/*
 * initX() initialize the qubit to |+>.
 */
void initX(QRegister *QReg, int qubit)
{
	if(M(QReg, qubit) == 1) {
		X(QReg, qubit);
	}
	H(QReg, qubit);
}

/*
 * Apply I-gate to the qubit
 */
void I(QRegister *QReg, int qubit) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0), complex_t(0, 0),
		complex_t(0, 0), complex_t(1, 0)
	};

	timer.start();
	timer.end();
	QReg->updateQRegStat(QGATE_ID, timer);
}

/*
 * Apply X-gate to the qubit
 */
void X(QRegister *QReg, int qubit) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(0, 0), complex_t(1, 0),
		complex_t(1, 0), complex_t(0, 0)
	};

	timer.start();
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_X, M);
	timer.end();
	QReg->updateQRegStat(QGATE_X, timer);
}

/*
 * Apply Z-gate to the qubit
 */
void Z(QRegister *QReg, int qubit) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0), complex_t(0, 0),
		complex_t(0, 0), complex_t(-1, 0)
	};

	timer.start();
	NMC_DiagonalGates(QReg, qubit, QGATE_Z, M);
	timer.end();
	QReg->updateQRegStat(QGATE_Z, timer);
}

/*
 * Apply Y-gate to the qubit
 */
void Y(QRegister *QReg, int qubit) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(0, 0), complex_t(0, -1),
		complex_t(0, 1), complex_t(0, 0)
	};

	timer.start();
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_X, M);
	timer.end();
	QReg->updateQRegStat(QGATE_Y, timer);
}

/*
 * Apply H-gate to the qubit
 */
void H(QRegister *QReg, int qubit) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(R_SQRT_2, 0), complex_t(R_SQRT_2, 0),
		complex_t(R_SQRT_2, 0), complex_t(-R_SQRT_2, 0)
	};

	timer.start();
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_H, M);
	timer.end();
	QReg->updateQRegStat(QGATE_H, timer);
}

/*
 * Apply U1-gate to the qubit
 */
void U1(QRegister *QReg, int qubit, double angle)
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0), complex_t(0, 0),
		complex_t(0, 0), complex_t(cos(angle), sin(angle))
	};

	timer.start();
	NMC_DiagonalGates(QReg, qubit, QGATE_U1, M);
	timer.end();
	QReg->updateQRegStat(QGATE_U1, timer);
}

/*
 * Apply U2-gate to the qubit
 */
void U2(QRegister *QReg, int qubit, double phi, double lambda)
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0) / std::sqrt(2), 
		-complex_t(cos(lambda), sin(lambda)) / std::sqrt(2),
		complex_t(cos(phi), sin(phi)) / std::sqrt(2), 
		complex_t(cos(phi+lambda), sin(phi+lambda)) / std::sqrt(2)
	};

	timer.start();
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_U2, M);
	timer.end();
	QReg->updateQRegStat(QGATE_U2, timer);
}

/*
 * Apply U3-gate to the qubit
 */
void U3(QRegister *QReg, int qubit, double theta, double phi, double lambda)
{
	QTimer timer;
	complex_t M[] = {
		complex_t(cos(theta/2), 0), 
		complex_t(-cos(lambda), -sin(lambda))*sin(theta/2),
		complex_t(cos(phi), sin(phi))*sin(theta/2), 
		complex_t(cos(phi+lambda), sin(phi+lambda))*cos(theta/2)
	};

	timer.start();
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_U3, M);
	timer.end();
	QReg->updateQRegStat(QGATE_U3, timer);
}


/*
 * Apply S-gate to the qubit
 */
void S(QRegister *QReg, int qubit) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0), complex_t(0, 0),
		complex_t(0, 0), complex_t(0, 1)
	};

	timer.start();
	NMC_DiagonalGates(QReg, qubit, QGATE_S, M);
	timer.end();
	QReg->updateQRegStat(QGATE_S, timer);
}

/*
 * Apply T-gate to the qubit
 */
void T(QRegister *QReg, int qubit) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0), complex_t(0, 0),
		complex_t(0, 0), complex_t(cos(Q_PI/4), sin(Q_PI/4))
	};

	timer.start();
	NMC_DiagonalGates(QReg, qubit, QGATE_T, M);
	timer.end();
	QReg->updateQRegStat(QGATE_T, timer);
}

/*
 * Apply S+-gate to the qubit
 */
void SDG(QRegister *QReg, int qubit)
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0), complex_t(0, 0),
		complex_t(0, 0), complex_t(0, -1)
	};

	timer.start();
	NMC_DiagonalGates(QReg, qubit, QGATE_SDG, M);
	timer.end();
	QReg->updateQRegStat(QGATE_SDG, timer);
}

/*
 * Apply T+-gate to the qubit
 */
void TDG(QRegister *QReg, int qubit)
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0), complex_t(0, 0),
		complex_t(0, 0), complex_t(cos(Q_PI/4), -sin(Q_PI/4))
	};

	timer.start();
	NMC_DiagonalGates(QReg, qubit, QGATE_TDG, M);
	timer.end();
	QReg->updateQRegStat(QGATE_TDG, timer);
}

/*
 * Apply RX-gate to the qubit
 */
void RX(QRegister *QReg, int qubit, double angle) 
{
	QTimer timer;
	complex_t M[] = {
		cos(angle/2), complex_t(0, -sin(angle/2)),
		complex_t(0, -sin(angle/2)), cos(angle/2)
	};

	timer.start();
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_RX, M);
	timer.end();
	QReg->updateQRegStat(QGATE_RX, timer);
}

/*
 * Apply RY-gate to the qubit
 */
void RY(QRegister *QReg, int qubit, double angle) 
{
	QTimer timer;
	complex_t M[] = {
		cos(angle/2), -sin(angle/2),
		sin(angle/2), cos(angle/2)
	};

	timer.start();
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_RY, M);
	timer.end();
	QReg->updateQRegStat(QGATE_RY, timer);
}

/*
 * Apply RZ-gate to the qubit
 */
void RZ(QRegister *QReg, int qubit, double angle) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(cos(-angle/2), sin(-angle/2)), 0, 
		0, complex_t(cos(angle/2), sin(angle/2))
	};

	timer.start();
	NMC_DiagonalGates(QReg, qubit, QGATE_RZ, M);
	timer.end();
	QReg->updateQRegStat(QGATE_RZ, timer);
}

/*
 * Apply P-gate to the qubit
 */
void P(QRegister *QReg, int qubit, double angle) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0), complex_t(0, 0),
		complex_t(0, 0), complex_t(cos(angle), sin(angle))
	};

	timer.start();
	NMC_DiagonalGates(QReg, qubit, QGATE_P, M);
	timer.end();
	QReg->updateQRegStat(QGATE_P, timer);
}

/*
 * Apply SX-gate to the qubit
 */
void SX(QRegister *QReg, int qubit) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 1)/std::sqrt(2), complex_t(1, -1)/std::sqrt(2),
		complex_t(1, -1)/std::sqrt(2), complex_t(1, 1)/std::sqrt(2)
	};

	timer.start();
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_SX, M);
	QReg->normalize();
	timer.end();
	QReg->updateQRegStat(QGATE_SX, timer);
}

/*
 * Apply Controlled-U1 gate
 */
void CU1(QRegister *QReg, int control, int target, double angle) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0), complex_t(0, 0),
		complex_t(0, 0), complex_t(cos(angle), sin(angle))
	};

	if(control == target) {
		return;
	}

	timer.start();
	NMC_DiagonalControlGates(QReg, control, target, QGATE_CU1, M);
	timer.end();
	QReg->updateQRegStat(QGATE_CU1, timer);
}

/*
 * Apply Controlled-U2 gate
 */
void CU2(QRegister *QReg, int control, int target, double phi, double lambda) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0) / std::sqrt(2), 
		-complex_t(cos(lambda), sin(lambda)) / std::sqrt(2),
		complex_t(cos(phi), sin(phi)) / std::sqrt(2), 
		complex_t(cos(phi+lambda), sin(phi+lambda)) / std::sqrt(2)
	};

	if(control == target) {
		return;
	}

	timer.start();
	NMC_NoneDiagonalControlGates(QReg, control, target, QGATE_CU2, M);
	timer.end();
	QReg->updateQRegStat(QGATE_CU2, timer);
}

/*
 * Apply Controlled-U1 gate
 */
void CU3(QRegister *QReg, int control, int target, double theta, double pie, double lambda) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(cos(theta/2), 0), 
		complex_t(-cos(lambda), -sin(lambda))*sin(theta/2),
		complex_t(cos(pie), sin(pie))*sin(theta/2), 
		complex_t(cos(pie+lambda), sin(pie+lambda))*cos(theta/2)
	};

	if(control == target) {
		return;
	}

	timer.start();
	NMC_NoneDiagonalControlGates(QReg, control, target, QGATE_CU3, M);
	timer.end();
	QReg->updateQRegStat(QGATE_CU3, timer);
}

/*
 * Apply controlled-H to the qubit
 */
void CH(QRegister *QReg, int control, int target) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(R_SQRT_2, 0), complex_t(R_SQRT_2, 0),
		complex_t(R_SQRT_2, 0), complex_t(-R_SQRT_2, 0)
	};

	if(control == target) {
		return;
	}

	timer.start();
	NMC_NoneDiagonalControlGates(QReg, control, target, QGATE_CH, M);
	timer.end();
	QReg->updateQRegStat(QGATE_CH, timer);
}

/*
 * Apply CNOT-gate to the qubit
 */
void CX(QRegister *QReg, int control, int target) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(0, 0), complex_t(1, 0),
		complex_t(1, 0), complex_t(0, 0)
	};

	if(control == target) {
		return;
	}

	timer.start();
	NMC_NoneDiagonalControlGates(QReg, control, target, QGATE_CX, M);
	timer.end();
	QReg->updateQRegStat(QGATE_CX, timer);
}

/*
 * Apply controlled-Y to the qubit
 */
void CY(QRegister *QReg, int control, int target) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(0, 0), complex_t(0, -1),
		complex_t(0, 1), complex_t(0, 0)
	};


	if(control == target) {
		return;
	}

	timer.start();
	NMC_NoneDiagonalControlGates(QReg, control, target, QGATE_CY, M);
	timer.end();
	QReg->updateQRegStat(QGATE_CY, timer);
}

/*
 * Apply controlled-Z to the qubit
 */
void CZ(QRegister *QReg, int control, int target) 
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0), complex_t(0, 0),
		complex_t(0, 0), complex_t(-1, 0)
	};

	if(control == target) {
		return;
	}

	timer.start();
	NMC_DiagonalControlGates(QReg, control, target, QGATE_CZ, M);
	timer.end();
	QReg->updateQRegStat(QGATE_CZ, timer);
}

/*
 * Apply CRZ-gate to the qubit
 */
void CRZ(QRegister *QReg, int control, int target, double angle) 
{
	QTimer timer;
	complex_t M[4];

	M[0] = complex_t(cos(-angle/2), sin(-angle/2));
	M[1] = 0.0;
	M[2] = 0.0;
	M[3] = complex_t(cos(angle/2), sin(angle/2));;

	double n = norm(M[0]);
	if(n > 10e-9) {
		complex_t p(M[0].real()/n,M[0].imag()/n);
		M[0] /= p;
		M[1] /= p;
		M[2] /= p;
		M[3] /= p;
	} else {
		double n = norm(M[1]);
		complex_t p(M[0].real()/n,M[0].imag()/n);
		M[0] /= p;
		M[1] /= p;
		M[2] /= p;
		M[3] /= p;
	}

	timer.start();
	NMC_DiagonalControlGates(QReg, control, target, QGATE_CRZ, M);
	timer.end();
	QReg->updateQRegStat(QGATE_CRZ, timer);
}

/*
 * Apply Toffoli-gate to the qubit
 */
void CCX(QRegister *QReg, int control1, int control2, int target)
{
	QTimer timer;
	timer.start();

	H(QReg, target);
	CX(QReg, control2, target);
	TDG(QReg, target);
	CX(QReg, control1, target);
	T(QReg, target);
	CX(QReg, control2, target);
	TDG(QReg, target);
	CX(QReg, control1, target);
	T(QReg, control2);
	T(QReg, target);
	CX(QReg, control1, control2);
	H(QReg, target);
	T(QReg, control1);
	TDG(QReg, control2);
	CX(QReg, control1, control2);

	timer.end();
	QReg->updateQRegStat(QGATE_CCX, timer);
}

/*
 * Apply SWAP-gate to the qubit
 */
void SWAP(QRegister *QReg, int qubit1, int qubit2) 
{
	QTimer timer;
	timer.start();

	if(qubit1 > QReg->getNumQubits()) {
		printf("qubit1(%d) out of range!\n", qubit1);
		exit(0);
	}

	if(qubit2 > QReg->getNumQubits()) {
		printf("qubit2 qubit(%d) out of range!\n", qubit2);
		exit(0);
	}

	if(qubit1 == qubit2) {
		printf("SWAP does not allow the same qubit(%d -> %d)!\n", qubit1, qubit2);
		exit(0);
	}

	CX(QReg, qubit1, qubit2);
	CX(QReg, qubit2, qubit1);
	CX(QReg, qubit1, qubit2);

	timer.end();
	QReg->updateQRegStat(QGATE_SWAP, timer);
}

/*
 * Apply iSWAP-gate to the qubit
 */
void iSWAP(QRegister *QReg, int qubit1, int qubit2) 
{
	QTimer timer;
	timer.start();

	S(QReg, qubit1);
	S(QReg, qubit2);
	H(QReg, qubit1);
	CX(QReg, qubit1, qubit2);
	CX(QReg, qubit2, qubit1);
	H(QReg, qubit2);

	timer.end();
	QReg->updateQRegStat(QGATE_ISWAP, timer);
}

/*
 * Apply CSWAP-gate to the qubit
 */
void CSWAP(QRegister *QReg, int control, int qubit1, int qubit2) 
{
	QTimer timer;
	timer.start();

	CX(QReg, qubit2, qubit1);
	CCX(QReg, control, qubit1, qubit2);
	CX(QReg, qubit2, qubit1);

	timer.end();
	QReg->updateQRegStat(QGATE_CSWAP, timer);
}

/*
 * Apply Z-basis measurement 
 */
int M(QRegister *QReg, int qubit)
{
	QTimer timer;
	int val = 0;

	timer.start();
	val = NMC_Measure(QReg, qubit);
	timer.end();
	QReg->updateQRegStat(QGATE_MEASURE, timer);

	return val;
}

/*
 * Apply forced Z-basis measurement 
 */
int MF(QRegister *QReg, int qubit, int collapse)
{
	QTimer timer;
	int val = 0;

	timer.start();
	val = NMC_MeasureF(QReg, qubit, collapse);
	timer.end();
	QReg->updateQRegStat(QGATE_MEASURE, timer);

	return val;
}

int MV(QRegister *QReg, int qubit) {
	int mv = M(QReg, qubit);

	return mv == 0 ? 1 : -1;
}

/******************************************************************
 *
 * Internal functions to support gate-aware matrix fuctions.
 *
 *  - NMC_DiagonalGates: Z, S, T, SDG, TDG, RZ, U1
 *  - NMC_NoneDiagonalGates: X, Y, H, U2, U3, RX, RY, SX
 *  - NMC_DiagonalControlGates: CZ, CU1, CRZ
 *  - NMC_NoneDiagonalControlGates: CX, CY, CU2, CU3, CH, CSWAP
 *  - NMC_Measure: M
 *
 ******************************************************************/
static void NMC_DiagonalGates(QRegister *QReg, int qubit, int gtype, complex_t M[])
{
	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		exit(0);
	}

	if(QReg->qubitTypes[qubit] == KET_ZERO && gtype != QGATE_RZ) {
		/* do nothing */
		return;
	}

	/************************************************** 
	 * (STEP1) evolve quantum states 
	 **************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		int thread_id = omp_get_thread_num();
		QState *Q = NULL;
		complex_t newAmp;
		qsize_t qidx;

		QMAPITER it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			Q = it->second;
			qidx = Q->getIndex();

			/* Despite the redundancy of the code, we subdivide the code 
			 * as shown below to maximize computational performance.
			 */
			if(QReg->qubitTypes[qubit] == KET_ZERO) {
				/* RZ */
				newAmp = Q->getAmplitude() * M[0];
				Q->setAmplitude(newAmp);
			} else if(QReg->qubitTypes[qubit] == KET_ONE) {
				/* Z, S, T, SDG, TDG, RZ, U1 */
				newAmp = Q->getAmplitude() * M[3];
				Q->setAmplitude(newAmp);
			} else {
				if(stripe_lower(qidx, qubit) == true) {
					/* RZ */
					newAmp = Q->getAmplitude() * M[0];
					Q->setAmplitude(newAmp);
				} else {
					/* Z, S, T, SDG, TDG, RZ, U1 */
					newAmp = Q->getAmplitude() * M[3];
					Q->setAmplitude(newAmp);
				}
			}
		} 
	}

	/************************************************** 
	 * (STEP2) update qubit type
	 **************************************************/
	/* There is no target to update the qubit type. */
}

static void NMC_NoneDiagonalGates(QRegister *QReg, int qubit, int gtype, complex_t M[])
{
	QReg->checkMemory();

	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		exit(0);
	}

	qsize_t stride = quantum_shiftL(1, (qsize_t)qubit);
	int qubitType = QReg->qubitTypes[qubit];

	vector<QState*> addQList[QSTORE_PARTITION][MAX_CORES];
	vector<QState*> delQList[QSTORE_PARTITION][MAX_CORES];
	
	bool isLower = false;
	bool isUpper = false;

	/************************************************** 
	 * (STEP1) evolve quantum states 
	 **************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		int thread_id = omp_get_thread_num();
		int tid = thread_id % QReg->getCPUCores();
		bool isLowerLocal = false;
		bool isUpperLocal = false;
		QState *Q = NULL;
		QState *lowerQ = NULL;
		QState *upperQ = NULL;
		complex_t oldAmp0;
		complex_t oldAmp1;
		complex_t newAmp0;
		complex_t newAmp1;
		qsize_t qidx = 0;
		qsize_t i0 = 0;
		qsize_t i1 = 0;
		int hashid = 0;

		QMAPITER it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			Q = it->second;
			qidx = Q->getIndex();

			/*****************************************************
			 * (NOTICE!!) Despite the redundancy of the code, we 
			 * subdivide the code by scenario as shown below to 
			 * increase the readability of the source code.
			 *****************************************************/
			if(qubitType == KET_ZERO) {
				/************************************************* 
				 * (Scenario#1) Qubit is |0> state.
				 * 
				 *  state will evolves as follows 
				 *    a|0> --> b|0> + c|1> 
				 *************************************************/
				oldAmp0 = Q->getAmplitude();
				oldAmp1 = 0;

				i0 = qidx;
				i1 = qidx + stride;

				newAmp0 = M[0]*oldAmp0 + M[1]*oldAmp1;
				newAmp1 = M[2]*oldAmp0 + M[3]*oldAmp1;

				/* update lower state */
				if(isRealizedState(newAmp0) == true) {
					/* update existing |0> amplitude */
					Q->setAmplitude(newAmp0);
					isLowerLocal = true;
				} else {
					/* remove zero amplitude |0> state */
					hashid = (int)(i0 % QSTORE_PARTITION);
					delQList[hashid][tid].push_back(Q);
				}

				/* update upper state */
				if(isRealizedState(newAmp1) == true) {
					/* add new |1> state */
					upperQ = getQState(i1, newAmp1);
					hashid = (int)(i1 % QSTORE_PARTITION);
					addQList[hashid][tid].push_back(upperQ);
					isUpperLocal = true;
				}
			} else if(qubitType == KET_ONE) {
				/************************************************* 
				 * (Scenario#2) Qubit is |1> state.
				 * 
				 *  state will evolves as follows 
				 *    a|1> --> b|0> + c|1> 
				 *************************************************/
				oldAmp0 = 0;
				oldAmp1 = Q->getAmplitude();

				i0 = qidx - stride;
				i1 = qidx;

				newAmp0 = M[0]*oldAmp0 + M[1]*oldAmp1;
				newAmp1 = M[2]*oldAmp0 + M[3]*oldAmp1;

				/* update lower state */
				if(isRealizedState(newAmp0) == true) {
					/* add new |0> state */
					lowerQ = getQState(i0, newAmp0);
					hashid = (int)(i0 % QSTORE_PARTITION);
					addQList[hashid][tid].push_back(lowerQ);
					isLowerLocal = true;
				}

				/* update upper state */
				if(isRealizedState(newAmp1) == true) {
					/* update existing |1> amplitude */
					Q->setAmplitude(newAmp1);
					isUpperLocal = true;
				} else {
					/* remove zero amplitude |1> state */
					hashid = (int)(i1 % QSTORE_PARTITION);
					delQList[hashid][tid].push_back(Q);
				}
			} else {
				/************************************************* 
				 * (Scenario#3) Qubit is in superposed state    
				 *
				 * This is divided into following sub-scenarios 
				 *************************************************/
				if(stripe_lower(qidx, qubit) == true) {
					i0 = qidx;
					i1 = qidx + stride;

					lowerQ = Q;
					upperQ = QReg->findQState_nolock(i1);
				} else {
					i0 = qidx - stride;
					i1 = qidx;

					lowerQ = QReg->findQState_nolock(i0);
					upperQ = Q;
				}

				if(lowerQ != NULL && upperQ != NULL) {
					/******************************************
					 * (Sub3-1) two matrix pair exist             
					 *  - just update new amplitudes 
				 	 ******************************************/
					oldAmp0 = lowerQ->getAmplitude();
					oldAmp1 = upperQ->getAmplitude();

					if(lowerQ->getIndex() != qidx) {
						/* (Important!!) To ensure concurrency when all matrix pairs are
						   exist, only the thread assigned lowerQ updates the state. */
						continue;
					}

					newAmp0 = M[0]*oldAmp0 + M[1]*oldAmp1;
					newAmp1 = M[2]*oldAmp0 + M[3]*oldAmp1;

					if(isRealizedState(newAmp0) == true) {
						lowerQ->setAmplitude(newAmp0);
						isLowerLocal = true;
					} else {
						/* remove zero amplitude |0> state */
						hashid = (int)(i0 % QSTORE_PARTITION);
						delQList[hashid][tid].push_back(lowerQ);
					}

					if(isRealizedState(newAmp1) == true) {
						upperQ->setAmplitude(newAmp1);
						isUpperLocal = true;
					} else {
						/* remove zero amplitude |1> state */
						hashid = (int)(i1 % QSTORE_PARTITION);
						delQList[hashid][tid].push_back(upperQ);
					}
				} else if(lowerQ != NULL) {
					/******************************************
					 * (Sub3-2) one of matrix pair(|0>) exists
				 	 *  state will evolves as follows 
		 			 *   a|0> --> b|0> + c|1> 
				 	 ******************************************/
					oldAmp0 = lowerQ->getAmplitude();
					oldAmp1 = 0;

					i0 = qidx;
					i1 = qidx + stride;

				#if 0
					newAmp0 = M[0]*oldAmp0 + M[1]*oldAmp1;
					newAmp1 = M[2]*oldAmp0 + M[3]*oldAmp1;
				#else
					newAmp0 = M[0]*oldAmp0;
					newAmp1 = M[2]*oldAmp0;
				#endif

					/* update lower state */
					if(isRealizedState(newAmp0) == true) {
						/* update existing |0> amplitude */
						lowerQ->setAmplitude(newAmp0);
						isLowerLocal = true;
					} else {
						/* remove zero amplitude |0> state */
						hashid = (int)(i0 % QSTORE_PARTITION);
						delQList[hashid][tid].push_back(lowerQ);
					}

					/* update upper state */
					if(isRealizedState(newAmp1) == true) {
						/* add new upper state */
						upperQ = getQState(i1, newAmp1);
						hashid = (int)(i1 % QSTORE_PARTITION);
						addQList[hashid][tid].push_back(upperQ);
						isUpperLocal = true;
					}
				} else if(upperQ != NULL) {
					/******************************************
					 * (Sub3-3) one of matrix pair(|1>) exists
				 	 *  state will evolves as follows 
		 			 *   a|1> --> b|0> + c|1> 
				 	 ******************************************/
					oldAmp0 = 0;
					oldAmp1 = upperQ->getAmplitude();

					i0 = qidx - stride;
					i1 = qidx;

				#if 0
					newAmp0 = M[0]*oldAmp0 + M[1]*oldAmp1;
					newAmp1 = M[2]*oldAmp0 + M[3]*oldAmp1;
				#else
					newAmp0 = M[1]*oldAmp1;
					newAmp1 = M[3]*oldAmp1;
				#endif

					/* update lower state */
					if(isRealizedState(newAmp0) == true) {
						/* add new |0> state */
						lowerQ = getQState(i0, newAmp0);
						hashid = (int)(i0 % QSTORE_PARTITION);
						addQList[hashid][tid].push_back(lowerQ);
						isLowerLocal = true;
					} 
	
					/* update upper state */
					if(isRealizedState(newAmp1) == true) {
						/* update existing |1> amplitude */
						upperQ->setAmplitude(newAmp1);
						isUpperLocal = true;
					} else {
						/* remove zero amplitude |1> state */
						hashid = (int)(i1 % QSTORE_PARTITION);
						delQList[hashid][tid].push_back(upperQ);
					}
				}
			}
		}

		if(isLowerLocal == true) isLower = true;
		if(isUpperLocal == true) isUpper = true;
	}

	/******************************************************** 
	 * (STEP2) remove zero amplitude states or add new state
	 ********************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<QReg->getCPUCores(); j++) {
			for(auto Q : delQList[i][j]) {
				QReg->eraseQState_nolock(Q->getIndex());
			}
			for(auto Q : addQList[i][j]) {
				QReg->setQState_nolock(Q->getIndex(), Q);
			}
			addQList[i][j].clear();
			delQList[i][j].clear();
		}
	}

	/************************************************** 
	 * (STEP3) update qubit type
	 **************************************************/
	if(isLower == true && isUpper == true) {
		QReg->qubitTypes[qubit] = KET_SUPERPOSED;
	} else if(isLower == true) {
		QReg->qubitTypes[qubit] = KET_ZERO;
	} else if(isUpper == true) {
		QReg->qubitTypes[qubit] = KET_ONE;
	}
}

static void NMC_DiagonalControlGates(QRegister *QReg, int control, int target, int gtype, complex_t M[])
{
	if(control >= QReg->getNumQubits()) {
		printf("control(%d) out of range!\n", control);
		exit(0);
	}

	if(target >= QReg->getNumQubits()) {
		printf("target(%d) out of range!\n", target);
		exit(0);
	}

	if(QReg->qubitTypes[control] == KET_ZERO) {
		/* do nothing */
		return;
	}

	/************************************************** 
	 * (STEP1) apply matrix
	 *************************************************/ 
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		int thread_id = omp_get_thread_num();
		cpu_set_t cpuset;
		QState *Q = NULL;
		qsize_t qidx;
		complex_t oldAmp;
		complex_t newAmp;

		QMAPITER it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			Q = it->second;
			qidx = Q->getIndex();
			oldAmp = Q->getAmplitude();
			newAmp = 0;

			if(QReg->qubitTypes[control] == KET_SUPERPOSED) {
				if(stripe_lower(qidx, control) == true) {
					continue;
				}
			}

			/* Despite the redundancy of the code, we subdivide the code 
			 * as shown below to maximize computational performance.
			 */

			if(QReg->qubitTypes[target] == KET_ZERO) {
				/* RZ */
				newAmp = oldAmp * M[0];
				Q->setAmplitude(newAmp);
			} else if(QReg->qubitTypes[target] == KET_ONE) {
				/* Z, S, T, SDG, TDG, RZ, U1 */
				newAmp = oldAmp * M[3];
				Q->setAmplitude(newAmp);
			} else {
				if(stripe_lower(qidx, target) == true) {
					/* RZ */
					newAmp = oldAmp * M[0];
					Q->setAmplitude(newAmp);
				} else {
					/* Z, S, T, SDG, TDG, RZ, U1 */
					newAmp = oldAmp * M[3];
					Q->setAmplitude(newAmp);
				}
			}
		}
	}

	/************************************************** 
	 * (STEP2) update qubit type
	 **************************************************/
	/* There is no target to update the qubit type. */
}

static void NMC_NoneDiagonalControlGates(QRegister *QReg, int control, int target, int gtype, complex_t M[])
{
	QReg->checkMemory();

	if(control >= QReg->getNumQubits()) {
		printf("control(%d) out of range!\n", control);
		exit(0);
	}

	if(target >= QReg->getNumQubits()) {
		printf("target(%d) out of range!\n", target);
		exit(0);
	}

	if(QReg->qubitTypes[control] == KET_ZERO) {
		/* do nothing */
		return;
	}

	qsize_t stride = quantum_shiftL(1, (qsize_t)target);
	int targetQubitType = QReg->qubitTypes[target];

	vector<QState*> addQList[QSTORE_PARTITION][MAX_CORES];
	vector<QState*> delQList[QSTORE_PARTITION][MAX_CORES];
	
	bool isLower = false;
	bool isUpper = false;

	/************************************************** 
	 * (STEP1) apply matrix
	 *************************************************/ 
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		int thread_id = omp_get_thread_num();
		int tid = thread_id % QReg->getCPUCores();
		cpu_set_t cpuset;
		bool isLowerLocal = false;
		bool isUpperLocal = false;
		QState *lowerQ = NULL;
		QState *upperQ = NULL;
		QState *Q = NULL;
		qsize_t qidx;
		qsize_t i0;
		qsize_t i1;
		complex_t oldAmp0;
		complex_t oldAmp1;
		complex_t newAmp0;
		complex_t newAmp1;
		int hashid;

		QMAPITER it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			Q = it->second;
			qidx = Q->getIndex();

			if(QReg->qubitTypes[control] == KET_SUPERPOSED) {
				if(stripe_lower(qidx, control) == true) {
					/* control bit is |0> */
					if(stripe_lower(qidx, target) == true) {
						isLowerLocal = true;
					} else {
						isUpperLocal = true;
					}
					continue;
				}
			}

			/*****************************************************
			 * (NOTICE!!) Despite the redundancy of the code, we 
			 * subdivide the code by scenario as shown below to 
			 * increase the readability of the source code.
			 *****************************************************/
			if(targetQubitType == KET_ZERO) {
				/************************************************* 
				 * (Scenario#1) Qubit is |0> state.
				 * 
				 *  state will evolves as follows 
				 *    a|0> --> b|0> + c|1> 
				 *************************************************/
				oldAmp0 = Q->getAmplitude();
				oldAmp1 = 0;

				i0 = qidx;
				i1 = qidx + stride;

			#if 0
				newAmp0 = M[0]*oldAmp0 + M[1]*oldAmp1;
				newAmp1 = M[2]*oldAmp0 + M[3]*oldAmp1;
			#else
				newAmp0 = M[0]*oldAmp0;
				newAmp1 = M[2]*oldAmp0;
			#endif

				/* update lower state */
				if(isRealizedState(newAmp0) == true) {
					/* update existing |0> amplitude */
					Q->setAmplitude(newAmp0);
					isLowerLocal = true;
				} else {
					/* remove zero amplitude |0> state */
					hashid = (int)(i0 % QSTORE_PARTITION);
					delQList[hashid][tid].push_back(Q);
				}

				/* update upper state */
				if(isRealizedState(newAmp1) == true) {
					/* add new |1> state */
					upperQ = getQState(i1, newAmp1);
					hashid = (int)(i1 % QSTORE_PARTITION);
					addQList[hashid][tid].push_back(upperQ);
					isUpperLocal = true;
				}
			} else if(targetQubitType == KET_ONE) {
				/************************************************* 
				 * (Scenario#2) Qubit is |1> state.
				 * 
				 *  state will evolves as follows 
				 *    a|1> --> b|0> + c|1> 
				 *************************************************/
				oldAmp0 = 0;
				oldAmp1 = Q->getAmplitude();

				i0 = qidx - stride;
				i1 = qidx;

			#if 0
				newAmp0 = M[0]*oldAmp0 + M[1]*oldAmp1;
				newAmp1 = M[2]*oldAmp0 + M[3]*oldAmp1;
			#else
				newAmp0 = M[1]*oldAmp1;
				newAmp1 = M[3]*oldAmp1;
			#endif

				/* update lower state */
				if(isRealizedState(newAmp0) == true) {
					/* add new |0> state */
					lowerQ = getQState(i0, newAmp0);
					hashid = (int)(i0 % QSTORE_PARTITION);
					addQList[hashid][tid].push_back(lowerQ);
					isLowerLocal = true;
				}

				/* update upper state */
				if(isRealizedState(newAmp1) == true) {
					/* update existing |1> amplitude */
					Q->setAmplitude(newAmp1);
					isUpperLocal = true;
				} else {
					/* remove zero amplitude |1> state */
					hashid = (int)(i1 % QSTORE_PARTITION);
					delQList[hashid][tid].push_back(Q);
				}
			} else {
				/************************************************* 
				 * (Scenario#3) Qubit is in superposed state    
				 *
				 * This is divided into following sub-scenarios 
				 *************************************************/
				if(stripe_lower(qidx, target) == true) {
					i0 = qidx;
					i1 = qidx + stride;

					lowerQ = Q;
					upperQ = QReg->findQState_nolock(i1);
				} else {
					i0 = qidx - stride;
					i1 = qidx;

					lowerQ = QReg->findQState_nolock(i0);
					upperQ = Q;
				}

				if(lowerQ != NULL && upperQ != NULL) {
					/******************************************
					 * (Sub3-1) two matrix pair exist             
					 *  - just update new amplitudes 
				 	 ******************************************/
					oldAmp0 = lowerQ->getAmplitude();
					oldAmp1 = upperQ->getAmplitude();

					if(lowerQ->getIndex() != qidx) {
						/* (Important!!) To ensure concurrency when all matrix pairs are
						   exist, only the thread assigned lowerQ updates the state. */
						continue;
					}

					newAmp0 = M[0]*oldAmp0 + M[1]*oldAmp1;
					newAmp1 = M[2]*oldAmp0 + M[3]*oldAmp1;

					if(isRealizedState(newAmp0) == true) {
						lowerQ->setAmplitude(newAmp0);
						isLowerLocal = true;
					} else {
						/* remove zero amplitude |0> state */
						hashid = (int)(i0 % QSTORE_PARTITION);
						delQList[hashid][tid].push_back(lowerQ);
					}

					if(isRealizedState(newAmp1) == true) {
						upperQ->setAmplitude(newAmp1);
						isUpperLocal = true;
					} else {
						/* remove zero amplitude |1> state */
						hashid = (int)(i1 % QSTORE_PARTITION);
						delQList[hashid][tid].push_back(upperQ);
					}
				} else if(lowerQ != NULL) {
					/******************************************
					 * (Sub3-2) one of matrix pair(|0>) exists
				 	 *  state will evolves as follows 
		 			 *   a|0> --> b|0> + c|1> 
				 	 ******************************************/
					oldAmp0 = lowerQ->getAmplitude();
					oldAmp1 = 0;

					i0 = qidx;
					i1 = qidx + stride;

				#if 0
					newAmp0 = M[0]*oldAmp0 + M[1]*oldAmp1;
					newAmp1 = M[2]*oldAmp0 + M[3]*oldAmp1;
				#else
					newAmp0 = M[0]*oldAmp0;
					newAmp1 = M[2]*oldAmp0;
				#endif

					/* update lower state */
					if(isRealizedState(newAmp0) == true) {
						/* update existing |0> amplitude */
						lowerQ->setAmplitude(newAmp0);
						isLowerLocal = true;
					} else {
						/* remove zero amplitude |0> state */
						hashid = (int)(i0 % QSTORE_PARTITION);
						delQList[hashid][tid].push_back(lowerQ);
					}

					/* update upper state */
					if(isRealizedState(newAmp1) == true) {
						/* add new upper state */
						upperQ = getQState(i1, newAmp1);
						hashid = (int)(i1 % QSTORE_PARTITION);
						addQList[hashid][tid].push_back(upperQ);
						isUpperLocal = true;
					}
				} else if(upperQ != NULL) {
					/******************************************
					 * (Sub3-3) one of matrix pair(|1>) exists
				 	 *  state will evolves as follows 
		 			 *   a|1> --> b|0> + c|1> 
				 	 ******************************************/
					oldAmp0 = 0;
					oldAmp1 = upperQ->getAmplitude();

					i0 = qidx - stride;
					i1 = qidx;

				#if 0
					newAmp0 = M[0]*oldAmp0 + M[1]*oldAmp1;
					newAmp1 = M[2]*oldAmp0 + M[3]*oldAmp1;
				#else
					newAmp0 = M[1]*oldAmp1;
					newAmp1 = M[3]*oldAmp1;
				#endif

					/* update lower state */
					if(isRealizedState(newAmp0) == true) {
						/* add new |0> state */
						lowerQ = getQState(i0, newAmp0);
						hashid = (int)(i0 % QSTORE_PARTITION);
						addQList[hashid][tid].push_back(lowerQ);
						if(isLowerLocal == false) isLowerLocal = true;
					} 
	
					/* update upper state */
					if(isRealizedState(newAmp1) == true) {
						/* update existing |1> amplitude */
						upperQ->setAmplitude(newAmp1);
						if(isUpperLocal == false) isUpperLocal = true;
					} else {
						/* remove zero amplitude |1> state */
						hashid = (int)(i1 % QSTORE_PARTITION);
						delQList[hashid][tid].push_back(upperQ);
					}
				}
			}
		}

		if(isLowerLocal == true) isLower = true;
		if(isUpperLocal == true) isUpper = true;
	}

	/******************************************************** 
	 * (STEP2) remove zero amplitude states or add new state
	 ********************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<QReg->getCPUCores(); j++) {
			for(auto Q : delQList[i][j]) {
				QReg->eraseQState_nolock(Q->getIndex());
			}
			for(auto Q : addQList[i][j]) {
				QReg->setQState_nolock(Q->getIndex(), Q);
			}
			addQList[i][j].clear();
			delQList[i][j].clear();
		}
	}

	/************************************************** 
	 * (STEP3) update qubit type
	 **************************************************/
	if(isLower == true && isUpper == true) {
		QReg->qubitTypes[target] = KET_SUPERPOSED;
	} else if(isLower == true) {
			QReg->qubitTypes[target] = KET_ZERO;
	} else if(isUpper == true) {
		QReg->qubitTypes[target] = KET_ONE;
	}
}

static int NMC_Measure(QRegister *QReg, int qubit) 
{
	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		return -1;
	}

	if(QReg->qubitTypes[qubit] == KET_ZERO || QReg->qubitTypes[qubit] == KET_ONE) {
		/* do nothing */
		return QReg->qubitTypes[qubit];
	}

	double f = (double)(rand() % 100) / 100.0;
	double lpm[QReg->getCPUCores()];
	double upm[QReg->getCPUCores()];
	double lengthm[QReg->getCPUCores()];
	double lp = 0;
	double up = 0;
	int state;

	vector<QState*> delQList[QSTORE_PARTITION][MAX_CORES];
	
	for(int i=0; i<QReg->getCPUCores(); i++) {
		lpm[i] = upm[i] = lengthm[i] = 0;
	}

	/************************************************** 
	 * (STEP1) get probability of |0> and |1> 
	 **************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		int thread_id = omp_get_thread_num();
		int tid = thread_id % QReg->getCPUCores();
		QState *Q = NULL;

		QMAPITER it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			Q = it->second;
			if(stripe_lower(Q->getIndex(), qubit) == true) {
				lpm[tid] += norm(Q->getAmplitude());
			} else {
				upm[tid] += norm(Q->getAmplitude());
			}
		}
	}

	for(int i=0; i<QReg->getCPUCores(); i++) {
		lp += lpm[i];
		up += upm[i];
	}

	if(lp == 0 || up == 0) {
		/* already measured */
		if(lp > 0) {
			state = 0;
		} else {
			state = 1;
		}

		return state;
	}

	/************************************************** 
	 * (STEP2) set final state according to the probability
	 **************************************************/
	if(f < lp) {
		state = 0;      // collapsed to |0>
	} else {
		state = 1;      // collapsed to |1>
	}

	/************************************************** 
	 * (STEP3) get collapsed states
	 **************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		int thread_id = omp_get_thread_num();
		int tid = thread_id % QReg->getCPUCores();
		QState *Q = NULL;

		QMAPITER it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			Q = it->second;
			qsize_t qidx = Q->getIndex();
			int hashid;

			if((state == 0 && stripe_upper(qidx, qubit) == true) || 
			   (state == 1 && stripe_lower(qidx, qubit) == true)) {
				hashid = (int)(qidx % QSTORE_PARTITION);
				delQList[hashid][tid].push_back(Q);
			}
		}
	}

	/************************************************** 
	 * (STEP5) remove zero amplitude states 
	 **************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<QReg->getCPUCores(); j++) {
			for(auto Q : delQList[i][j]) {
				QReg->eraseQState_nolock(Q->getIndex());
			}
			delQList[i][j].clear();
		}
	}

	/************************************************** 
	 * (STEP6) Normalize amplitudes
	 **************************************************/
	QReg->normalize();

	/************************************************** 
	 * (STEP7) Revalidate qubit types
	 **************************************************/
	for(int i=0; i<QReg->getNumQubits(); i++) {
		QReg->qubitTypes[i] = QType(QReg, i);
	}

	return state;
}

static int NMC_MeasureF(QRegister *QReg, int qubit, int collapse) 
{
	if(QReg->qubitTypes[qubit] == KET_ZERO || QReg->qubitTypes[qubit] == KET_ONE) {
		/* do nothing */
		return QReg->qubitTypes[qubit];
	}

	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		exit(0);
	}

	vector<QState*> delQList[QSTORE_PARTITION][MAX_CORES];
	
	/************************************************** 
	 * (STEP1) get collapsed states according to argument
	 **************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		int thread_id = omp_get_thread_num();
		int tid = thread_id % QReg->getCPUCores();
		QState *Q = NULL;

		QMAPITER it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			Q = it->second;
			qsize_t qidx = Q->getIndex();
			int hashid;
	
			if((collapse == 0 && stripe_upper(qidx, qubit) == true) || 
			   (collapse == 1 && stripe_lower(qidx, qubit) == true)) {
				hashid = (int)(qidx % QSTORE_PARTITION);
				delQList[hashid][tid].push_back(Q);
			}
		}
	}

	/************************************************** 
	 * (STEP5) remove zero amplitude states 
	 **************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<QReg->getCPUCores(); j++) {
			for(auto Q : delQList[i][j]) {
				QReg->eraseQState_nolock(Q->getIndex());
			}
			delQList[i][j].clear();
		}
	}

	/************************************************** 
	 * (STEP4) Normalize amplitudes
	 **************************************************/
	QReg->normalize();

	/************************************************** 
	 * (STEP5) Revalidate qubit types
	 **************************************************/
	for(int i=0; i<QReg->getNumQubits(); i++) {
		QReg->qubitTypes[i] = QType(QReg, i);
	}

	return collapse;
}
