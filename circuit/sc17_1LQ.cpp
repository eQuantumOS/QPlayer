#include <stdio.h>

#include "qplayer.h"

typedef enum {
	Q0, Q1, Q2, Q3, Q4, Q5, Q6, Q7, Q8, Q9,
	Q10, Q11, Q12, Q13, Q14, Q15, Q16, Q17, Q18, Q19,
	Q20, Q21, Q22, Q23, Q24, Q25
} QUBIT_INDEX;

typedef enum { Z_BASIS = 0, X_BASIS } MEASURE_TYPE;
typedef enum { X_LEFT = 0, X_RIGHT } FLAVOUR;

struct lqubit_info {
	int id;
	int flavor;
	int row;
	int col;
	int pq_num;
	int qubits[128];
};

struct stabilizer_circuit {
	int sq_index;
	int sq_mval;
	int dq_size;
	int dq_index[4];
};

static int x_decoder[2][16] = {
	{
		 /* flavor x-left */
		 -1,	/* 0000 */
		 Q20,	/* 0001 */
		 Q18,	/* 0010 */
		 Q19,	/* 0011 */
		 Q6,	/* 0100 */
		 Q6,	/* 0101 */
		 Q12,	/* 0110 */
		 Q12,	/* 0111 */
		 Q4,	/* 1000 */
		 Q4,	/* 1001 */
		 Q4,	/* 1010 */
		 Q19,	/* 1011 */
		 Q5,	/* 1100 */
		 Q5,	/* 1101 */
		 Q5,	/* 1110 */
		 Q5		/* 1111 */
	},
	{
		 /* flavor x-right */
		 -1,	/* 0000 */
		 Q18,	/* 0001 */
		 Q20,	/* 0010 */
		 Q19,	/* 0011 */
		 Q4,	/* 0100 */
		 Q4,	/* 0101 */
		 Q12,	/* 0110 */
		 Q12,	/* 0111 */
		 Q6,	/* 1000 */
		 Q6,	/* 1001 */
		 Q6,	/* 1010 */
		 Q19,	/* 1011 */
		 Q5,	/* 1100 */
		 Q5,	/* 1101 */
		 Q5,	/* 1110 */
		 Q5		/* 1111 */
	}
};

static int z_decoder[2][16] = {
	{
		 /* flavor x-left */
		 -1,	/* 0000 */
		 Q6,	/* 0001 */
		 Q20,	/* 0010 */
		 Q13,	/* 0011 */
		 Q4,	/* 0100 */
		 Q4,	/* 0101 */
		 Q12,	/* 0110 */
		 Q12,	/* 0111 */
		 Q18,	/* 1000 */
		 Q18,	/* 1001 */
		 Q18,	/* 1010 */
		 Q13,	/* 1011 */
		 Q11,	/* 1100 */
		 Q11,	/* 1101 */
		 Q11,	/* 1110 */
		 Q11	/* 1111 */
	},
	{
		 /* flavor x-right */
		 -1,	/* 0000 */
		 Q20,	/* 0001 */
		 Q6,	/* 0010 */
		 Q13,	/* 0011 */
		 Q18,	/* 0100 */
		 Q18,	/* 0101 */
		 Q12,	/* 0110 */
		 Q12,	/* 0111 */
		 Q4,	/* 1000 */
		 Q4,	/* 1001 */
		 Q4,	/* 1010 */
		 Q13,	/* 1011 */
		 Q11,	/* 1100 */
		 Q11,	/* 1101 */
		 Q11,	/* 1110 */
		 Q11	/* 1111 */
	}
};

struct lqubit_info lqs1 = {
	0,		/* LQ identifier */
	X_LEFT,	/* flavor */
	1,		/* row in lattice */
	1,		/* col in lattice */
	25,		/* number of data qubits */
	{
#if 0
	    0,    1,    2,      3,
  	       4,    5,     6,
	    7,    8,    9,     10,
	      11,    12,   13,
	   14,   15,     16,   17,
	      18,    19,   20,
	   21,   22,    23,    24 
#else
  	    9,    10,    11,    12,
  	       0,     1,     2,
	   13,    14,    15,    16,
	       3,     4,     5,
	   17,    18,    19,    20,
	       6,     7,     8,
	   21,    22,    23,    24 
#endif
	}

};

class logicalQubit {
  public:
	QRegister * QReg;

	struct lqubit_info *LQ;	/* all logical qubits info */

	int flavor;					/* flavor of this LQ */
	int lq_id;					/* identifier of this LQ */
	int lq_num;					/* number of total LQ */
	int row;					/* row number in lattice */
	int col;					/* col number in lattice */

	int dq_list[9];				/* data PQ list in this LQ */
	int logical_x[3];			/* logical X operator */
	int logical_z[3];			/* logical Z operator */

	struct stabilizer_circuit xs_circuit[4];	/* X-stabizer circuit */
	struct stabilizer_circuit zs_circuit[4];	/* Z-stabizer circuit */

  public:
	 logicalQubit(struct lqubit_info *lqinfo) {
		/*************************************/
		/* STEP1: set global parameters      */
		/*************************************/
		LQ = (struct lqubit_info *)malloc(sizeof(struct lqubit_info));
		 memcpy(LQ, lqinfo, sizeof(struct lqubit_info));

		 lq_id = LQ->id;
		 flavor = LQ->flavor;
		 row = LQ->row;
		 col = LQ->col;

		/*************************************/
		/* STEP2: allocate quantum register  */
		/*************************************/
		 QReg = new QRegister(LQ->pq_num);

		/*************************************/
		/* STEP3: set data qubit index       */
		/*************************************/
		 dq_list[0] = Q4;
		 dq_list[1] = Q5;
		 dq_list[2] = Q6;
		 dq_list[3] = Q11;
		 dq_list[4] = Q12;
		 dq_list[5] = Q13;
		 dq_list[6] = Q18;
		 dq_list[7] = Q19;
		 dq_list[8] = Q20;

		/*************************************/
		/* STEP4: set logical operator index */
		/*************************************/
		 logical_x[0] = Q5;
		 logical_x[1] = Q12;
		 logical_x[2] = Q19;

		 logical_z[0] = Q11;
		 logical_z[1] = Q12;
		 logical_z[2] = Q13;

		/*************************************/
		/* STEP5: set stabilizer circuit     */
		/*************************************/
		if (flavor == X_LEFT) {
			/* x-stabilizer */
			xs_circuit[0].sq_index = Q1;
			xs_circuit[0].dq_size = 2;
			xs_circuit[0].dq_index[0] = Q4;
			xs_circuit[0].dq_index[1] = Q5;

			xs_circuit[1].sq_index = Q9;
			xs_circuit[1].dq_size = 4;
			xs_circuit[1].dq_index[0] = Q5;
			xs_circuit[1].dq_index[1] = Q6;
			xs_circuit[1].dq_index[2] = Q12;
			xs_circuit[1].dq_index[3] = Q13;

			xs_circuit[2].sq_index = Q15;
			xs_circuit[2].dq_size = 4;
			xs_circuit[2].dq_index[0] = Q11;
			xs_circuit[2].dq_index[1] = Q12;
			xs_circuit[2].dq_index[2] = Q18;
			xs_circuit[2].dq_index[3] = Q19;

			xs_circuit[3].sq_index = Q23;
			xs_circuit[3].dq_size = 2;
			xs_circuit[3].dq_index[0] = Q19;
			xs_circuit[3].dq_index[1] = Q20;

			/* z-stabilizer */
			zs_circuit[0].sq_index = Q14;
			zs_circuit[0].dq_size = 2;
			zs_circuit[0].dq_index[0] = Q11;
			zs_circuit[0].dq_index[1] = Q18;

			zs_circuit[1].sq_index = Q8;
			zs_circuit[1].dq_size = 4;
			zs_circuit[1].dq_index[0] = Q4;
			zs_circuit[1].dq_index[1] = Q5;
			zs_circuit[1].dq_index[2] = Q11;
			zs_circuit[1].dq_index[3] = Q12;

			zs_circuit[2].sq_index = Q16;
			zs_circuit[2].dq_size = 4;
			zs_circuit[2].dq_index[0] = Q12;
			zs_circuit[2].dq_index[1] = Q13;
			zs_circuit[2].dq_index[2] = Q19;
			zs_circuit[2].dq_index[3] = Q20;

			zs_circuit[3].sq_index = Q10;
			zs_circuit[3].dq_size = 2;
			zs_circuit[3].dq_index[0] = Q6;
			zs_circuit[3].dq_index[1] = Q13;
		} else {
			/* x-stabilizer */
			xs_circuit[0].sq_index = Q2;
			xs_circuit[0].dq_size = 2;
			xs_circuit[0].dq_index[0] = Q5;
			xs_circuit[0].dq_index[1] = Q6;

			xs_circuit[1].sq_index = Q8;
			xs_circuit[1].dq_size = 4;
			xs_circuit[1].dq_index[0] = Q4;
			xs_circuit[1].dq_index[1] = Q5;
			xs_circuit[1].dq_index[2] = Q11;
			xs_circuit[1].dq_index[3] = Q12;

			xs_circuit[2].sq_index = Q16;
			xs_circuit[2].dq_size = 4;
			xs_circuit[2].dq_index[0] = Q12;
			xs_circuit[2].dq_index[1] = Q13;
			xs_circuit[2].dq_index[2] = Q19;
			xs_circuit[2].dq_index[3] = Q20;

			xs_circuit[3].sq_index = Q22;
			xs_circuit[3].dq_size = 2;
			xs_circuit[3].dq_index[0] = Q18;
			xs_circuit[3].dq_index[1] = Q19;

			/* z-stabilizer */
			zs_circuit[0].sq_index = Q7;
			zs_circuit[0].dq_size = 2;
			zs_circuit[0].dq_index[0] = Q4;
			zs_circuit[0].dq_index[1] = Q11;

			zs_circuit[1].sq_index = Q15;
			zs_circuit[1].dq_size = 4;
			zs_circuit[1].dq_index[0] = Q11;
			zs_circuit[1].dq_index[1] = Q12;
			zs_circuit[1].dq_index[2] = Q18;
			zs_circuit[1].dq_index[3] = Q19;

			zs_circuit[2].sq_index = Q9;
			zs_circuit[2].dq_size = 4;
			zs_circuit[2].dq_index[0] = Q5;
			zs_circuit[2].dq_index[1] = Q6;
			zs_circuit[2].dq_index[2] = Q12;
			zs_circuit[2].dq_index[3] = Q13;

			zs_circuit[3].sq_index = Q17;
			zs_circuit[3].dq_size = 2;
			zs_circuit[3].dq_index[0] = Q13;
			zs_circuit[3].dq_index[1] = Q20;
		}
	}

	~logicalQubit(void) {
		free(LQ);
		delete QReg;
	}

	int getId(void) {
		return lq_id;
	}
	int getTotalPQSize(void) {
		return LQ->pq_num;
	}
	int getPQSize(void) {
		return LQ->pq_num;
	}
	int getPQ(int index) {
		return LQ->qubits[index];
	}
	void dump(void) {
		showQState(QReg);
	}

  public:
	void LX(void) {
		/* logical-x operation */
		for (int i = 0; i < 3; i++) {
			X(QReg, getPQ(logical_x[i]));
		}
	}

	void LZ(void) {
		/* logical-z operation */
		for (int i = 0; i < 3; i++) {
			Z(QReg, getPQ(logical_z[i]));
		}
	}

	void LH(void) {
		/* logical hadamard operation */
		for (int i = 0; i < 9; i++) {
			H(QReg, getPQ(dq_list[i]));
		}

		/* switch stabilizer circuit */
		struct stabilizer_circuit tmp_circuit[4];
		memcpy(tmp_circuit, xs_circuit, sizeof(tmp_circuit));
		memcpy(xs_circuit, zs_circuit, sizeof(tmp_circuit));
		memcpy(zs_circuit, tmp_circuit, sizeof(tmp_circuit));

		flavor = (flavor == X_LEFT ? X_RIGHT : X_LEFT);

		runStabilizer();
	}

	int LMZV(void) {
		int mv = 1;
		for (int i = 0; i < 9; i++) {
			if (M(QReg, getPQ(dq_list[i])) == 1) {
				mv *= -1;
			}
		}

		return mv;
	}

	int LMXV(void) {
		for (int i = 0; i < 9; i++) {
			H(QReg, getPQ(dq_list[i]));
		}
		return LMZV();
	}

	int LMZ(void) {
		return LMZV() == 1 ? 0 : 1;
	}

	int LMX(void) {
		return LMXV() == 1 ? 0 : 1;
	}

  public:
	int genStabilizerStates(struct stabilizer_circuit *circuit) {
		qsize_t states = 0;

		for (int i = 0; i < 4; i++) {
			if (circuit[i].sq_mval == 1) {
				states |= quantum_shiftL(1, 3 - i);
			}
		}

		return (int)states;
	}

	void ESM(void) {
		/* build x-stabilizer circuit */
		for (int i = 0; i < 4; i++) {
			initZ(QReg, getPQ(xs_circuit[i].sq_index));
			H(QReg, getPQ(xs_circuit[i].sq_index));
			for (int j = 0; j < xs_circuit[i].dq_size; j++) {
				CX(QReg, getPQ(xs_circuit[i].sq_index), getPQ(xs_circuit[i].dq_index[j]));
			}
			H(QReg, getPQ(xs_circuit[i].sq_index));
			xs_circuit[i].sq_mval = M(QReg, getPQ(xs_circuit[i].sq_index));
		}

		/* build z-stabilizer circuit */
		for (int i = 0; i < 4; i++) {
			initZ(QReg, getPQ(zs_circuit[i].sq_index));
			for (int j = 0; j < zs_circuit[i].dq_size; j++) {
				CX(QReg, getPQ(zs_circuit[i].dq_index[j]), getPQ(zs_circuit[i].sq_index));
			}
			zs_circuit[i].sq_mval = M(QReg, getPQ(zs_circuit[i].sq_index));
		}
	}

	void ErrorCorrection(void) {
		int round = 3;

		for (int i = 0; i < round; i++) {
			int xs_states = genStabilizerStates(xs_circuit);
			int zs_states = genStabilizerStates(zs_circuit);

			if (xs_states == 0 && zs_states == 0) {
				break;
			}

			/* Z error correction */
			if (xs_states != 0) {
				Z(QReg, getPQ(x_decoder[flavor][xs_states]));
			}

			/* X error correction */
			if (zs_states != 0) {
				X(QReg, getPQ(z_decoder[flavor][zs_states]));
			}

			ESM();
		}
	}

	void runStabilizer(void) {
		ESM();
		ErrorCorrection();
	}

  public:
	void clear(void) {
		for (int i = 0; i < getPQSize(); i++) {
			initZ(QReg, getPQ(i));
		}
	}

	void init(int mode) {
		for (int i = 0; i < getPQSize(); i++) {
			initZ(QReg, getPQ(i));
		}

		if (mode == KET_LZERO) {
			runStabilizer();
		} else if (mode == KET_LONE) {
			runStabilizer();
			LX();
		} else if (mode == KET_LPLUS) {
			for (int i = 0; i < 9; i++) {
				H(QReg, getPQ(dq_list[i]));
			}
			runStabilizer();
		} else if (mode == KET_LMINUS) {
			for (int i = 0; i < 9; i++) {
				H(QReg, getPQ(dq_list[i]));
			}
			runStabilizer();
			LZ();
		}
	}
};

void CASE_ESM(logicalQubit *LQ)
{
	printf("[%s] build ESM circuit.\n", __func__);
	LQ->runStabilizer();
}

void CASE_LX(logicalQubit *LQ)
{
	printf("[%s] set logical X to LQ\n", __func__);
	LQ->LX();
}

void CASE_LZ(logicalQubit *LQ)
{
	printf("[%s] set logical Z to LQ\n", __func__);
	LQ->LZ();
}

void CASE_LH(logicalQubit *LQ)
{
	printf("[%s] set logical H to LQ\n", __func__);
	LQ->LH();
	LQ->runStabilizer();
}

void CASE_XERROR(logicalQubit *LQ)
{
	printf("[%s] set X error to LQ\n", __func__);
	X(LQ->QReg, LQ->getPQ(LQ->dq_list[4]));	
}

void CASE_ZERROR(logicalQubit *LQ)
{
	printf("[%s] set z error to LQ\n", __func__);
	Z(LQ->QReg, LQ->getPQ(LQ->dq_list[4]));	
}

void CASE_XERROR_CORRECTION(logicalQubit *LQ)
{
	printf("[%s] correct X error of LQ\n", __func__);
	LQ->runStabilizer();
}

void CASE_ZERROR_CORRECTION(logicalQubit *LQ)
{
	printf("[%s] correct Z error of LQ\n", __func__);
	LQ->runStabilizer();
}

int main(int argc, char **argv)
{
	logicalQubit *LQ = new logicalQubit(&lqs1);
	
	printf("initialize LQ to |0>\n");
	LQ->init(KET_LZERO);

	do {
		printf("\n======================================\n");
		printf("           select test case           \n");
		printf("======================================\n");
		printf("  1. normal surface code cycle\n");
		printf("  2. apply logical X\n");
		printf("  3. apply logical Z\n");
		printf("  4. apply logical Hadamard\n");
		printf("  5. data qubit X error\n");
		printf("  6. data qubit Z error\n");
		printf("  7. data qubit X error correction\n");
		printf("  8. data qubit Z error correction\n");
		printf("  0. quit\n");

		/* Initialize QRegister */
		char c = getchar();
		switch(c) {
			case '0' : return 0;
			case '1' : CASE_ESM(LQ); break;
			case '2' : CASE_LX(LQ); break;
			case '3' : CASE_LZ(LQ); break;
			case '4' : CASE_LH(LQ); break;
			case '5' : CASE_XERROR(LQ); break;
			case '6' : CASE_ZERROR(LQ); break;
			case '7' : CASE_XERROR_CORRECTION(LQ); break;
			case '8' : CASE_ZERROR_CORRECTION(LQ); break;
		}

		LQ->dump();
		
		/* this is for ignoring carrige return */
		c = getchar();
	} while(1);

	return 0;
}

