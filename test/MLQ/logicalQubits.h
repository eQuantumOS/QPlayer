#ifndef __LOGICAL_QUBITS_H__
#define __LOGICAL_QUBITS_H__

#include <stdio.h>

#include "qplayer.h"

#include "ds.h"
#include "logicalQubit.h"
#include "logicalCNOT.h"
#include "logicalST.h"

struct qubit_delimiter qd;

class logicalQubits {
public:
	logicalQubit *LQ[MAX_LQUBITS];
	int lq_num;
	int pq_num;

public:
	logicalQubits(struct lqubits_info *lqinfo) { 
		/********************************/
		/* STEP1: set global parameters */
		/********************************/
		lq_num = lqinfo->lq_num;
		pq_num = lqinfo->pq_num;

		/**********************************/
		/* STEP2: allocate Logical Qubits */
		/**********************************/
		for(int i=0; i<lq_num; i++) {
			LQ[i] = new logicalQubit(lqinfo->lqubits[i].id, lqinfo);
		}

		/**********************************/
		/* [DEBUG] set dump delimeter     */ 
		/**********************************/
		qd.size = lq_num;
		for(int i=0; i<lq_num; i++) {
			qd.qubits[i] = LQ[i]->getPQ(LQ[i]->dq_list[8]);
		}
	} 

	~logicalQubits(void) { 
		for(int i=0; i<lq_num; i++) {
			delete LQ[i];
		}
	}

	void normalize(void) {
		for(int i=0; i<lq_num; i++) {
			LQ[i]->QReg->normalize();
		}
	}

	void showQState(void) {
		printf("**********************************");
		printf(" Dump %2d Logical Qubits ", lq_num);
		printf("**********************************\n");
		for(int i=0; i<lq_num; i++) {
			LQ[i]->showQState(0, LQ[lq_num-1]->getPQ(LQ[lq_num-1]->dq_list[8]));
		}
	}

	void showNumStates(void) {
		for(int i=0; i<lq_num; i++) {
			printf("[LQ%d] %ld states\n", i, (uint64_t)LQ[lq_num-1]->QReg->getNumStates());
		}
		fflush(stdout);
	}

public:
	void initLQ(int lqid, int mode) {
		/* initialize this logical qubit */
		LQ[lqid]->init(mode);

		/* initialize neighbor magic qubits */
		for(int i=0; i<lq_num; i++) {
			if(i == lqid) {
				continue;
			}

			if(mode == KET_LZERO) {
				LQ[i]->magic_initZ(lqid);
			} else if(mode == KET_LONE) {
				LQ[i]->magic_initZ(lqid);
				LQ[i]->magic_X(lqid);
			} else if(mode == KET_LPLUS) {
				LQ[i]->magic_initZ(lqid);
				LQ[i]->magic_H(lqid);
			} else if(mode == KET_LMINUS) {
				LQ[i]->magic_initZ(lqid);
				LQ[i]->magic_X(lqid);
				LQ[i]->magic_H(lqid);
			}
		}

		normalize();
	}

	void inject(int lqid, double rx, double ry, double rz) {
		/* logical X operation for this logical qubit */
		LQ[lqid]->inject(rx, ry, rz);

		/* physical X operation for neighbor magic qubits */
		for(int i=0; i<lq_num; i++) {
			if(i != lqid) {
				LQ[i]->magic_inject(lqid, rx, ry, rz);
			}
		}
	}

public:
	void LX(int lqid) {
		/* logical X operation for this logical qubit */
		LQ[lqid]->LX();

		/* physical X operation for neighbor magic qubits */
		for(int i=0; i<lq_num; i++) {
			if(i != lqid) {
				LQ[i]->magic_X(lqid);
			}
		}
	}

	void LZ(int lqid) {
		/* logical Z operation for this logical qubit */
		LQ[lqid]->LZ();

		/* physical Z operation for neighbor magic qubits */
		for(int i=0; i<lq_num; i++) {
			if(i != lqid) {
				LQ[i]->magic_Z(lqid);
			}
		}
	}

	void LH(int lqid) {
		/* logical H operation for this logical qubit */
		LQ[lqid]->LH();

		/* physical H operation for neighbor magic qubits */
		for(int i=0; i<lq_num; i++) {
			if(i != lqid) {
				LQ[i]->magic_H(lqid);
			}
		}
	}

	int LMZ(int lqid) {
		int mv = 0;

		/* Z-basis measurement of this logical logical qubit */
		mv = LQ[lqid]->LMZ();

		/* forced collpase of neighbor magic qubits */
		for(int i=0; i<lq_num; i++) {
			if(i != lqid) {
				LQ[i]->magic_collapse(lqid, mv);
			}
		}

		normalize();

		return mv;
	}

	int LMX(int lqid) {
		int mv = 0;

		/* X-basis measurement of this logical logical qubit */
		mv = LQ[lqid]->LMX();

		/* forced collpase of neighbor magic qubits */
		for(int i=0; i<lq_num; i++) {
			if(i != lqid) {
				LQ[i]->magic_collapse(lqid, mv);
			}
		}

		return mv;
	}

	void LCNOT(int id_A, int id_C, int id_T) {
		/* logical CNOT operation for this logical qubit */
		logicalCNOT *mergeLQ = NULL;
		logicalQubit *AQ = LQ[id_A];
		logicalQubit *CQ = LQ[id_C];
		logicalQubit *TQ = LQ[id_T];
		int mxx1, mxx2;
		int mzz1, mzz2;
		int mx;

		QTimer timer;

		timer.start();
		mergeLQ = new logicalCNOT(CQ, TQ);
		timer.end();
		printf("[composite  ] Elapsed Time : %s\n", timer.getTime());

		timer.start();
		mergeLQ->latticeSurgery(AQ);
		timer.end();
		printf("[logicalCNOT] Elapsed Time : %s\n", timer.getTime());

		timer.start();
		mergeLQ->decompose();
		timer.end();
		printf("[decomposite] Elapsed Time : %s\n", timer.getTime());
		delete mergeLQ;

		/* physical CNOT operation for neighbor magic qubits */
		timer.start();
		for(int i=0; i<lq_num; i++) {
			if((i != id_C) && (i != id_T)) {
				LQ[i]->magic_CNOT(id_C, id_T);
			}
		}
		timer.end();
		printf("[magicCX] Elapsed Time : %s\n", timer.getTime());
	}

	void LS(int lqid, int lqid_A) {
		/* logical S operation for this logical qubit */
		logicalST *ST = new logicalST(LQ[lqid], LQ[lqid_A]);
		ST->logicalS();
		delete ST;

		/* physical S operation for neighbor magic qubits */
		for(int i=0; i<lq_num; i++) {
			if(i != lqid) {
				LQ[i]->magic_S(lqid);
			}
		}
	}

	void LT(int lqid, int lqid_A) {
		/* logical T operation for this logical qubit */
		logicalST *ST = new logicalST(LQ[lqid], LQ[lqid_A]);
		ST->logicalT();
		delete ST;

		/* physical T operation for neighbor magic qubits */
		for(int i=0; i<lq_num; i++) {
			if(i != lqid) {
				LQ[i]->magic_T(lqid);
			}
		}
	}
};

#endif
