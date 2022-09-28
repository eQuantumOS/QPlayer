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

#include "register.h"
#include "experimental.h"
#include "gate.h"
#include "dump.h"

using namespace std;

std::mutex lock_is_entangle;
std::mutex lock_add_entangle;


void showEntangle(std::vector<list<int>> eGroups) 
{
	for(auto group : eGroups) {
		for(auto qubit : group) {
			printf("%d ", qubit);
		}
		printf("\n");
	}
}

bool __is_entangle(std::vector<list<int>>& eGroups, int q1, int q2)
{
	lock_is_entangle.lock();

	for(auto group : eGroups) {
		bool find_q1 = false;
		bool find_q2 = false;
		for(auto qubit : group) {
			if(qubit == q1) {
				find_q1 = true;
			} else if(qubit == q2) {
				find_q2 = true;
			}

			if(find_q1 == true && find_q2 == true) {
				lock_is_entangle.unlock();
				return true;
			}
		}
	}

	lock_is_entangle.unlock();

	return false;
}

bool __is_samestates(QRegister *QReg1, QRegister *QReg2) 
{
	QState *Q1 = NULL;
	QState *Q2 = NULL;
	bool positive_same = true;
	bool negative_same = true;
	double real1 = 0;
	double real2 = 0;
	double imag1 = 0;
	double imag2 = 0;
	double delta = 0.0000000000001;

	/* check normal phase */
	QReg1->setOrderedQState();
	QReg2->setOrderedQState();
	do {
		Q1 = QReg1->getOrderedQState();
		Q2 = QReg2->getOrderedQState();

		if(Q1 == NULL || Q2 == NULL) {
			break;
		}

		real1 = Q1->getAmplitude().real();
		imag1 = Q1->getAmplitude().imag();
		real2 = Q2->getAmplitude().real();
		imag2 = Q2->getAmplitude().imag();

		if(((real1 != real2) && (real1-real2) > delta) ||
		   ((imag1 != imag2) && (imag1-imag2) > delta)) { 
			positive_same = false;
			break;
		}
	} while(1);

	if(positive_same == true) {
		return true;
	}

	/* check normal phase */
	QReg1->setOrderedQState();
	QReg2->setOrderedQState();
	do {
		Q1 = QReg1->getOrderedQState();
		Q2 = QReg2->getOrderedQState();

		if(Q1 == NULL || Q2 == NULL) {
			break;
		}

		real1 = Q1->getAmplitude().real();
		imag1 = Q1->getAmplitude().imag();
		real2 = Q2->getAmplitude().real() * -1.0;
		imag2 = Q2->getAmplitude().imag() * -1.0;

		if(((real1 != real2) && (real1-real2) > delta) ||
		   ((imag1 != imag2) && (imag1-imag2) > delta)) { 
			negative_same = false;
			break;
		}
	} while(1);

	if(positive_same == true || negative_same == true) {
		return true;
	} 

	return false;
}

void __add_entangle(std::vector<list<int>>& eGroups, int q1, int q2)
{
	bool found = false;
	int gsize = eGroups.size();
	int group1 = -1;
	int group2 = -1;

	lock_add_entangle.lock();

	/* find previous entangle group of q1, q2 */
	for(int i=0; i<gsize; i++) {
		for(auto qubit : eGroups[i]) {
			if(qubit == q1) {
				group1 = i;
			} else if(qubit == q2) {
				group2 = i;
			}
		}
	}

	if(group1 == -1 && group2 == -1) {
		/* first addition of q1, q2 */
		list<int> group;
		group.push_back(q1);
		group.push_back(q2);
		eGroups.push_back(group);
	} else if(group1 != -1 && group2 == -1) {
		/* add q2 into q1 group */
		eGroups[group1].push_back(q2);
	} else if(group1 == -1 && group2 != -1) {
		/* add q1 into q0 group */
		eGroups[group2].push_back(q1);
	} else {
		/* merge q1 and q2 group */
		for(auto qubit : eGroups[group2]) {
			eGroups[group1].push_back(qubit);
		}
		eGroups.erase(eGroups.begin() + group2);
	}

	lock_add_entangle.unlock();
}

void __estimation_step1(QRegister *QReg, std::vector<int> candidates, std::vector<list<int>>& eGroups) 
{
	int qubits = QReg->getNumQubits();
	int candidateQubits = candidates.size();
	int Q1 = 0;
	int Q2 = 0;

	for(int i=0; i<candidateQubits-1; i++) {
		QRegister *QRegClone = new QRegister(QReg);
		Q1 = candidates[i];

		MF(QRegClone, Q1, 0);

		for(int j=i+1; j<candidateQubits; j++) {
			Q2 = candidates[j];

			if(__is_entangle(eGroups, Q1, Q2) == true) {
				continue;
			}

			if (QType(QRegClone, Q2) == KET_ZERO || QType(QRegClone, Q2) == KET_ONE) {
				__add_entangle(eGroups, Q1, Q2);
			}
		}

		delete QRegClone;
	}
}

void __estimation_step2(QRegister *QReg, std::vector<int> candidates, std::vector<list<int>>& eGroups) 
{
	int qubits = QReg->getNumQubits();
	int candidateQubits = candidates.size();
	int Q1 = 0;
	int Q2 = 0;

	for(int i=0; i<candidateQubits-1; i++) {
		QRegister *QRegClone = new QRegister(QReg);
		Q1 = candidates[i];

		H(QRegClone, Q1);
		MF(QRegClone, Q1, 0);

		for(int j=i+1; j<candidateQubits; j++) {
			Q2 = candidates[j];

			if(__is_entangle(eGroups, Q1, Q2) == true) {
				continue;
			}

			if (QType(QRegClone, Q2) == KET_ZERO || QType(QRegClone, Q2) == KET_ONE) {
				__add_entangle(eGroups, Q1, Q2);
			}
		}

		delete QRegClone;
	}
}

void __estimation_step3(QRegister *QReg, std::vector<int> candidates, std::vector<list<int>>& eGroups) 
{
	int qubits = QReg->getNumQubits();
	int candidateQubits = candidates.size();

	for(int i=0; i<candidateQubits-1; i++) {
		for(int j=i+1; j<candidateQubits; j++) {
			int Q1 = candidates[i];
			int Q2 = candidates[j];

			if(__is_entangle(eGroups, Q1, Q2) == true) {
				continue;
			}

			QRegister *QRegClone0 = new QRegister(QReg);
			QRegister *QRegClone1 = new QRegister(QReg);

			MF(QRegClone0, Q2, 0);
			MF(QRegClone1, Q2, 1);

			if(__is_samestates(QRegClone0, QRegClone1) == false) {
				__add_entangle(eGroups, Q1, Q2);
			} 

			delete QRegClone0;
			delete QRegClone1;
		}
	}
}

void entangle_estimation(QRegister *QReg, std::vector<list<int>>& eGroups)
{
	QRegister *QRegs[128];
	int qubits = QReg->getNumQubits();
	std::vector<int> candidates;

	for (int i = 0; i < qubits; i++) {
		if (QType(QReg, i) == KET_ZERO || QType(QReg, i) == KET_ONE) {
			continue;
		}
		candidates.push_back(i);
	}
	std::sort(candidates.begin(), candidates.end());

	__estimation_step1(QReg, candidates, eGroups);
	__estimation_step2(QReg, candidates, eGroups);
	__estimation_step3(QReg, candidates, eGroups);
}

/*
 * Estimate Entanglement States
 */
void getEntanglements(QRegister *QReg, std::vector<list<int>>& eGroups) {
	entangle_estimation(QReg, eGroups);
}

void getEntanglements(QRegister *QReg) {
	std::vector<list<int>> eGroups;
	getEntanglements(QReg, eGroups);

	printf("========== Entanglement Groups ==========\n"); 
	for(auto group : eGroups) {
		for(auto qubit : group) {
			printf("%d ", qubit);
		}
		printf("\n");
	}
}

/*
 * Estimate the quantum state int four types below.
 * 
 *   - KET_ZERO  : |0>
 *   - KET_ONE   : |1>
 *   - KET_SUPERPOSED : a|0> + b|1> or a|0> - b|1>
 */
int QType(QRegister *QReg, int qubit) 
{
	qsize_t mask = quantum_shiftL(1, qubit);
	QState *Q = NULL;
	bool is_zero = false;
	bool is_one = false;
	int type = KET_UNKNOWN;

	QReg->setOrderedQState();
	while((Q = QReg->getOrderedQState()) != NULL) {
		if((Q->getIndex() & mask) == 0) {
			is_zero = true;
		} else {
			is_one = true;
		}

		if(is_zero == true && is_one == true) {
			break;
		}
	}

	if(is_zero == true && is_one == true) {
		type = KET_SUPERPOSED;
	} else {
		if(is_zero == true) {
			type = KET_ZERO;
		} else {
			type = KET_ONE;
		}
	}

	return type;
}

char *QTypeStr(QRegister *QReg, int qubit)
{
	int type = QType(QReg, qubit);

	if(type == KET_ZERO) return "|0>"; 
	else if(type == KET_ONE) return "|1>"; 
	else if(type == KET_SUPERPOSED) return "|S>";

	return "UNKNOWN";
}

/*
 * show probability of the qubit 
 */
double getQubitProb(QRegister *QReg, int qubit, int state) 
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

	lo = lo / length;
	up = up / length;

	if(state == KET_ZERO) {
		return norm(lo);
	} else if(state == KET_ONE) {
		return norm(up);
	}

	return 0;
}
