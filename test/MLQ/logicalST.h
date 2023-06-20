#ifndef __LOGICAL_ST_H__
#define __LOGICAL_ST_H__

#include <stdio.h>

#include "qplayer.h"
#include "logicalQubit.h"

class logicalST {
public:
	logicalQubit *LQ;
	logicalQubit *AQ;

	logicalQubit *LQHigh;
	logicalQubit *LQLow;

	int mzz;
	int mx;

public:
	logicalST(logicalQubit *_lq, logicalQubit *_aq) {
		/***************************************/
		/* STEP1: allocate merged Q-Register   */
		/***************************************/
		LQ = _lq;
		AQ = _aq;

		/***************************************/
		/* STEP2: set LQs in qubit order       */
		/***************************************/
		if(LQ->getPQ(LQ->dq_list[0]) < AQ->getPQ(AQ->dq_list[0])) {
			LQLow = LQ;
			LQHigh = AQ;
		} else {
			LQLow = AQ;
			LQHigh = LQ;
		}
	}

	~logicalST(void) { }

private:
	void inject_Y(void) {
		for(int i=0; i<AQ->getPQSize(); i++) {
			initZ(LQ->QReg, AQ->getPQ(i));
		}

		/* set seed to center PQ */
		H(LQ->QReg, AQ->getPQ(Q12));
		S(LQ->QReg, AQ->getPQ(Q12));

		/* CNOT along with X operator */
		CX(LQ->QReg, AQ->getPQ(Q12), AQ->getPQ(Q5));
		CX(LQ->QReg, AQ->getPQ(Q12), AQ->getPQ(Q19));

		AQ->runStabilizer(LQ->QReg);
	}

	void inject_A(void) {
		for(int i=0; i<AQ->getPQSize(); i++) {
			initZ(LQ->QReg, AQ->getPQ(i));
		}

		/* set seed to center PQ */
		H(LQ->QReg, AQ->getPQ(Q12));
		T(LQ->QReg, AQ->getPQ(Q12));

		/* CNOT along with X operator */
		CX(LQ->QReg, AQ->getPQ(Q12), AQ->getPQ(Q5));
		CX(LQ->QReg, AQ->getPQ(Q12), AQ->getPQ(Q19));

		AQ->runStabilizer(LQ->QReg);
	}

	/* As the goal is to verify logical S or T operations, test qubit numbering 
       is mapped statically. In other words, dynamic logical S or T operations 
       based on the flavor of logical qubits are not supported. Of course, 
       this is not impossible, but the code will not be further improved for 
       the time being. 
	*/
	void merge_ZZ(void) {
		int mz1 = 0;
		int mz2 = 0;	
		int mx1 = 0;

		/* Z-merge #1 */ 
		initZ(LQ->QReg, LQ->getPQ(Q21));
		CX(LQ->QReg, LQ->getPQ(Q18), LQ->getPQ(Q21));
		CX(LQ->QReg, AQ->getPQ(Q4), AQ->getPQ(Q0));

		/* Z-merge #2 */ 
		initZ(LQ->QReg, LQ->getPQ(Q23));
		CX(LQ->QReg, LQ->getPQ(Q19), LQ->getPQ(Q23));
		CX(LQ->QReg, LQ->getPQ(Q20), LQ->getPQ(Q23));
		CX(LQ->QReg, AQ->getPQ(Q5), AQ->getPQ(Q0));
		CX(LQ->QReg, AQ->getPQ(Q6), AQ->getPQ(Q2));

		/* split */
		mz1 = M(LQ->QReg, LQ->getPQ(Q21)) == 0 ? 1 : -1;
		mz2 = M(LQ->QReg, LQ->getPQ(Q23)) == 0 ? 1 : -1;

		/* X-measure ancilla */
		AQ->runStabilizer(LQ->QReg);
		LQ->runStabilizer();
		mx1 = AQ->LMX(LQ->QReg) == 0 ? 1 : -1;

		/* set measured value for post operation */
		mzz = mz1 * mz2;
		mx = mx1;
	}

	void do_post_S(void) {
		printf("[%s] mzz(%d), mx(%d)\n", __FUNCTION__, mzz, mx);
		if(mzz == -1) LQ->LZ();
		if(mx == -1) LQ->LZ();
	}

	void do_post_T(void) {
		printf("[%s] mzz(%d), mx(%d)\n", __FUNCTION__, mzz, mx);
		if(mzz == 1) {
			if(mx == -1) {
				LQ->LZ();
			}
		} else {
			if(mx == 1) {
				logicalS();
			} else {
				LQ->LZ();
				logicalS();
			}
		}
	}

public:
	void logicalS(void) {
		/* inject ancilla states to |Y> */
		inject_Y();

		/* do Mzz operation */
		merge_ZZ();

		/* do post operation */
		do_post_S();
	}

	void logicalT(void) {
		/* inject ancilla states to |A> */
		inject_A();

		/* do Mzz operation */
		merge_ZZ();

		/* do post operation */
		do_post_T();

		LQ->runStabilizer();
	}
};

#endif
