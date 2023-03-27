#include <stdio.h>

#include "qplayer.h"
#include "lattice.h"
#include "logicalQubits.h"

/**************************************************/
/* Scenarios #1 : initialize logical qubits       */
/**************************************************/
void init_LQ4(void) {
	logicalQubits *MLQ = new logicalQubits(&lqs4);
	QTimer timer;
	int state = KET_LMINUS;

	timer.start();
	for(int i=0; i<MLQ->lq_num; i++) {
		MLQ->initLQ(i, state);
	}
	timer.end();

	MLQ->showQState();
	printf("[%s] Elapsed Time : %s\n", __FUNCTION__, timer.getTime());
	showMemoryInfo();
}

void init_LQ9(void) {
	logicalQubits *MLQ = new logicalQubits(&lqs9);
	QTimer timer;
	int state = KET_LMINUS;

	timer.start();
	for(int i=0; i<MLQ->lq_num; i++) {
		MLQ->initLQ(i, state);
	}
	timer.end();

	MLQ->showQState();
	printf("[%s] Elapsed Time : %s\n", __FUNCTION__, timer.getTime());
	showMemoryInfo();
}

void init_LQ16(void) {
	logicalQubits *MLQ = new logicalQubits(&lqs16);
	QTimer timer;
	int state = KET_LMINUS;

	timer.start();
	for(int i=0; i<MLQ->lq_num; i++) {
		MLQ->initLQ(i, state);
	}
	timer.end();

	MLQ->showQState();
	printf("[%s] Elapsed Time : %s\n", __FUNCTION__, timer.getTime());
	showMemoryInfo();
}

void init_LQ20(void) {
	logicalQubits *MLQ = new logicalQubits(&lqs20);
	QTimer timer;
	int state = KET_LMINUS;

	timer.start();
	for(int i=0; i<MLQ->lq_num; i++) {
		MLQ->initLQ(i, state);
	}
	timer.end();

	MLQ->showQState();
	printf("[%s] Elapsed Time : %s\n", __FUNCTION__, timer.getTime());
	showMemoryInfo();
}

/**************************************************/
/* Scenarios #2 : logical CNOT with LS            */
/**************************************************/
void cnot_LQ4(void) {
	logicalQubits *MLQ = new logicalQubits(&lqs4);
	QTimer timer;

	timer.start();

	MLQ->initLQ(0, KET_LPLUS);
	MLQ->initLQ(3, KET_LZERO);

	MLQ->LCNOT(2, 0, 3);		/* A, C, T */

	timer.end();

	MLQ->showQState();
	printf("[%s] Elapsed Time : %s\n", __FUNCTION__, timer.getTime());
	showMemoryInfo();
}

void cnot_LQ9(void) {
	logicalQubits *MLQ = new logicalQubits(&lqs9);
	QTimer timer;

	timer.start();

	MLQ->initLQ(4, KET_LPLUS);
	MLQ->initLQ(0, KET_LZERO);
	MLQ->initLQ(2, KET_LZERO);
	MLQ->initLQ(6, KET_LZERO);
	MLQ->initLQ(8, KET_LZERO);

	MLQ->LCNOT(1, 4, 0);		/* A, C, T */
	MLQ->LCNOT(1, 4, 2);		/* A, C, T */
	MLQ->LCNOT(7, 4, 6);		/* A, C, T */
	MLQ->LCNOT(7, 4, 8);		/* A, C, T */

	timer.end();

	MLQ->showQState();
	printf("[%s] Elapsed Time : %s\n", __FUNCTION__, timer.getTime());
	showMemoryInfo();
}

void cnot_LQ16(void) {
	logicalQubits *MLQ = new logicalQubits(&lqs16);
	QTimer timer;

	timer.start();

	MLQ->initLQ(0, KET_LPLUS);
	MLQ->initLQ(2, KET_LZERO);
	MLQ->initLQ(5, KET_LZERO);
	MLQ->initLQ(7, KET_LZERO);
	MLQ->initLQ(8, KET_LZERO);
	MLQ->initLQ(10, KET_LZERO);
	MLQ->initLQ(13, KET_LZERO);
	MLQ->initLQ(15, KET_LZERO);

	MLQ->LCNOT(1, 0, 5);		/* A, C, T */
	MLQ->LCNOT(1, 5, 2);		/* A, C, T */
	MLQ->LCNOT(6, 2, 7);		/* A, C, T */
	MLQ->LCNOT(11, 7, 10);		/* A, C, T */
	MLQ->LCNOT(14, 10, 13);		/* A, C, T */
	MLQ->LCNOT(14, 10, 15);		/* A, C, T */

	timer.end();

	MLQ->showQState();
	printf("[%s] Elapsed Time : %s\n", __FUNCTION__, timer.getTime());
	showMemoryInfo();
}

void cnot_LQ20(void) {
	logicalQubits *MLQ = new logicalQubits(&lqs20);
	QTimer timer;

	timer.start();

	MLQ->initLQ(0, KET_LPLUS);
	MLQ->initLQ(2, KET_LZERO);
	MLQ->initLQ(4, KET_LZERO);
	MLQ->initLQ(6, KET_LZERO);
	MLQ->initLQ(8, KET_LZERO);
	MLQ->initLQ(10, KET_LZERO);
	MLQ->initLQ(12, KET_LZERO);
	MLQ->initLQ(14, KET_LZERO);
	MLQ->initLQ(16, KET_LZERO);
	MLQ->initLQ(18, KET_LZERO);

	MLQ->LCNOT(5, 0, 6);		/* A, C, T */
	MLQ->LCNOT(1, 6, 2);		/* A, C, T */
	MLQ->LCNOT(7, 2, 8);		/* A, C, T */
	MLQ->LCNOT(3, 8, 4);		/* A, C, T */
	MLQ->LCNOT(13, 8, 14);		/* A, C, T */
	MLQ->LCNOT(19, 14, 18);		/* A, C, T */
	MLQ->LCNOT(11, 6, 10);		/* A, C, T */
	MLQ->LCNOT(11, 6, 12);		/* A, C, T */
	MLQ->LCNOT(15, 10, 16);		/* A, C, T */

	timer.end();

	MLQ->showQState();
	printf("[%s] Elapsed Time : %s\n", __FUNCTION__, timer.getTime());
	showMemoryInfo();
}

/***************************************************************/
/* Scenarios #3 : logical S and T gate                         */
/*                                                             */
/* NOTICE: It should be noted that the global phase varies     */ 
/* depending on the joint-measurement values.                  */   
/*                                                             */
/* For example, after performing a logical S operation, 3      */
/* different logical states can be obtained. However, if the   */
/* global phase is removed, all of these states are equivalent.*/
/*                                                             */
/* [Post operation after joint-measurement for logical-S]      */
/*  if Mzz = +1                                                */
/*     if Mx = +1,       (1) |psi> = a|0> + bi|1>              */
/*     else if Mx = -1,  (1) |psi> = a|0> + bi|1>              */
/*  else if Mzz = -1,                                          */
/*     if Mx = +1        (2) |psi> = ai|0> - b|1>              */
/*                                 = e^(i*pi/2)(ai|0> + b|1>)  */
/*     else if Mx = -1,  (3) |psi> = -ai|0> + b|1>             */
/*                                 = -e^(i*pi/2)(ai|0> + b|1>) */
/***************************************************************/
void S_LQ1(void) {
	logicalQubits *MLQ = new logicalQubits(&lqs4);

	MLQ->initLQ(0, KET_LPLUS);
	MLQ->LS(0, 2);				/* LQ, Ancilla(|Y>) */

	MLQ->showQState();
}

void T_LQ1(void) {
	logicalQubits *MLQ = new logicalQubits(&lqs4);

	MLQ->initLQ(0, KET_LPLUS);
	MLQ->LT(0, 2);				/* LQ, Ancilla(|A>) */

	MLQ->showQState();
}

/**************************************************/
/*               control function                 */
/**************************************************/
int main(int argc, char **argv)
{
	int mode = 1;

	if(mode == 0) {
		init_LQ4();
		// init_LQ9();
		// init_LQ16();
		// init_LQ20();
	} else if(mode == 1) {
		cnot_LQ4();
		// cnot_LQ9();
		// cnot_LQ16();
		// cnot_LQ20();
	} else if(mode == 2) {
		S_LQ1();
		T_LQ1();
	}

	return 0;
}
