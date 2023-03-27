#ifndef __DS_H__
#define __DS_H__

#include <stdio.h>

/* 
 * We map the number of the qubits by assuming surface code 17. 
 * The numbers of individual physical qubits of logical qubit are 
 * converted into a single shape as follows. 
 */
typedef enum { 
	 Q0,  Q1,  Q2,  Q3,  Q4,  
	 Q5,  Q6,  Q7,  Q8,  Q9, 
	Q10, Q11, Q12, Q13, Q14, 
	Q15, Q16, Q17, Q18, Q19, 
	Q20, Q21, Q22, Q23, Q24
} QUBIT_INDEX;

typedef enum { Z_BASIS=0, X_BASIS } MEASURE_TYPE;
typedef enum { X_LEFT=0, X_RIGHT, Z_LEFT, Z_RIGHT } FLAVOUR;
typedef enum { LEFT_UP=0, RIGHT_UP, LEFT_DOWN, RIGHT_DOWN } MERGE_TYPE;

#define MAX_LQUBITS	 		25

struct lqubit_info {
	int id;
	int flavor;
	int row;
	int col;
	int pq_num;
	int qubits[128];
};

struct lqubits_info {
	int lq_num;
	int pq_num;
	struct lqubit_info lqubits[MAX_LQUBITS];
};

struct stabilizer_circuit {
	int sq_index;
	int sq_mval;
	int dq_size;
	int dq_index[4];
};

struct merge_circuit {
	int sq;
	int sq_mval;
	int dq_size;
	int dq_list[4];
};

static int x_decoder[2][16] = {
	{ 
		/* flavor x-left */
		-1,		/* 0000 */
		Q20,	/* 0001 */
		Q18,	/* 0010 */
		Q19,	/* 0011 */
		Q6,		/* 0100 */
		Q6,		/* 0101 */
		Q12,	/* 0110 */
		Q12,	/* 0111 */
		Q4,		/* 1000 */
		Q4,		/* 1001 */
		Q4,		/* 1010 */
		Q19,	/* 1011 */
		Q5,		/* 1100 */
		Q5,		/* 1101 */
		Q5,		/* 1110 */
		Q5		/* 1111 */
	},
	{
		/* flavor x-right */
		-1,		/* 0000 */
		Q18,	/* 0001 */
		Q20,	/* 0010 */
		Q19,	/* 0011 */
		Q4,		/* 0100 */
		Q4,		/* 0101 */
		Q12,	/* 0110 */
		Q12,	/* 0111 */
		Q6,		/* 1000 */
		Q6,		/* 1001 */
		Q6,		/* 1010 */
		Q19,	/* 1011 */
		Q5,		/* 1100 */
		Q5,		/* 1101 */
		Q5,		/* 1110 */
		Q5		/* 1111 */
	}
};
	
static int z_decoder[2][16] = {
	{ 
		/* flavor x-left */
		-1,		/* 0000 */
		Q6,		/* 0001 */
		Q20,	/* 0010 */
		Q13,	/* 0011 */
		Q4,		/* 0100 */
		Q4,		/* 0101 */
		Q12,	/* 0110 */
		Q12,	/* 0111 */
		Q18,	/* 1000 */
		Q18,	/* 1001 */
		Q18,	/* 1010 */
		Q13,	/* 1011 */
		Q11,	/* 1100 */
		Q11,	/* 1101 */
		Q11,	/* 1110 */
		Q11		/* 1111 */
	},
	{
		/* flavor x-right */
		-1,		/* 0000 */
		Q20,	/* 0001 */
		Q6,		/* 0010 */
		Q13,	/* 0011 */
		Q18,	/* 0100 */
		Q18,	/* 0101 */
		Q12,	/* 0110 */
		Q12,	/* 0111 */
		Q4,		/* 1000 */
		Q4,		/* 1001 */
		Q4,		/* 1010 */
		Q13,	/* 1011 */
		Q11,	/* 1100 */
		Q11,	/* 1101 */
		Q11,	/* 1110 */
		Q11		/* 1111 */
	}
};

extern struct qubit_delimiter qd;

#endif
