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
	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		exit(0);
	}

	size_t stage = QReg->incStage();

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

			/* check if already applied by other thread */
			if(QReg->checkStage(lower, upper, i0, stage) < 0) {
				/* just skip previously applied states */
				continue;
			}

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
			#if 0
				if(norm(amp0) > AMPLITUDE_EPS) {
			#else
				if(norm(amp0) > 0) {
			#endif
					lower->setAmplitude(amp0);
				} else {
					delQState[i].push_back(lower);
				}
			} else {
			#if 0
				if(norm(amp0) > AMPLITUDE_EPS) {
			#else
				if(norm(amp0) > 0) {
			#endif
					lower = new QState(i0, amp0);
			
					newQState[i].push_back(lower);
				}
			}

			if(upper) {
			#if 0
				if(norm(amp1) > AMPLITUDE_EPS) {
			#else
				if(norm(amp1) > 0) {
			#endif
					upper->setAmplitude(amp1);
				} else {
					delQState[i].push_back(upper);
				}
			} else {
			#if 0
				if(norm(amp1) > AMPLITUDE_EPS) {
			#else
				if(norm(amp1) > 0) {
			#endif
					upper = new QState(i1, amp1);
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
	if(target >= QReg->getNumQubits()) {
		printf("target(%d) out of range!\n", target);
		exit(0);
	}

	if(control >= QReg->getNumQubits()) {
		printf("control(%d) out of range!\n", control);
		exit(0);
	}

	size_t stage = QReg->incStage();

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

			/* check if already applied by other thread */
			if(QReg->checkStage(lower, upper, i0, stage) < 0) {
				/* just skip previously applied states */
				continue;
			}

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
					lower = new QState(i0, amp0);
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
					upper = new QState(i1, amp1);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
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
	applyMatrix(QReg, qubit, M);
	timer.end();
	QReg->updateQRegStat(QGATE_P, timer);
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
	applyControlledMatrix(QReg, control, target, M);
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
	applyControlledMatrix(QReg, control, target, M);
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
	applyControlledMatrix(QReg, control, target, M);
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
	applyControlledMatrix(QReg, control, target, M);
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
	applyControlledMatrix(QReg, control, target, M);
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
	applyControlledMatrix(QReg, control, target, M);
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
	applyControlledMatrix(QReg, control, target, M);
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
	applyControlledMatrix(QReg, control, target, M);
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
				upper = new QState(i1, Q->getAmplitude());
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
int M(QRegister *QReg, int qubit) 
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

	timer.end();
	QReg->updateQRegStat(QGATE_MEASURE, timer);

	return state;
}

int MV(QRegister *QReg, int qubit) {
	int mv = M(QReg, qubit);

	return mv == 0 ? 1 : -1;
}

int MF(QRegister *QReg, int qubit, int collapse) 
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

int MNOP(QRegister *QReg, int qubit) 
{
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

	return state;
}

/*
 * Estimate the quantum state int four types below.
 * 
 *   - KET_ZERO  : |0>
 *   - KET_ONE   : |1>
 *   - KET_PLUS  : |+> = 0.7|0> + 0.7|1>
 *   - KET_MINUS : |-> = 0.7|0> - 0.7|1>
 *   - KET_SUPERPOSED : a|0> + b|1> or a|0> - b|1>
 */
int QType(QRegister *QReg, int qubit) 
{
	QRegister *QRegMask = new QRegister(QReg->getNumQubits());
	qsize_t mask = quantum_shiftL(1, qubit);
	QState *Q = NULL;
	int type = KET_UNKNOWN;
	complex_t amp_zero;
	complex_t amp_one;
	bool is_zero = false;
	bool is_one = false;

	QRegMask->clear();
	QReg->setOrderedQState();
	while((Q = QReg->getOrderedQState()) != NULL) {
		QState *newQ = NULL;
		qsize_t newIdx = 0;

		newIdx = Q->getIndex() & mask;
		newQ = QRegMask->findQState(newIdx);
		if(newQ == NULL) {
			newQ = new QState(newIdx, Q->getAmplitude());
			QRegMask->setQState(newIdx, newQ);

			if((newIdx & mask) == 0) {
				is_zero = true;
				amp_zero = newQ->getAmplitude();
			} else if((newIdx & mask) != 0) {
				is_one = true;
				amp_one = newQ->getAmplitude();
			}
		}
	}

	if(is_zero == true && is_one == true) {
		if(norm(amp_zero) == norm(amp_one)) {
			if((amp_zero.real() * amp_one.real()) > 0) {
				type = KET_PLUS;
			} else {
				type = KET_MINUS;
			}
		} else {
			type = KET_SUPERPOSED;
		}
	} else {
		if(is_zero == true) {
			type = KET_ZERO;
		} else {
			type = KET_ONE;
		}
	}

	delete QRegMask;

	return type;
}

char *QTypeStr(QRegister *QReg, int qubit)
{
	int type = QType(QReg, qubit);

	if(type == KET_ZERO) return "|0>"; 
	else if(type == KET_ONE) return "|1>"; 
	else if(type == KET_PLUS) return "|+>"; 
	else if(type == KET_MINUS) return "|->"; 
	else if(type == KET_SUPERPOSED) return "|S>";

	return "UNKNOWN";
}

/*
 * show probability of the qubit 
 */
double showQubitProb(QRegister *QReg, int qubit, int state) 
{
	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		exit(0);
	}

	double real = 0;
	double imag = 0;
	double length = 0;
	complex_t lo = 0;
	complex_t up = 0;

	std::map<qsize_t, QState*>::iterator it[QSTORE_PARTITION];
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(it[i] = QReg->qstore[i].begin(); it[i] != QReg->qstore[i].end(); it[i]++) {
			QState *Q = it[i]->second;
			real = Q->getAmplitude().real();
			imag = Q->getAmplitude().imag();
			if(stripe_lower(Q->getIndex(), qubit) == true) {
				lo += complex_t(abs(real), abs(imag));
			} else {
				up += complex_t(abs(real), abs(imag));
			}
		}
	}

	/* normalize */
	length = (norm(lo) + norm(up));
	length = std::sqrt(length);

	// printf("norm(lo)=%f, norm(up)=%f\n", norm(lo), norm(up));

	lo = lo / length;
	up = up / length;

#if 0
	/* print */
	printf("Qubit-%d", qubit);
	printf("%5s [P=%.6f] [%.6f, %.6f] |0>\n", " ", norm(lo), abs(lo.real()), abs(lo.imag()));
	printf("%13s [P=%.6f] [%.6f, %.6f] |1>\n", " ", norm(up), abs(up.real()), abs(up.imag()));
	printf("\n");
#endif

	if(state == KET_ZERO) {
		return norm(lo);
	} else if(state == KET_ONE) {
		return norm(up);
	}

	return 0;
}

/*
 * show relations of all qubits
 */
void __estimation_entanglement(QRegister *QReg, std::vector<int> Qubits, std::vector<pair<int, int>>& entanglePair)
{
	QRegister *QRegMask = new QRegister(QReg->getNumQubits());
	int target_qubits = Qubits.size();
	QState *Q = NULL;

	for(int i=0; i<target_qubits; i++) {
		if(QType(QReg, Qubits[i]) == KET_ZERO || QType(QReg, Qubits[i]) == KET_ONE) {
			continue;
		}		

		for(int j=i+1; j<target_qubits; j++) {
			if(QType(QReg, Qubits[j]) == KET_ZERO || QType(QReg, Qubits[j]) == KET_ONE) {
				continue;
			}

			int Q1 = i;
			int Q2 = j;
			qsize_t maskQ1 = quantum_shiftL(1, Q1);
			qsize_t maskQ2 = quantum_shiftL(1, Q2);
			qsize_t mask = maskQ1 | maskQ2;
			complex_t amp[4];
			int ampPos = 0;

			QRegMask->clear();
			QReg->setOrderedQState();
			while((Q = QReg->getOrderedQState()) != NULL) {
				QState *newQ = NULL;
				qsize_t newIdx = Q->getIndex() & mask;

				newQ = QRegMask->findQState(newIdx);
				if(newQ == NULL) {
					newQ = new QState(newIdx, Q->getAmplitude());
					QRegMask->setQState(newIdx, newQ);

					amp[ampPos++] = Q->getAmplitude();
				}
			}

			if(QRegMask->getNumStates() != 2) {
				complex_t M[4];
				M[0] = amp[0];
				M[1] = amp[1];
				M[2] = amp[2];
				M[3] = amp[3];
				if(getSchmidtNumber(M) == 1) {
					continue;
				}
			}

			entanglePair.push_back(make_pair(Q1, Q2));
		}
	}

	delete QRegMask;
}

void __showQubitRelation(QRegister *QReg, std::vector<std::vector<int>>& entangleList, bool print) 
{
	std::vector<int> candidateQubits;
	std::vector<pair<int, int>> entanglePair;
	int qubits = QReg->getNumQubits();
	QState *Q = NULL;

	/***********************************/
	/* STEP1: initialize candidate map */
	/***********************************/
	for(int i=0; i<qubits; i++) {
		if(QType(QReg, i) == KET_ZERO || QType(QReg, i) == KET_ONE) {
			continue;
		}
		candidateQubits.push_back(i);
	}

	/******************************************************/
	/* STEP2: build initial entangle map of global states */
	/******************************************************/
	__estimation_entanglement(QReg, candidateQubits, entanglePair);

	/*****************************************************/
	/* STEP3: re-build nested entangle of partial states */
	/*****************************************************/
	for(auto qubit : candidateQubits) {
		QRegister *QRegClone = new QRegister(QReg);

		M(QRegClone, qubit);
		__estimation_entanglement(QRegClone, candidateQubits, entanglePair);

		delete QRegClone;
	}

	/************************************/
	/* STEP4: build global entangle map */
	/************************************/
	int eid = 0;
	int emap[qubits];
	for(int i=0; i<qubits; i++) {
		emap[i] = -1;
	}

	for(auto ePair : entanglePair) {
		int Q1 = ePair.first;
		int Q2 = ePair.second;

		// printf("Q%d - Q%d\n", Q1, Q2);

		if(emap[Q1] == -1 && emap[Q2] == -1) {
			emap[Q1] = emap[Q2] = eid++;
		} else if(emap[Q1] != -1 && emap[Q2] == -1) {
			emap[Q2] = emap[Q1];
		} else if(emap[Q1] == -1 && emap[Q2] != -1) {
			emap[Q1] = emap[Q2];
		} else if(emap[Q1] != -1 && emap[Q2] != -1) {
			if(emap[Q1] != emap[Q2]) {
				int newEid = emap[Q1];
				int oldEid = emap[Q2];
				for(int i=0; i<qubits; i++) {
					if(emap[i] == oldEid) {
						emap[i] = newEid;
					}
				}
			}
		}
	}

	/*****************************************/
	/* STEP5: print global entangle relation */
	/*****************************************/
	if(print == true) {
		printf("total qubits   = %d\n", qubits);
		printf("tensor product = ");
		for(int i=0; i<qubits; i++) {
			if(emap[i] == -1) {
				printf("%d ", i);
			}
		}
		cout << endl;
	}

	if(print == true) {
		printf("entangle pair  = %d\n", eid);
	}
	int newEid = 0;
	for(int i=0; i<eid; i++) {
		std::vector<int> eList;
		int equbits = 0;
		for(int j=0; j<qubits; j++) {
			if(emap[j] == i) {
				equbits++;
			}
		}

		if(equbits == 0) {
			continue;
		}

		if(print == true) {
			printf("- #%d [%d]: ", newEid++, equbits);
		}
		for(int j=0; j<qubits; j++) {
			if(emap[j] != i) {
				continue;
			}
			if(print == true) {
				printf("%d ", j);
			}
			eList.push_back(j);
		}
		if(print == true) {
			printf("\n");
		}
		entangleList.push_back(eList);
	}
}

void showQubitRelation(QRegister *QReg)
{
    std::vector<std::vector<int>> entangleList;
    __showQubitRelation(QReg, entangleList, true);
}

void showQubitRelation(QRegister *QReg, std::vector<std::vector<int>>& entangleList)
{
    __showQubitRelation(QReg, entangleList, false);
}
