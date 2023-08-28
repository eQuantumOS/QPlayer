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

#include <omp.h>

#include "register.h"
#include "gate.h"
#include "dump.h"
#include "experimental.h"

using namespace std;

static void set_affinity(int core)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(core, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

static bool isRealizedState(complex_t amp)
{
#if 1
	if(norm(amp) > AMPLITUDE_EPS) {
		return true;
	} else {
		return false;
	}
#else
	if(abs(amp.real()) > AMPLITUDE_EPS || abs(amp.imag()) > AMPLITUDE_EPS) {
		return true;
	} else {
		return false;
	}
#endif
}

/*
 * Z, S, T, SDG, TDG, RZ, U1 
 */
void NMC_DiagonalGates(QRegister *QReg, int qubit, int gtype, complex_t M[])
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

		std::map<qsize_t, QState*>::iterator it;
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

/*
 * H, U2, U3, RX, RY, SX 
 */
void NMC_NoneDiagonalGates(QRegister *QReg, int qubit, int gtype, complex_t M[])
{
	QReg->checkMemory();

	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		exit(0);
	}

	qsize_t stride = quantum_shiftL(1, (qsize_t)qubit);
	int qubitType = QReg->qubitTypes[qubit];
	
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

		std::map<qsize_t, QState*>::iterator it;
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
					QReg->delQList[hashid][tid].push_back(Q);
				}

				/* update upper state */
				if(isRealizedState(newAmp1) == true) {
					/* add new |1> state */
					upperQ = getQState(i1, newAmp1);
					hashid = (int)(i1 % QSTORE_PARTITION);
					QReg->addQList[hashid][tid].push_back(upperQ);
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
					QReg->addQList[hashid][tid].push_back(lowerQ);
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
					QReg->delQList[hashid][tid].push_back(Q);
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
						QReg->delQList[hashid][tid].push_back(lowerQ);
					}

					if(isRealizedState(newAmp1) == true) {
						upperQ->setAmplitude(newAmp1);
						isUpperLocal = true;
					} else {
						/* remove zero amplitude |1> state */
						hashid = (int)(i1 % QSTORE_PARTITION);
						QReg->delQList[hashid][tid].push_back(upperQ);
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
						QReg->delQList[hashid][tid].push_back(lowerQ);
					}

					/* update upper state */
					if(isRealizedState(newAmp1) == true) {
						/* add new upper state */
						upperQ = getQState(i1, newAmp1);
						hashid = (int)(i1 % QSTORE_PARTITION);
						QReg->addQList[hashid][tid].push_back(upperQ);
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
						QReg->addQList[hashid][tid].push_back(lowerQ);
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
						QReg->delQList[hashid][tid].push_back(upperQ);
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
			for(auto Q : QReg->delQList[i][j]) {
				QReg->eraseQState_nolock(Q->getIndex());
			}
			for(auto Q : QReg->addQList[i][j]) {
				QReg->setQState_nolock(Q->getIndex(), Q);
			}
			QReg->delQList[i][j].clear();
			QReg->addQList[i][j].clear();
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

/*
 * X, Y
 */
void NMC_ToggleGates(QRegister *QReg, int qubit, int gtype, complex_t M[])
{
	QReg->checkMemory();

	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		exit(0);
	}

	qsize_t stride = quantum_shiftL(1, (qsize_t)qubit);
	int qubitType = QReg->qubitTypes[qubit];

	/************************************************** 
	 * (STEP1) evolve quantum states 
	 **************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		int thread_id = omp_get_thread_num();
		int tid = thread_id % QReg->getCPUCores();
		cpu_set_t cpuset;
		QState *Q = NULL;
		QState *newQ = NULL;
		QState *lowerQ = NULL;
		QState *upperQ = NULL;
		qsize_t qidx;
		qsize_t newQidx;
		qsize_t i0, i1;
		complex_t amp;
		complex_t newAmp;
		complex_t newAmp0;
		complex_t newAmp1;
		int hashid;

		std::map<qsize_t, QState*>::iterator it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			Q = it->second;
			amp = Q->getAmplitude();
			qidx = Q->getIndex();

			if(qubitType == KET_ZERO) {
				/************************************************* 
				 * (Scenario#1) Qubit is in |0> state.
				 *  - We just move state to |1>    
				 *************************************************/
				newQidx = qidx + stride;
				newAmp = amp * M[2];

				 /* remove state from old index */
				hashid = (int)(qidx % QSTORE_PARTITION);
				QReg->delQList[hashid][tid].push_back(Q);

				/* new state to new index */
				newQ = getQState(newQidx, newAmp);
				hashid = (int)(newQidx % QSTORE_PARTITION);
				QReg->addQList[hashid][tid].push_back(newQ);
			} else if(qubitType == KET_ONE) {
				/************************************************* 
				 * (Scenario#2) Qubit is in |1> state.
				 *  - We just move state to |0>    
				 *************************************************/
				newQidx = qidx - stride;
				newAmp = amp * M[1];

				/* new state to new index */
				newQ = getQState(newQidx, newAmp);
				hashid = (int)(newQidx % QSTORE_PARTITION);
				QReg->addQList[hashid][tid].push_back(newQ);

				/* remove state from old index */
				hashid = (int)(qidx % QSTORE_PARTITION);
				QReg->delQList[hashid][tid].push_back(Q);
			} else {
				/************************************************* 
				 * (Scenario#3) Qubit is in superposed state    
				 *
				 * This is divided into two sub-scenarios 
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

				if(lowerQ == NULL || upperQ == NULL) {
					/******************************************
					 * (Sub1) matrix pair does not exist             
				 	 *  - just move state between |0> and |1> 
				 	 *     a|0> --> a|1>  or  b|1> --> b|0> 
				 	 ******************************************/
					newAmp = Q->getAmplitude();

					if(lowerQ == NULL) {
						newQidx = i0;
						newAmp *= M[1];
					} else {
						newQidx = i1;
						newAmp *= M[2];
					}

					/* remove old state */
					hashid = (int)(qidx % QSTORE_PARTITION);
					QReg->delQList[hashid][tid].push_back(Q);

					/* add new state */
					newQ = getQState(newQidx, newAmp);
					hashid = (int)(newQidx % QSTORE_PARTITION);
					QReg->addQList[hashid][tid].push_back(newQ);
				} else {
					/******************************************
					 * (Sub2) matrix pair exist             
					 *  - just swap two amplitudes of states         
				 	 *     a|0> --> b|0>  
					 *     b|1> --> a|1> 
				 	 ******************************************/
					if(lowerQ->getIndex() != qidx) {
						/* (Important!!) To ensure concurrency when all matrix pairs are
						   exist, only the thread assigned lowerQ updates the state. */
						continue;
					}

					newAmp0 = upperQ->getAmplitude() * M[1];
					newAmp1 = lowerQ->getAmplitude() * M[2];

					lowerQ->setAmplitude(newAmp0);
					upperQ->setAmplitude(newAmp1);
				}
			}
		}
	}

	/******************************************************** 
	 * (STEP3) remove zero amplitude states or add new state
	 ********************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<QReg->getCPUCores(); j++) {
			for(auto Q : QReg->delQList[i][j]) {
				QReg->eraseQState_nolock(Q->getIndex());
			}
			for(auto Q : QReg->addQList[i][j]) {
				QReg->setQState_nolock(Q->getIndex(), Q);
			}
			QReg->delQList[i][j].clear();
			QReg->addQList[i][j].clear();
		}
	}

	/************************************************** 
	 * (STEP4) update qubit type
	 **************************************************/
	if(qubitType == KET_ZERO) {
		QReg->qubitTypes[qubit] = KET_ONE;
	} else if(qubitType == KET_ONE) {
		QReg->qubitTypes[qubit] = KET_ZERO;
	} else {
		QReg->qubitTypes[qubit] = KET_SUPERPOSED;
	}
}

/*
 * CZ, CU1, CRZ
 */
void NMC_DiagonalControlGates(QRegister *QReg, int control, int target, int gtype, complex_t M[])
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

		std::map<qsize_t, QState*>::iterator it;
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

/*
 * CX, CY, CU2, CU3, CH, CSWAP
 */
void NMC_NoneDiagonalControlGates(QRegister *QReg, int control, int target, int gtype, complex_t M[])
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

		std::map<qsize_t, QState*>::iterator it;
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
					QReg->delQList[hashid][tid].push_back(Q);
				}

				/* update upper state */
				if(isRealizedState(newAmp1) == true) {
					/* add new |1> state */
					upperQ = getQState(i1, newAmp1);
					hashid = (int)(i1 % QSTORE_PARTITION);
					QReg->addQList[hashid][tid].push_back(upperQ);
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
					QReg->addQList[hashid][tid].push_back(lowerQ);
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
					QReg->delQList[hashid][tid].push_back(Q);
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
						QReg->delQList[hashid][tid].push_back(lowerQ);
					}

					if(isRealizedState(newAmp1) == true) {
						upperQ->setAmplitude(newAmp1);
						isUpperLocal = true;
					} else {
						/* remove zero amplitude |1> state */
						hashid = (int)(i1 % QSTORE_PARTITION);
						QReg->delQList[hashid][tid].push_back(upperQ);
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
						QReg->delQList[hashid][tid].push_back(lowerQ);
					}

					/* update upper state */
					if(isRealizedState(newAmp1) == true) {
						/* add new upper state */
						upperQ = getQState(i1, newAmp1);
						hashid = (int)(i1 % QSTORE_PARTITION);
						QReg->addQList[hashid][tid].push_back(upperQ);
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
						QReg->addQList[hashid][tid].push_back(lowerQ);
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
						QReg->delQList[hashid][tid].push_back(upperQ);
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
			for(auto Q : QReg->delQList[i][j]) {
				QReg->eraseQState_nolock(Q->getIndex());
			}
			for(auto Q : QReg->addQList[i][j]) {
				QReg->setQState_nolock(Q->getIndex(), Q);
			}
			QReg->delQList[i][j].clear();
			QReg->addQList[i][j].clear();
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
int NMC_Measure(QRegister *QReg, int qubit) 
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

		std::map<qsize_t, QState*>::iterator it;
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

		std::map<qsize_t, QState*>::iterator it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			Q = it->second;
			qsize_t qidx = Q->getIndex();
			int hashid;

			if((state == 0 && stripe_upper(qidx, qubit) == true) || 
			   (state == 1 && stripe_lower(qidx, qubit) == true)) {
				hashid = (int)(qidx % QSTORE_PARTITION);
				QReg->delQList[hashid][tid].push_back(Q);
			}
		}
	}

	/************************************************** 
	 * (STEP5) remove zero amplitude states 
	 **************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<QReg->getCPUCores(); j++) {
			for(auto Q : QReg->delQList[i][j]) {
				QReg->eraseQState_nolock(Q->getIndex());
			}
			QReg->delQList[i][j].clear();
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

int NMC_MeasureF(QRegister *QReg, int qubit, int collapse) 
{
	if(QReg->qubitTypes[qubit] == KET_ZERO || QReg->qubitTypes[qubit] == KET_ONE) {
		/* do nothing */
		return QReg->qubitTypes[qubit];
	}

	if(qubit >= QReg->getNumQubits()) {
		printf("[%s] qubit(%d) out of range!\n", __func__, qubit);
		exit(0);
	}

	/************************************************** 
	 * (STEP1) get collapsed states according to argument
	 **************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		int thread_id = omp_get_thread_num();
		int tid = thread_id % QReg->getCPUCores();
		QState *Q = NULL;

		std::map<qsize_t, QState*>::iterator it;
		for(it = QReg->qstore[i].begin(); it != QReg->qstore[i].end(); it++) {
			Q = it->second;
			qsize_t qidx = Q->getIndex();
			int hashid;
	
			if((collapse == 0 && stripe_upper(qidx, qubit) == true) || 
			   (collapse == 1 && stripe_lower(qidx, qubit) == true)) {
				hashid = (int)(qidx % QSTORE_PARTITION);
				QReg->delQList[hashid][tid].push_back(Q);
			}
		}
	}

	/************************************************** 
	 * (STEP5) remove zero amplitude states 
	 **************************************************/
	#pragma omp parallel for
	for(int i=0; i<QSTORE_PARTITION; i++) {
		for(int j=0; j<QReg->getCPUCores(); j++) {
			for(auto Q : QReg->delQList[i][j]) {
				QReg->eraseQState_nolock(Q->getIndex());
			}
			QReg->delQList[i][j].clear();
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
