#include <stdio.h>

#include "qplayer.h"

using namespace std;

typedef enum { 
	 Q0,  Q1,  Q2,  Q3,  Q4,  Q5,  Q6,  Q7,  Q8,  Q9,
	Q10, Q11, Q12, Q13, Q14, Q15, Q16, Q17, Q18, Q19,
	Q20, Q21, Q22, Q23, Q24, Q25
} QUBIT_INDEX;

typedef enum { AX=0, AZ } MERGE_TYPE;
typedef enum { Z_BASIS=0, X_BASIS } MEASURE_TYPE;
typedef enum { X_LEFT=0, X_RIGHT } FLAVOUR;

struct qubit_list {
	int size;
	int qubit_number[128];
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
		Q8,		/* 1011 */
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
		Q4		/* 1111 */
	}
};

struct qubit_list LQ0_qubits = {
	25,
	{
		36,      37,      38,      39,
		     0,       1,       2,
		43,      44,      45,      46,
		     3,       4,       5,
		50,      51,      52,      53,
		     6,       7,       8,
		57,      58,      59,      60
	}
};

struct qubit_list LQ1_qubits = {
	25,
	{
		39,       40,       41,       42,
		      9,       10,       11,
		46,       47,       48,       49,
		     12,       13,       14,
		53,       54,       55,       56,
		     15,       16,       17,
		60,       61,       62,       63
	}
};

struct qubit_list LQ2_qubits = {
	25,
	{
		57,       58,       59,       60,
		     18,       19,       20,
		64,       65,       66,       67,
		     21,       22,       23,
		71,       72,       73,       74,
		     24,       25,       26,
		78,       79,       80,       81
	}
};

struct qubit_list LQ3_qubits = {
	25,
	{
		60,       62,       63,       64,
		     27,       28,       29,
		67,       68,       69,       70,
		     30,       31,       32,
		74,       75,       76,       77,
		     33,       34,       35,
		81,       82,       83,       84
	}
};

class logicalQubit {
public:
	QRegister *QReg;
	char name[16];
	int flavor;

public:
	int qubit_size;
	int dq_size;
	int sq_size;
	int qubit_list[25];
	int dq_list[9];
	int logical_x[3];
	int logical_z[3];
	struct stabilizer_circuit xs_circuit[4];
	struct stabilizer_circuit zs_circuit[4];

public:
	logicalQubit(QRegister *_QReg, char *_name, int _flavor, struct qubit_list *qubits) { 
		/* set logical qubit catalog */
		QReg = _QReg;
		strcpy(name, _name);
		flavor = _flavor;
		qubit_size = qubits->size;
		dq_size = 9;
		sq_size = 4;

		/* set all qubits */
		for(int i=0; i<qubits->size; i++) {
			qubit_list[i] = qubits->qubit_number[i];
		}

		/* set data qubit index */
		dq_list[0] = Q4;
		dq_list[1] = Q5;
		dq_list[2] = Q6;
		dq_list[3] = Q11;
		dq_list[4] = Q12;
		dq_list[5] = Q13;
		dq_list[6] = Q18;
		dq_list[7] = Q19;
		dq_list[8] = Q20;

		/* set logical-x/z operator index */
		logical_x[0] = Q5;
		logical_x[1] = Q12;
		logical_x[2] = Q19;

		logical_z[0] = Q11;
		logical_z[1] = Q12;
		logical_z[2] = Q13;

		/* set stabilizer circuit index */
		if(flavor == X_LEFT) {
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

	~logicalQubit(void) { }

	int QUBIT(int index) { return qubit_list[index]; }

public:
	void logical_XOP(void) {
		for(int i=0; i<3; i++) {
			X(QReg, QUBIT(logical_x[i]));
		}
	}

	void logical_ZOP(void) {
		for(int i=0; i<3; i++) {
			Z(QReg, QUBIT(logical_z[i]));
		}
	}

	void logical_HOP(void) {
		for(int i=0; i<dq_size; i++) {
			H(QReg, QUBIT(dq_list[i]));
		}
	}

public:
	int logical_MZV(void) {
		int mv = 1;

		for(int i=0; i<dq_size; i++) {
			if(M(QReg, QUBIT(dq_list[i])) == 1) {
				mv *= -1;
			}
		}

		return mv;
	}

	int logical_MXV(void) {
		for(int i=0; i<dq_size; i++) {
			H(QReg, QUBIT(dq_list[i]));
		}

		return logical_MZV();
	}

	int logical_MZ(void) {
		int mv = logical_MZV();
		return mv == 1 ? 0 : 1;
	}

	int logical_MX(void) {
		int mv = logical_MXV();
		return mv == 1 ? 0 : 1;
	}

public:
	int genStabilizerStates(struct stabilizer_circuit *circuit) {
		int states = 0;

		for(int i=0; i<4; i++) {
			if(circuit[i].sq_mval == 1) {
				states |= (1 << (3 - i));
			}
		}

		return states;
	}

	void printStabilizerStates(char *type, int states) {
		printf("Type(%s) : ", type);

		int flag = 8;
		do {
			if(states&flag) printf("1");
			else printf("0");
			flag/=2;
		} while(flag >= 1);
		printf(">\n");
	}

	void ESM(void) {
		/* build x-stabilizer circuit */
		for(int i=0; i<4; i++) {
			initZ(QReg, QUBIT(xs_circuit[i].sq_index));
			H(QReg, QUBIT(xs_circuit[i].sq_index));
			for(int j=0; j<xs_circuit[i].dq_size; j++) {
				CX(QReg, QUBIT(xs_circuit[i].sq_index), QUBIT(xs_circuit[i].dq_index[j]));
			}
			H(QReg, QUBIT(xs_circuit[i].sq_index));
			xs_circuit[i].sq_mval = M(QReg, QUBIT(xs_circuit[i].sq_index));
		}

		/* build x-stabilizer circuit */
		for(int i=0; i<4; i++) {
			initZ(QReg, QUBIT(zs_circuit[i].sq_index));
			for(int j=0; j<zs_circuit[i].dq_size; j++) {
				CX(QReg, QUBIT(zs_circuit[i].dq_index[j]), QUBIT(zs_circuit[i].sq_index));
			}
			zs_circuit[i].sq_mval = M(QReg, QUBIT(zs_circuit[i].sq_index));
		}
	}

	void ErrorCorrection(void) {
		int round = 3;

		for(int i=0; i<round; i++) {
			int xs_states = genStabilizerStates(xs_circuit);
			int zs_states = genStabilizerStates(zs_circuit);

			if(xs_states == 0 && zs_states == 0) {
				break;
			}

			/* Z error correction */
			if(xs_states != 0) {
				Z(QReg, qubit_list[x_decoder[flavor][xs_states]]);
			}

			/* X error correction */
			if(zs_states != 0) {
				X(QReg, qubit_list[z_decoder[flavor][zs_states]]);
			}

			ESM();
		}
	}

	void runStabilizer(void) {
		ESM();
		ErrorCorrection();
	}

public:
	void initLZero(void) {
		runStabilizer();
	}

	void initLOne(void) {
		runStabilizer();
		logical_XOP();
	}

	void initLPlus(void) {
		logical_HOP();
		runStabilizer();
	}

	void initLMinus(void) {
		initLPlus();
		logical_ZOP();
	}

	void initState(int mode) {
		for(int i=0; i<qubit_size; i++) {
			initZ(QReg, qubit_list[i]);
		}

		if(mode == KET_LZERO) {
			initLZero();
		} else if(mode == KET_LONE) {
			initLOne();
		} else if(mode == KET_LPLUS) {
			initLPlus();
		} else if(mode == KET_LMINUS) {
			initLMinus();
		} 
	}
};

class SC17_3LQ_CNOT {
public:
	QRegister *QReg;
	struct qubit_delimiter qd;

public:
	SC17_3LQ_CNOT(void) {
		QReg = new QRegister(85);
	} 

	~SC17_3LQ_CNOT(void) {
		delete QReg;
	}

public:
	int MergeSplitXX(logicalQubit *AQ, logicalQubit *LQ) {
		struct merge_circuit circuit[2] = {
			AQ->qubit_list[Q3],
			0,
			2,
			{
				AQ->qubit_list[Q6],
				LQ->qubit_list[Q4],
			},
			AQ->qubit_list[Q17],
			0,
			4,
			{
				AQ->qubit_list[Q13],
				AQ->qubit_list[Q20],
				LQ->qubit_list[Q11],
				LQ->qubit_list[Q18],
			}
		};

		/* build x-stabilizer circuit */
		for(int i=0; i<2; i++) {
			initZ(QReg, circuit[i].sq);
			H(QReg, circuit[i].sq);
			for(int j=0; j<circuit[i].dq_size; j++) {
				CX(QReg, circuit[i].sq, circuit[i].dq_list[j]);
			}
			H(QReg, circuit[i].sq);
			circuit[i].sq_mval = MV(QReg, circuit[i].sq);
		}

		AQ->runStabilizer();
		LQ->runStabilizer();

		return (circuit[0].sq_mval * circuit[1].sq_mval);
	}

	int MergeSplitZZ(logicalQubit *AQ, logicalQubit *LQ) {
		struct merge_circuit circuit[2] = {
			AQ->qubit_list[Q22],
			0,
			4,
			{
				AQ->qubit_list[Q18],
				AQ->qubit_list[Q19],
				LQ->qubit_list[Q4],
				LQ->qubit_list[Q5],
			},
			AQ->qubit_list[Q24],
			0,
			2,
			{
				AQ->qubit_list[Q20],
				LQ->qubit_list[Q6],
			}
		};

		/* build x-stabilizer circuit */
		for(int i=0; i<2; i++) {
			initZ(QReg, circuit[i].sq);
			for(int j=0; j<circuit[i].dq_size; j++) {
				CX(QReg, circuit[i].dq_list[j], circuit[i].sq);
			}
			circuit[i].sq_mval = MV(QReg, circuit[i].sq);
		}

		AQ->runStabilizer();
		LQ->runStabilizer();

		return (circuit[0].sq_mval * circuit[1].sq_mval);
	}

	void validateCNOT(int CQtype, logicalQubit *CQ, int TQtype, logicalQubit *TQ) {
		/* measure CQ, TQ followed by basis */
		int cq_measure_type;
		int tq_measure_type;
		int mc;
		int mt;
		if(CQtype == KET_LPLUS || CQtype == KET_LMINUS) {
			if(TQtype == KET_LONE || TQtype == KET_LZERO) {
				cq_measure_type = Z_BASIS;
				tq_measure_type = Z_BASIS;
			} else {
				cq_measure_type = X_BASIS;
				tq_measure_type = X_BASIS;
			}
		} else {
			if(TQtype == KET_LONE || TQtype == KET_LZERO) {
				cq_measure_type = Z_BASIS;
				tq_measure_type = Z_BASIS;
			} else {
				cq_measure_type = Z_BASIS;
				tq_measure_type = X_BASIS;
			}
		}

		if(cq_measure_type == Z_BASIS) {
			mc = CQ->logical_MZ();
		} else {
			mc = CQ->logical_MX();
		}

		if(tq_measure_type == Z_BASIS) {
			mt = TQ->logical_MZ();
		} else {
			mt = TQ->logical_MX();
		}

		/* validate CQ, TQ */
		printf("\n******************** RESULT ********************\n");
		printf("CNOT %s,%s\tStatus ", modeString(CQtype), modeString(TQtype));
		if(cq_measure_type == Z_BASIS) {
			if(mc == 0) {
				printf("CQ=|0>, ");
			} else {
				printf("CQ=|1>, ");
			}
		} else {
			if(mc == 0) {
				printf("CQ=|+>, ");
			} else {
				printf("CQ=|->, ");
			}
		}

		if(tq_measure_type == Z_BASIS) {
			if(mt == 0) {
				printf("TQ:|0>");
			} else {
				printf("TQ:|1>");
			}
		} else {
			if(mt == 0) {
				printf("TQ:|+>");
			} else {
				printf("TQ:|->");
			}
		}
		printf("\n");
	}

public:
	void cnot(void) {
		logicalQubit *AQ = new logicalQubit(QReg, "LQ0", X_LEFT, &LQ0_qubits);
		logicalQubit *TQ = new logicalQubit(QReg, "LQ1", X_RIGHT, &LQ1_qubits);
		logicalQubit *CQ = new logicalQubit(QReg, "LQ2", X_RIGHT, &LQ2_qubits);
		int cq_mode = KET_LPLUS;
		int tq_mode = KET_LZERO;

		qd.size = 3;
		qd.qubits[0] = AQ->QUBIT(AQ->dq_list[8]);
		qd.qubits[1] = CQ->QUBIT(CQ->dq_list[8]);
		qd.qubits[2] = TQ->QUBIT(TQ->dq_list[8]);

		/************************************/
		/* STEP1: initialize logical qubits */
		/************************************/
		AQ->initState(KET_ZERO);
		CQ->initState(cq_mode);
		TQ->initState(tq_mode);

		/************************************/
		/* STEP2: merge & split             */
		/************************************/
		int mxx = MergeSplitXX(AQ, TQ);
		int mzz = MergeSplitZZ(AQ, CQ);
		int mx = AQ->logical_MXV();

		/************************************/
		/* STEP3: post process              */
		/************************************/
		int a = (mxx == -1 ? 1 : 0);
		int b = (mzz == -1 ? 1 : 0);
		int c = (mx == -1 ? 1 : 0);

		if(a == 1) CQ->logical_ZOP();
		if(c == 1) CQ->logical_ZOP();
		if(b == 1) TQ->logical_XOP();

		/************************************/
		/* STEP4: validate status of CQ, TQ */
		/************************************/
		validateCNOT(cq_mode, CQ, tq_mode, TQ);
	}
};

int main(int argc, char **argv)
{
    SC17_3LQ_CNOT *CNOT = new SC17_3LQ_CNOT();
	QTimer timer;

	timer.start();

    CNOT->cnot();

	timer.end();
    printf("\nElapsed Time : %s\n", timer.getTime());
}
