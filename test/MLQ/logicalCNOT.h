#ifndef __LOGICAL_CNOT_H__
#define __LOGICAL_CNOT_H__

#include <stdio.h>

#include "qplayer.h"
#include "logicalQubit.h"

struct surgery_circuit {
    int sq;
    int sq_mval;
    int dq_size;
    int dq_list[4];
};

class logicalCNOT {
public:
	QRegister *QRegMerge;

	logicalQubit *AQ;
	logicalQubit *CQ;
	logicalQubit *TQ;

	logicalQubit *LQHigh;
	logicalQubit *LQLow;

	int pq_num;
	int stateL;
	int stateH;
	int relation;

public:
	logicalCNOT(logicalQubit *controlLQ, logicalQubit *targetLQ) {
		/***************************************/
		/* STEP1: allocate merged Q-Register   */
		/***************************************/
		CQ = controlLQ;
		TQ = targetLQ;

		pq_num = CQ->QReg->getNumQubits();
		QRegMerge = new QRegister(pq_num);
		QRegMerge->clear();

		/***************************************/
		/* STEP2: set LQs in qubit order       */
		/***************************************/
		if(CQ->getPQ(CQ->dq_list[0]) < TQ->getPQ(TQ->dq_list[0])) {
			LQLow = CQ;
			LQHigh = TQ;
		} else {
			LQLow = TQ;
			LQHigh = CQ;
		}

		/***************************************/
		/* STEP3: determine two LQs states     */
		/***************************************/
		stateH = getLogicalStates(LQHigh);
		stateL = getLogicalStates(LQLow);
		relation = getLogicalRelation(stateH, stateL);

	#if 1
		printf("\nCNOT : LQ%d -> LQ%d\n", CQ->lq_id, TQ->lq_id);
		printf("\t[LQ%2d] stateH   = %s\n", LQHigh->lq_id, modeString(stateH));
		printf("\t[LQ%2d] stateL   = %s\n", LQLow->lq_id, modeString(stateL));
	#endif

		/***************************************/
		/* STEP4: merge two LQsa               */
		/***************************************/
		compose();
	}

	~logicalCNOT(void) { 
		delete QRegMerge; 
	}

public:
	int getLogicalStates(logicalQubit *LQ) {
		QRegister *QRegMask = NULL;
		QState *Q;
		int mode = KET_UNKNOWN;
		int num_zero = 0;
		int num_one = 0;
		int num_plus = 0;
		int num_minus = 0;
		int num_states = 0;

		/*********************************************/
		/* STEP1: generate LQ to have masking states */
		/*********************************************/
		QRegMask = new QRegister(pq_num);
		QRegMask->clear();

		/* 
		 * Extract only the data qubit bits of this logical qubit. That is, 
		 * the number of states of QRegMask is always one of 16 or 32.
         *  - if LQ=|0> or |1> --> QRegMask has 16 states.
         *  - if LQ=|+> or |-> --> QRegMask has 32 states.
		 */
		LQ->QReg->setOrderedQState();
		while((Q = LQ->QReg->getOrderedQState()) != NULL) {
			qsize_t newIdx = Q->getIndex() & LQ->getBitMask();
			QState *newQ = QRegMask->findQState(newIdx);
			if(newQ == NULL) {
				newQ = new QState(newIdx, Q->getAmplitude());
				QRegMask->setQState(newQ->getIndex(), newQ);
			}

			/* check sign of states */
			if(Q->getAmplitude().real() > 0) {
				num_plus++;
			} else {
				num_minus++;
			}
		}

		/***************************************/
		/* STEP2: analyze all masked states    */
		/***************************************/
		QRegMask->setOrderedQState();
		while((Q = QRegMask->getOrderedQState()) != NULL) {
			char qstring[pq_num];
			int ones = 0;

			memset(qstring, 0, sizeof(qstring));

			/* check '1' bit of states */
			to_binary(Q->getIndex(), pq_num, qstring);
			for(int i=0; i<pq_num; i++) {
				if(qstring[i] == '1') {
					ones++;
				}
			}

			if((ones % 2) == 0) {
				num_zero++;
			} else {
				num_one++;
			}

			num_states++;
		}

		/***************************************/
		/* STEP3: determine the logical state  */
		/***************************************/
		if(num_states == 1 || num_states == 16) {
			if(num_zero == num_states) {
				mode = KET_LZERO;
			} else {
				mode = KET_LONE;
			}
		} else if(num_states == 2 || num_states == 32) {
			/* In antually, it is difficult to determine the |+>, |-> 
			 * states only by the number of signs. In logical qubit synthesis,
			 * the sign of the lower logical qubit is important. If is superposed
			 * ans has a minus sign amplitude, it is defined to |->.
			 */
			if(num_plus == num_minus) {
				mode = KET_LMINUS;
			} else {
				mode = KET_LPLUS;
			}
		}

		delete QRegMask;

		return mode;
	}

	int getLogicalRelation(int stateLQ1, int stateLQ2) {
		QRegister *QRegMask = NULL;
		QState *Q;
		int tensorStateNum = 0;
		bool tensorState[4];
		int type = TENSOR;

		/***********************************************/
		/* STEP1: check if LQ is superposed or not     */
		/***********************************************/
		/* If LQ is not superposed, it cannot be entangled. Therefore, 
		 * in this case, it is definitely a tensor product.
         */
		if(stateLQ1 == KET_LZERO || stateLQ1 == KET_LONE) {
			return TENSOR;
		}

		if(stateLQ2 == KET_LZERO || stateLQ2 == KET_LONE) {
			return TENSOR;
		}

		/*********************************************/
		/* STEP2: generate LQ to have masking states */
		/*********************************************/
		QRegMask = new QRegister(pq_num);
		QRegMask->clear();

		/* generate masked LQHigh */
		LQHigh->QReg->setOrderedQState();
		while((Q = LQHigh->QReg->getOrderedQState()) != NULL) {
			qsize_t newIdx;

			newIdx = Q->getIndex();
			newIdx &= (LQHigh->getBitMask() | LQLow->getBitMask());
			QState *newQ = QRegMask->findQState(newIdx);
			if(newQ == NULL) {
				newQ = new QState(newIdx, Q->getAmplitude());
				QRegMask->setQState(newQ->getIndex(), newQ);
			}
		}

		/***********************************************/
		/* STEP3: scan all states of two superposed LQ */
		/***********************************************/
		for(int i=0; i<4; i++) {
			tensorState[i] = false;
		}

		QRegMask->setOrderedQState();
		while((Q = QRegMask->getOrderedQState()) != NULL) {
			qsize_t stateH = Q->getIndex() & LQHigh->getBitMask();
			qsize_t stateL = Q->getIndex() & LQLow->getBitMask();
			char qstringH[pq_num];
			char qstringL[pq_num];
			int onesH = 0;
			int onesL = 0;

			memset(qstringH, 0, sizeof(qstringH));
			memset(qstringL, 0, sizeof(qstringL));

			/* check '1' bit of high state */
			to_binary(stateH, pq_num, qstringH);
			for(int i=0; i<pq_num; i++) {
				if(qstringH[i] == '1') {
					onesH++;
				}
			}

			/* check '1' bit of low state */
			to_binary(stateL, pq_num, qstringL);
			for(int i=0; i<pq_num; i++) {
				if(qstringL[i] == '1') {
					onesL++;
				}
			}

			if((onesH % 2) == 0 && (onesL % 2) == 0) tensorState[0] = true;
			else if((onesH % 2) == 0 && (onesL % 2) == 1) tensorState[1] = true;
			else if((onesH % 2) == 1 && (onesL % 2) == 0) tensorState[2] = true;
			else if((onesH % 2) == 1 && (onesL % 2) == 1) tensorState[3] = true;
		}

		/***********************************************/
		/* STEP4: determine logical relation           */
		/***********************************************/
		for(int i=0; i<4; i++) {
			if(tensorState[i] == true) {
				tensorStateNum++;
			}
		}

		delete QRegMask;

		return (tensorStateNum == 4) ? TENSOR : ENTANGLE;
	}

	int getLogical(qsize_t qidx, int qubits) {
		char qstring[qubits];
		int ones = 0;

		/* check '1' bit of states */
		memset(qstring, 0, sizeof(qstring));
		to_binary(qidx, qubits, qstring);
		for(int i=0; i<qubits; i++) {
			if(qstring[i] == '1') {
				ones++;
			}
		}

		return (ones % 2) == 0 ? 0 : 1;
	}

public:
	void bitMerge(logicalQubit *fromLQ, logicalQubit *toLQ) {
		QRegister *QRegReduced = new QRegister(pq_num);
		QState *newQ;
		QState *toQ;
		QState *fromQ;
		qsize_t newIdx;
		qsize_t fromMask = 0;
		qsize_t toMask = 0;

		/* get bitmask of two logical qubits */
		for(int i=0; i<9; i++) {
			fromMask |= quantum_shiftL(1, fromLQ->getPQ(fromLQ->dq_list[i]));
		}

		for(int i=0; i<9; i++) {
			toMask |= quantum_shiftL(1, toLQ->getPQ(toLQ->dq_list[i]));
		}

		/* Shrink fromLQ to remove redundancy of state-bit.       */
		/* Because fromLQ is |0> or |1>, 16 states should remain. */
		QRegReduced->clear();
		fromLQ->QReg->setOrderedQState();
		while((fromQ = fromLQ->QReg->getOrderedQState()) != NULL) {
			newIdx = fromQ->getIndex() & fromMask;

			newQ = QRegReduced->findQState(newIdx);
			if(newQ == NULL) {
				newQ = new QState(newIdx, fromQ->getAmplitude());
				QRegReduced->setQState(newIdx, newQ);
			} 
		}

		/* merge two logical qubits */
		toLQ->QReg->setOrderedQState();
		while((toQ = toLQ->QReg->getOrderedQState()) != NULL) {
			QRegReduced->setOrderedQState();
			while((fromQ = QRegReduced->getOrderedQState()) != NULL) {
				newIdx = toQ->getIndex() & ~fromMask;
				newIdx |= fromQ->getIndex();

				newQ = QRegMerge->findQState(newIdx);
				if(newQ == NULL) {
					newQ = new QState(newIdx, toQ->getAmplitude());
					QRegMerge->setQState(newIdx, newQ);
				} 
			}
		}
		
		delete QRegReduced;
	}

	void entangleMerge(logicalQubit *fromLQ, logicalQubit *toLQ) {
		bitMerge(fromLQ, toLQ);
	}

	void bitNphaseMerge(logicalQubit *fromLQ, logicalQubit *toLQ) {
		QRegister *QRegReduced = new QRegister(pq_num);
		qsize_t stride = quantum_shiftL(1, (qsize_t)fromLQ->getMagicPQ());
		QState *newQ = NULL;
		QState *toQ = NULL;
		QState *fromQ = NULL;
		qsize_t newIdx;
		qsize_t fromMask = 0;
		qsize_t toMask = 0;
		complex_t newAmp;

		/* get bitmask of two logical qubits */
		for(int i=0; i<9; i++) {
			fromMask |= quantum_shiftL(1, fromLQ->getPQ(fromLQ->dq_list[i]));
		}

		for(int i=0; i<9; i++) {
			toMask |= quantum_shiftL(1, toLQ->getPQ(toLQ->dq_list[i]));
		}

		/* shrink fromLQ to remove redundancy of state-bit.       */
		/* Because fromLQ is |+> or |->, 32 states should remain. */
		QRegReduced->clear();
		fromLQ->QReg->setOrderedQState();
		while((fromQ = fromLQ->QReg->getOrderedQState()) != NULL) {
			newIdx = fromQ->getIndex() & fromMask;

			newQ = QRegReduced->findQState(newIdx);
			if(newQ == NULL) {
				newQ = new QState(fromQ->getIndex(), fromQ->getAmplitude());
				QRegReduced->setQState(newQ->getIndex(), newQ);
			} 
		}

		/* merge two logical qubits */
		toLQ->QReg->setOrderedQState();
		while((toQ = toLQ->QReg->getOrderedQState()) != NULL) {
			/* check sign of fromStates in toLQ */
			QState *lower = NULL;
			QState *upper = NULL;
			bool negativeSign = false;

			if(stripe_lower(toQ->getIndex(), fromLQ->getMagicPQ()) == true) {
				lower = toQ;
			} else {
				lower = toLQ->QReg->findQState(toQ->getIndex() - stride);
			}

			if(lower->getAmplitude().real() < 0) {
				/* this is the case when sign is -+ or -- */
				negativeSign = true;
			}

			QRegReduced->setOrderedQState();
			while((fromQ = QRegReduced->getOrderedQState()) != NULL) {
				/* check if from-state-region is the same */ 
				int fromState1 = getLogical(toQ->getIndex() & fromMask, pq_num);
				int fromState2 = getLogical(fromQ->getIndex() & fromMask, pq_num);
				if(fromState1 != fromState2) {
					continue;
				}
				
				newAmp = fromQ->getAmplitude();
				if(negativeSign == true) {
					newAmp *= -1;
				}

				newIdx = toQ->getIndex() & ~fromMask;
				newIdx |= fromQ->getIndex();

				newQ = QRegMerge->findQState(newIdx);
				if(newQ == NULL) {
					newQ = new QState(newIdx, newAmp);
					QRegMerge->setQState(newIdx, newQ);
				} 
			}
		}

		delete QRegReduced;
	}

	void compose(void) {
		if(stateL == KET_LZERO || stateL == KET_LONE) {
			/* just transfer bits of lowLQ to highLQ */
			bitMerge(LQLow, LQHigh);
		} else if(stateH == KET_LZERO || stateH == KET_LONE) {
			/* just transfer bits of highLQ to lowLQ */
			bitMerge(LQHigh, LQLow);
		} else if(relation == ENTANGLE) {
			/* just transfer bits of lowLQ to highLQ */
			entangleMerge(LQLow, LQHigh);
		} else {
			/* merging bits & phase of lowLQ to highLQ */
			bitNphaseMerge(LQLow, LQHigh);
		}

		printf("Merged States = %ld\n", (uint64_t)QRegMerge->getNumStates());
	}

	void decompose(void) {
		QState *Q;

		/******************************************/
		/* STEP1: clear all states of two LQs     */
		/******************************************/
		LQHigh->QReg->clear();
		LQLow->QReg->clear();

		/******************************************/
		/* STEP2: generate states for two LQs     */
		/******************************************/
		QRegMerge->setOrderedQState();
		while((Q = QRegMerge->getOrderedQState()) != NULL) {
			qsize_t stateH = Q->getIndex() & LQHigh->getBitMask();
			qsize_t stateL = Q->getIndex() & LQLow->getBitMask();
			char qstringH[pq_num];
			char qstringL[pq_num];
			qsize_t newIdxH;
			qsize_t newIdxL;
			QState *newQH;
			QState *newQL;
			int onesH = 0;
			int onesL = 0;

			/********************************************/
			/* STEP2-1: check logical state for two LQs */
			/********************************************/
			memset(qstringH, 0, sizeof(qstringH));
			memset(qstringL, 0, sizeof(qstringL));

			/* check '1' bit of high state */
			to_binary(stateH, pq_num, qstringH);
			for(int i=0; i<pq_num; i++) {
				if(qstringH[i] == '1') {
					onesH++;
				}
			}

			/* check '1' bit of low state */
			to_binary(stateL, pq_num, qstringL);
			for(int i=0; i<pq_num; i++) {
				if(qstringL[i] == '1') {
					onesL++;
				}
			}

			/********************************************/
			/* STEP2-2: determine new quantum index     */
			/********************************************/
			/* set LQHigh bit to '000000000' */
			newIdxH = Q->getIndex() & ~LQLow->getBitMask();
			if((onesL % 2) != 0) {
				/* set LQHigh bit to '000010000' */
				newIdxH |= LQLow->getBitMaskOne();
			}

			/* set LQLow bit to '000000000' */
			newIdxL = Q->getIndex() & ~LQHigh->getBitMask();
			if((onesH % 2) != 0) {
				/* set LQLow bit to '000010000' */
				newIdxL |= LQHigh->getBitMaskOne();
			}

			/*********************************************/
			/* STEP2-3: set LQHigh state with magicQL    */
			/*********************************************/
			/* add new state of LQHigh */
			newQH = LQHigh->QReg->findQState(newIdxH);
			if(newQH == NULL) {
				newQH = new QState(newIdxH, Q->getAmplitude());
				LQHigh->QReg->setQState(newQH->getIndex(), newQH);
			}

			/* add new state of LQLow */
			newQL = LQLow->QReg->findQState(newIdxL);
			if(newQL == NULL) {
				newQL = new QState(newIdxL, Q->getAmplitude());
				LQLow->QReg->setQState(newQL->getIndex(), newQL);
			}
		}
	}

public:
	int getXMergeType(void) {
		int type = 0;

		if(AQ->col < TQ->col) {
			if(AQ->row < CQ->row) {
				type = RIGHT_UP;
			} else {
				type = RIGHT_DOWN;
			}
		} else {
			if(AQ->row < CQ->row) {
				type = LEFT_UP;
			} else {
				type = LEFT_DOWN;
			}
		}

		return type;
	}

	int getZMergeType(void) {
		int type = 0;

		if(AQ->row < CQ->row) {
			if(AQ->col < TQ->col) {
				type = LEFT_DOWN;
			} else {
				type = RIGHT_DOWN;
			}
		} else {
			if(AQ->col < TQ->col) {
				type = LEFT_UP;
			} else {
				type = RIGHT_UP;
			}
		}

		return type;
	}

	void genXMergeCircuit(struct surgery_circuit *circuit) {
		int type = getXMergeType();

		if(type == LEFT_UP || type == LEFT_DOWN) {
			circuit[0].sq = AQ->getPQ(Q21);
			circuit[0].sq_mval = 0;
			circuit[0].dq_size = 2;
			circuit[0].dq_list[0] = AQ->getPQ(Q18);
			circuit[0].dq_list[1] = TQ->getPQ(Q20);

			circuit[1].sq = AQ->getPQ(Q7);
			circuit[1].sq_mval = 0;
			circuit[1].dq_size = 4;
			circuit[1].dq_list[0] = AQ->getPQ(Q4);
			circuit[1].dq_list[1] = AQ->getPQ(Q11);
			circuit[1].dq_list[2] = TQ->getPQ(Q6);
			circuit[1].dq_list[3] = TQ->getPQ(Q13);
		} else if(type == RIGHT_UP || type == RIGHT_DOWN) {
			circuit[0].sq = AQ->getPQ(Q3);
			circuit[0].sq_mval = 0;
			circuit[0].dq_size = 2;
			circuit[0].dq_list[0] = AQ->getPQ(Q6);
			circuit[0].dq_list[1] = TQ->getPQ(Q4);

			circuit[1].sq = AQ->getPQ(Q17);
			circuit[1].sq_mval = 0;
			circuit[1].dq_size = 4;
			circuit[1].dq_list[0] = AQ->getPQ(Q13);
			circuit[1].dq_list[1] = AQ->getPQ(Q20);
			circuit[1].dq_list[2] = TQ->getPQ(Q11);
			circuit[1].dq_list[3] = TQ->getPQ(Q18);
		}
	}

	void genZMergeCircuit(struct surgery_circuit *circuit) {
		int type = getZMergeType();

		if(type == LEFT_UP || type == RIGHT_UP) {
			circuit[0].sq = AQ->getPQ(Q0);
			circuit[0].sq_mval = 0;
			circuit[0].dq_size = 2;
			circuit[0].dq_list[0] = AQ->getPQ(Q4);
			circuit[0].dq_list[1] = CQ->getPQ(Q18);

			circuit[1].sq = AQ->getPQ(Q2);
			circuit[1].sq_mval = 0;
			circuit[1].dq_size = 4;
			circuit[1].dq_list[0] = AQ->getPQ(Q5);
			circuit[1].dq_list[1] = AQ->getPQ(Q6);
			circuit[1].dq_list[2] = CQ->getPQ(Q19);
			circuit[1].dq_list[3] = CQ->getPQ(Q20);
		} else if(type == LEFT_DOWN || type == RIGHT_DOWN) {
			circuit[0].sq = AQ->getPQ(Q24);
			circuit[0].sq_mval = 0;
			circuit[0].dq_size = 2;
			circuit[0].dq_list[0] = AQ->getPQ(Q20);
			circuit[0].dq_list[1] = CQ->getPQ(Q6);

			circuit[1].sq = AQ->getPQ(Q22);
			circuit[1].sq_mval = 0;
			circuit[1].dq_size = 4;
			circuit[1].dq_list[0] = AQ->getPQ(Q18);
			circuit[1].dq_list[1] = AQ->getPQ(Q19);
			circuit[1].dq_list[2] = CQ->getPQ(Q4);
			circuit[1].dq_list[3] = CQ->getPQ(Q5);
		}
	}

	void latticeSurgeryXX(int *mxx1, int *mxx2) {
		struct surgery_circuit *xxMerge = NULL;

		/*********************************************/
		/* STEP1: generate merge-xx circuit          */
		/*********************************************/
		xxMerge = (struct surgery_circuit *)malloc(sizeof(struct surgery_circuit) * 2);
		genXMergeCircuit(xxMerge);

		/*********************************************/
		/* STEP2: run x-stabilizer circuit           */
		/*********************************************/
		for(int i=0; i<2; i++) {
			initZ(QRegMerge, xxMerge[i].sq);
			H(QRegMerge, xxMerge[i].sq);
			for(int j=0; j<xxMerge[i].dq_size; j++) {
				CX(QRegMerge, xxMerge[i].sq, xxMerge[i].dq_list[j]);
			}
			H(QRegMerge, xxMerge[i].sq);
			xxMerge[i].sq_mval = M(QRegMerge, xxMerge[i].sq);
		}

		*mxx1 = xxMerge[0].sq_mval;
		*mxx2 = xxMerge[1].sq_mval;

		free(xxMerge);
	}

	void latticeSurgeryZZ(int *mzz1, int *mzz2) {
		struct surgery_circuit *zzMerge = NULL;

		/*********************************************/
		/* STEP1: generate merge-xx circuit          */
		/*********************************************/
		zzMerge = (struct surgery_circuit *)malloc(sizeof(struct surgery_circuit) * 2);
		genZMergeCircuit(zzMerge);

		/*********************************************/
		/* STEP2: run x-stabilizer circuit           */
		/*********************************************/
		for(int i=0; i<2; i++) {
			initZ(QRegMerge, zzMerge[i].sq);
			for(int j=0; j<zzMerge[i].dq_size; j++) {
				CX(QRegMerge, zzMerge[i].dq_list[j], zzMerge[i].sq);
			}
			zzMerge[i].sq_mval = M(QRegMerge, zzMerge[i].sq);
		}

		*mzz1 = zzMerge[0].sq_mval;
		*mzz2 = zzMerge[1].sq_mval;

		free(zzMerge);
	}

	int measureXAncilla(void) {
		return AQ->LMX(QRegMerge);
	}

	void latticeSurgery(logicalQubit *ancillaLQ) {
		int mxx1, mxx2;
		int mzz1, mzz2;
		int mx;

		/*********************************************/
		/* STEP1: Initialize Ancilla                 */
		/*********************************************/
		AQ = ancillaLQ;
		AQ->init(QRegMerge, KET_LZERO);

		/*********************************************/
		/* STEP2: XX merge of Ancilla & Target       */
		/*********************************************/
		latticeSurgeryXX(&mxx1, &mxx2);
		AQ->runStabilizer(QRegMerge);
		TQ->runStabilizer(QRegMerge);

		/*********************************************/
		/* STEP3: ZZ merge of Ancilla & Control      */
		/*********************************************/
		latticeSurgeryZZ(&mzz1, &mzz2);
		AQ->runStabilizer(QRegMerge);
		CQ->runStabilizer(QRegMerge);

		/*********************************************/
		/* STEP4: X measurement of Ancilla           */
		/*********************************************/
		mx = measureXAncilla();

		/*********************************************/
		/* STEP5: post processing                    */
		/*********************************************/
		mxx1 = (mxx1 == 0 ? 1 : -1);
		mxx2 = (mxx2 == 0 ? 1 : -1);
		mzz1 = (mzz1 == 0 ? 1 : -1);
		mzz2 = (mzz2 == 0 ? 1 : -1);
		mx = (mx == 0 ? 1 : -1);

		int a = ((mxx1 * mxx2) == -1 ? 1 : 0);
		int b = ((mzz1 * mzz2) == -1 ? 1 : 0);
		int c = (mx == -1 ? 1 : 0);
		
		if(a == 1) CQ->LZ(QRegMerge);
		if(c == 1) CQ->LZ(QRegMerge);
		if(b == 1) TQ->LX(QRegMerge);

		/*********************************************/
		/* STEP6: set all PQs in Ancilla to |0>      */
		/*********************************************/
		AQ->clear(QRegMerge);
	}
};

#endif
