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
	int limit = 0;

	/* check normal phase */
	limit = 0;
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
	} while(limit++ < 100);

	if(positive_same == true) {
		return true;
	}

	/* check normal phase */
	limit = 0;
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
	} while(limit++ < 100);

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

	for(int i=0; i<candidateQubits-1; i++) {
		for(int j=i+1; j<candidateQubits; j++) {
			int Q1 = candidates[i];
			int Q2 = candidates[j];

			if(__is_entangle(eGroups, Q1, Q2) == true) {
				continue;
			}

			QRegister *QRegClone = new QRegister(QReg);

			MF(QRegClone, Q2, 0);

			if (QType(QRegClone, Q1) == KET_ZERO || QType(QRegClone, Q2) == KET_ONE) {
				__add_entangle(eGroups, Q1, Q2);
			}

			delete QRegClone;
		}
	}
}

void __estimation_step2(QRegister *QReg, std::vector<int> candidates, std::vector<list<int>>& eGroups) 
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
}

void getEntanglements(QRegister *QReg, std::vector<list<int>>& eGroups) {
	entangle_estimation(QReg, eGroups);
}

void getEntanglement(QRegister *QReg) {
	std::vector<list<int>> eGroups;
	getEntanglements(QReg, eGroups);
}
