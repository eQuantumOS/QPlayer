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

using namespace std;

/* 
 * phase factoring : copied from qx-simulator 
 */
/*
static void reset_gphase(complex_t M[])
{
	double n = norm(M[0]);

	for(int i=0; i<4; i++) cout << i << " : " << M[i] << endl;

	if (n > 10e-9) {
		complex_t p(M[0].real()/n,M[0].imag()/n);
		M[0] /= p;
		M[1] /= p;
		M[2] /= p;
		M[3] /= p;
		cout << "n : " << n << endl;
		cout << "p : " << p << endl;
	} else {
		n = norm(M[1]);
		complex_t p(M[1].real()/n,M[1].imag()/n);
		cout << "n : " << n << endl;
		cout << "p : " << p << endl;
		M[0] /= p;
		M[1] /= p;
		M[2] /= p;
		M[3] /= p;
	}

	double n1 = std::sqrt(norm(M[0])+norm(M[1]));
	double n2 = std::sqrt(norm(M[2])+norm(M[3]));
	M[0] /= n1;
	M[1] /= n2;
	M[2] /= n1;
	M[3] /= n2;

	printf("\n");
	for(int i=0; i<4; i++) cout << i << " : " << M[i] << endl;
}
*/

/*
 * Apply 1-qubit matrix operations to quantum states. 
 */
static void applyMatrix(QRegister *QReg, int qubit, complex_t M[])
{
	QReg->checkMemory();
	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		exit(0);
	}

#ifndef ENABLE_NMC
	size_t stage = QReg->incStage();
#endif

	qsize_t stride = quantum_shiftL(1, (qsize_t)qubit);

	complex_t m00 = M[0];
	complex_t m01 = M[1];
	complex_t m10 = M[2];
	complex_t m11 = M[3];

	/*
	 * We apply the matrix by traversing the qstores of all quantum registers.
	 */
	std::vector<QState *> newQState[QSTORE_PARTITION];
	std::vector<QState *> delQState[QSTORE_PARTITION];

	/* 
	 * (STEP1) apply matrix
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		std::map<qsize_t, QState*>::iterator it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			QState *Q = it->second;
			QState *lower = NULL;
			QState *upper = NULL;
			complex_t amp0, amp1;
			complex_t in0, in1;
			qsize_t i0, i1;

			i0 = Q->getIndex();

			if(stripe_lower(i0, qubit) == true) {
				/* lower bound of this stripe */
				i1 = i0 + stride;

				lower = Q;
				upper = QReg->findQState(i1);
			} else {
				/* upper bound of this stripe */
				i1 = i0;
				i0 = i1 - stride;

				lower = QReg->findQState(i0);
				upper = Q;
			}

		#ifndef ENABLE_NMC
			/* check if already applied by other thread */
			if(QReg->checkStage(lower, upper, i0, stage) < 0) {
				/* just skip previously applied states */
				continue;
			}
		#endif

			if(lower == NULL) {
				in0 = 0.0;
			} else {
				in0 = lower->getAmplitude();
			}

			if(upper == NULL) {
				in1 = 0.0;
			} else {
				in1 = upper->getAmplitude();
			}
	
			amp0 = m00*in0+m01*in1;
			amp1 = m10*in0+m11*in1;

			if(lower) {
			#if 1
				if(norm(amp0) > AMPLITUDE_EPS) {
			#else
				if(norm(amp0) > 0) {
			#endif
					lower->setAmplitude(amp0);
				} else {
					delQState[i].push_back(lower);
				}
			} else {
			#if 1
				if(norm(amp0) > AMPLITUDE_EPS) {
			#else
				if(norm(amp0) > 0) {
			#endif
					lower = getQState(i0, amp0);
			
					newQState[i].push_back(lower);
				}
			}

			if(upper) {
			#if 1
				if(norm(amp1) > AMPLITUDE_EPS) {
			#else
				if(norm(amp1) > 0) {
			#endif
					upper->setAmplitude(amp1);
				} else {
					delQState[i].push_back(upper);
				}
			} else {
			#if 1
				if(norm(amp1) > AMPLITUDE_EPS) {
			#else
				if(norm(amp1) > 0) {
			#endif
					upper = getQState(i1, amp1);
					newQState[i].push_back(upper);
				}
			}
		}
	}

	/* 
	 * (STEP2) remove zero amplitude states 
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<delQState[i].size(); j++) {
			QState *Q = delQState[i][j];
			QReg->eraseQState(Q->getIndex());
		}
		delQState[i].clear();
	}

	/* 
	 * (STEP3) add new non-zero amplitude states
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<newQState[i].size(); j++) {
			QState *Q = newQState[i][j];
			QReg->setQState(Q->getIndex(), Q);
		}
		newQState[i].clear();
	}
}

/*
 * Apply controlled matrix operations to quantum states. 
 */
static void applyControlledMatrix(QRegister *QReg, int control, int target, complex_t M[])
{
	QReg->checkMemory();
	if(target >= QReg->getNumQubits()) {
		printf("target(%d) out of range!\n", target);
		exit(0);
	}

	if(control >= QReg->getNumQubits()) {
		printf("control(%d) out of range!\n", control);
		exit(0);
	}

#ifndef ENABLE_NMC
	size_t stage = QReg->incStage();
#endif

	qsize_t stride = quantum_shiftL(1, (qsize_t)target);

	complex_t m00 = M[0];
	complex_t m01 = M[1];
	complex_t m10 = M[2];
	complex_t m11 = M[3];

	/*
	 * We apply the matrix by traversing the qstores of all quantum registers.
	 */
	std::vector<QState *> newQState[QSTORE_PARTITION];
	std::vector<QState *> delQState[QSTORE_PARTITION];

	/* 
	 * (STEP1) apply matrix
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		std::map<qsize_t, QState*>::iterator it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			QState *Q = it->second;
			QState *lower = NULL;
			QState *upper = NULL;
			complex_t amp0, amp1;
			complex_t in0, in1;
			qsize_t i0, i1;

			i0 = Q->getIndex();

			if(stripe_lower(Q->getIndex(), control) == true) {
				/* control is |0> */
				continue;
			}

			if(stripe_lower(i0, target) == true) {
				/* lower bound of this stripe */
				i1 = i0 + stride;

				lower = Q;
				upper = QReg->findQState(i1);
			} else {
				/* upper bound of this stripe */
				i1 = i0;
				i0 = i1 - stride;

				lower = QReg->findQState(i0);
				upper = Q;
			}

		#ifndef ENABLE_NMC
			/* check if already applied by other thread */
			if(QReg->checkStage(lower, upper, i0, stage) < 0) {
				/* just skip previously applied states */
				continue;
			}
		#endif

			if(lower == NULL) {
				in0 = 0.0;
			} else {
				in0 = lower->getAmplitude();
			}

			if(upper == NULL) {
				in1 = 0.0;
			} else {
				in1 = upper->getAmplitude();
			}
	
			amp0 = m00*in0+m01*in1;
			amp1 = m10*in0+m11*in1;
	
			if(lower) {
				if(norm(amp0) > AMPLITUDE_EPS) {
					lower->setAmplitude(amp0);
				} else {
					delQState[i].push_back(lower);
				}
			} else {
				if(norm(amp0) > AMPLITUDE_EPS) {
					lower = getQState(i0, amp0);
					newQState[i].push_back(lower);
				}
			}

			if(upper) {
				if(norm(amp1) > AMPLITUDE_EPS) {
					upper->setAmplitude(amp1);
				} else {
					delQState[i].push_back(upper);
				}
			} else {
				if(norm(amp1) > AMPLITUDE_EPS) {
					upper = getQState(i1, amp1);
					newQState[i].push_back(upper);
				}
			}
		}
	}

	/* 
	 * (STEP2) remove zero amplitude states 
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<delQState[i].size(); j++) {
			QState *Q = delQState[i][j];
			QReg->eraseQState(Q->getIndex());
		}
		delQState[i].clear();
	}

	/* 
	 * (STEP3) add new non-zero amplitude states
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<newQState[i].size(); j++) {
			QState *Q = newQState[i][j];
			QReg->setQState(Q->getIndex(), Q);
		}
		newQState[i].clear();
	}
}

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
	applyMatrix(QReg, qubit, M);
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
#ifdef ENABLE_NMC
	NMC_ToggleGates(QReg, qubit, QGATE_X, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_DiagonalGates(QReg, qubit, QGATE_Z, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_ToggleGates(QReg, qubit, QGATE_Y, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_H, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
	timer.end();
	QReg->updateQRegStat(QGATE_H, timer);
}

void U1(QRegister *QReg, int qubit, double angle)
{
	QTimer timer;
	complex_t M[] = {
		complex_t(1, 0), complex_t(0, 0),
		complex_t(0, 0), complex_t(cos(angle), sin(angle))
	};

	timer.start();
#ifdef ENABLE_NMC
	NMC_DiagonalGates(QReg, qubit, QGATE_U1, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
	timer.end();
	QReg->updateQRegStat(QGATE_U1, timer);
}

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
#ifdef ENABLE_NMC
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_U2, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
	timer.end();
	QReg->updateQRegStat(QGATE_U2, timer);
}

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
#ifdef ENABLE_NMC
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_U3, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_DiagonalGates(QReg, qubit, QGATE_S, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_DiagonalGates(QReg, qubit, QGATE_T, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_DiagonalGates(QReg, qubit, QGATE_SDG, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_DiagonalGates(QReg, qubit, QGATE_TDG, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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

#if 0
	reset_gphase(M);
#endif
	timer.start();
#ifdef ENABLE_NMC
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_RX, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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

#if 0
	reset_gphase(M);
#endif
	timer.start();
#ifdef ENABLE_NMC
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_RY, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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

#if 0
	reset_gphase(M);
#endif
	timer.start();
#ifdef ENABLE_NMC
	NMC_DiagonalGates(QReg, qubit, QGATE_RZ, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_DiagonalGates(QReg, qubit, QGATE_P, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_NoneDiagonalGates(QReg, qubit, QGATE_SX, M);
#else
	applyMatrix(QReg, qubit, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_DiagonalControlGates(QReg, control, target, QGATE_CU1, M);
#else
	applyControlledMatrix(QReg, control, target, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_NoneDiagonalControlGates(QReg, control, target, QGATE_CU2, M);
#else
	applyControlledMatrix(QReg, control, target, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_NoneDiagonalControlGates(QReg, control, target, QGATE_CU3, M);
#else
	applyControlledMatrix(QReg, control, target, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_NoneDiagonalControlGates(QReg, control, target, QGATE_CH, M);
#else
	applyControlledMatrix(QReg, control, target, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_NoneDiagonalControlGates(QReg, control, target, QGATE_CX, M);
#else
	applyControlledMatrix(QReg, control, target, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_NoneDiagonalControlGates(QReg, control, target, QGATE_CY, M);
#else
	applyControlledMatrix(QReg, control, target, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_DiagonalControlGates(QReg, control, target, QGATE_CZ, M);
#else
	applyControlledMatrix(QReg, control, target, M);
#endif
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
#ifdef ENABLE_NMC
	NMC_DiagonalControlGates(QReg, control, target, QGATE_CRZ, M);
#else
	applyControlledMatrix(QReg, control, target, M);
#endif
	timer.end();
	QReg->updateQRegStat(QGATE_CRZ, timer);
}

/* 
 * Apply Toffoli gate
 */
void CCX(QRegister *QReg, int control1, int control2, int target) 
{
	QTimer timer;
	timer.start();

	if(control1 >= QReg->getNumQubits()) {
		printf("control1 qubit(%d) out of range!\n", control1);
		exit(0);
	}

	if(control2 >= QReg->getNumQubits()) {
		printf("control2 qubit(%d) out of range!\n", control2);
		exit(0);
	}

	if(target >= QReg->getNumQubits()) {
		printf("target qubit(%d) out of range!\n", target);
		exit(0);
	}

	if(control1 == target || control2 == target) {
		printf("CCNOT does not allow the same qubit of control & target\n");
		exit(0);
	}

	std::vector<QState *> newQState[QSTORE_PARTITION];
	std::vector<QState *> delQState[QSTORE_PARTITION];

	/* 
	 * (STEP1) CCNOT operation
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		std::map<qsize_t, QState*>::iterator it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			QState *Q = it->second;
			QState *lower = NULL;
			QState *upper = NULL;
			complex_t amp0, amp1;
			qsize_t i0, i1;
	
			i0 = Q->getIndex();
			i1 = i0;

			if(stripe_lower(Q->getIndex(), control1) == true) {
				/* control1 is |0> */
				continue;
			}

			if(stripe_lower(Q->getIndex(), control2) == true) {
				/* control1 is |0> */
				continue;
			}

			if(stripe_lower(i0, target) == true) {
				i1 = i0 + quantum_shiftL(1, (qsize_t)target);
			} else {
				i1 = i0 - quantum_shiftL(1, (qsize_t)target);
			}

			lower = QReg->findQState(i0);
			upper = QReg->findQState(i1);
			if(upper == NULL) {
				/* 
				 *  pair state does not exist, just move state index 
				 *  +--+       +--+
				 *  |00|  -->  |01|
				 *  +--+       +--+
				 */
				upper = getQState(i1, Q->getAmplitude());
				delQState[i].push_back(lower);
				newQState[i].push_back(upper);
			} else {
				/* 
				 *    swap two entry
				 *  +--+         +--+
				 *  |00| -- -->  |00|
				 *  +--+   -     +--+
				 *  |01| -- -->  |01|
				 *  +--+         +--+
				 */
				lower->setIndex(i1);
				upper->setIndex(i0);
				newQState[i].push_back(lower);
				newQState[i].push_back(upper);
			}
		}
	}

	/* 
	 * (STEP2) remove zero amplitude states after CCNOT 
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<delQState[i].size(); j++) {
			QState *Q = delQState[i][j];
			QReg->eraseQState(Q->getIndex());
		}
		delQState[i].clear();
	}

	/* 
	 * (STEP3) add non-zero amplitude states after CCNOT 
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<newQState[i].size(); j++) {
			QState *Q = newQState[i][j];
			QReg->setQState(Q->getIndex(), Q);
		}
		newQState[i].clear();
	}

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
 * Apply SWAP-gate to the qubit
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
 * The quantum state is measured on a Z basis. After measurement, 
 * the quantum state collapses to a state of |0> or |1>.
 * If you want to measure on the basis of X, you must apply 
 * the H operation in advance.
 *
 * The return value is 0 or 1, not +1 or -1. Note that if it 
 * collapses with |0>, it returns 0, and if it collapses with |1>, 
 * it returns 1.
 */
static int Measure(QRegister *QReg, int qubit) 
{
	QTimer timer;
	timer.start();

	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		return -1;
	}

	double f = (double)(rand() % 100) / 100.0;
	double lpm[QSTORE_PARTITION];
	double upm[QSTORE_PARTITION];
	double lengthm[QSTORE_PARTITION];
	double lp = 0;
	double up = 0;
	double length = 0;
	int state;

	for(int i=0; i<QSTORE_PARTITION; i++) {
		lpm[i] = upm[i] = lengthm[i] = 0;
	}

	/* 
	 * (STEP1) Calculate the amplitude according to the |0> or |1>, respectively.
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		std::map<qsize_t, QState*>::iterator it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			QState *Q = it->second;
			if(stripe_lower(Q->getIndex(), qubit) == true) {
				lpm[i] += norm(Q->getAmplitude());
			} else {
				upm[i] += norm(Q->getAmplitude());
			}
		}
	}

	for(int i=0; i<QSTORE_PARTITION; i++) {
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

		timer.end();
		QReg->updateQRegStat(QGATE_MEASURE, timer);

		return state;
	}

	/* 
	 * (STEP2) Determine final state according to the probability
	 */
	if(f < lp) {
		state = 0;      // collapsed to |0>
	} else {
		state = 1;      // collapsed to |1>
	}

	/* 
	 * (STEP3) Set zero-amplited after collapse.
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		std::map<qsize_t, QState*>::iterator it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			QState *Q = it->second;
			qsize_t i0 = Q->getIndex();
	
			if(state == 0 && stripe_upper(i0, qubit) == true) {
				Q->setAmplitude(complex_t(0, 0));
			} else if(state == 1 && stripe_lower(i0, qubit) == true) {
				Q->setAmplitude(complex_t(0, 0));
			}
		}
	}

	/* 
	 * (STEP4) Cleanup zero amplitude states
	 */
	QReg->clearZeroStates();

	/* 
	 * (STEP5) Normalize amplitudes
	 */
	QReg->normalize();

	timer.end();
	QReg->updateQRegStat(QGATE_MEASURE, timer);

	return state;
}

static int MeasureF(QRegister *QReg, int qubit, int collapse) 
{
	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		exit(0);
	}

	double f = (double)(rand() % 100) / 100.0;
	double lpm[QSTORE_PARTITION];
	double upm[QSTORE_PARTITION];
	double lengthm[QSTORE_PARTITION];
	double lp = 0;
	double up = 0;
	double length = 0;
	int state;

	for(int i=0; i<QSTORE_PARTITION; i++) {
		lpm[i] = upm[i] = lengthm[i] = 0;
	}

	/* 
	 * (STEP1) Calculate the amplitude according to the |0> or |1>, respectively.
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		std::map<qsize_t, QState*>::iterator it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			QState *Q = it->second;
			if(stripe_lower(Q->getIndex(), qubit) == true) {
				lpm[i] += norm(Q->getAmplitude());
			} else {
				upm[i] += norm(Q->getAmplitude());
			}
		}
	}

	for(int i=0; i<QSTORE_PARTITION; i++) {
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

	/* 
	 * (STEP2) Determine final state according to the probability
	 */
	state = collapse;

	/* 
	 * (STEP3) Set zero-amplited after collapse.
	 */
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		std::map<qsize_t, QState*>::iterator it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			QState *Q = it->second;
			qsize_t i0 = Q->getIndex();
	
			if(state == 0 && stripe_upper(i0, qubit) == true) {
				Q->setAmplitude(0);
			} else if(state == 1 && stripe_lower(i0, qubit) == true) {
				Q->setAmplitude(0);
			}
		}
	}

	/* 
	 * (STEP4) Cleanup zero amplitude states
	 */
	QReg->clearZeroStates();

	/* 
	 * (STEP5) Normalize amplitudes
	 */
	QReg->normalize();

	return state;
}

int M(QRegister *QReg, int qubit)
{
#ifdef GATE_DEBUG
	printf("M(%d)\n", qubit);
#endif

	QTimer timer;
	int val = 0;

	timer.start();
#ifdef ENABLE_NMC
	val = NMC_Measure(QReg, qubit);
#else
	val = Measure(QReg, qubit);
#endif
	timer.end();
	QReg->updateQRegStat(QGATE_MEASURE, timer);

#ifdef STATE_DEBUG
	if(checkQType(QReg) < 0) {
		printf("M(%d) may be causing malfunction..\n", qubit);
		exit(0);
	}
#endif

	return val;
}

int MF(QRegister *QReg, int qubit, int collapse)
{
#ifdef GATE_DEBUG
	printf("M(%d)\n", qubit);
#endif

	QTimer timer;
	int val = 0;

	timer.start();
#ifdef ENABLE_NMC
	val = NMC_MeasureF(QReg, qubit, collapse);
#else
	val = MeasureF(QReg, qubit, collapse);
#endif
	timer.end();
	QReg->updateQRegStat(QGATE_MEASURE, timer);

#ifdef STATE_DEBUG
	if(checkQType(QReg) < 0) {
		printf("MF(%d) may be causing malfunction..\n", qubit);
		exit(0);
	}
#endif

	return val;
}

int MV(QRegister *QReg, int qubit) {
	int mv = M(QReg, qubit);

	return mv == 0 ? 1 : -1;
}

