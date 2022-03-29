#include <stdio.h>

#include "qplayer.h"

using namespace std;

typedef enum { D1=0, D2, D3, D4, D5, D6, D7, D8, D9 } DQ_SEQUENCE;
typedef enum { AX=0, AZ } MERGE_TYPE;

static int x_decoder[2][16] = {
	{ 
		{ -1 },		/* 0000 */
		{ D9 },		/* 0001 */
		{ D7 },		/* 0010 */
		{ D8 },		/* 0011 */
		{ D3 },		/* 0100 */
		{ D3 },		/* 0101 */
		{ D5 },		/* 0110 */
		{ D5 },		/* 0111 */
		{ D1 },		/* 1000 */
		{ D1 },		/* 1001 */
		{ D1 },		/* 1010 */
		{ D8 },		/* 1011 */
		{ D2 },		/* 1100 */
		{ D2 },		/* 1101 */
		{ D2 },		/* 1110 */
		{ D2 }		/* 1111 */
	},
	{
		{ -1 },		/* 0000 */
		{ D7 },		/* 0001 */
		{ D9 },		/* 0010 */
		{ D8 },		/* 0011 */
		{ D1 },		/* 0100 */
		{ D1 },		/* 0101 */
		{ D5 },		/* 0110 */
		{ D5 },		/* 0111 */
		{ D3 },		/* 1000 */
		{ D3 },		/* 1001 */
		{ D3 },		/* 1010 */
		{ D8 },		/* 1011 */
		{ D2 },		/* 1100 */
		{ D2 },		/* 1101 */
		{ D2 },		/* 1110 */
		{ D2 }		/* 1111 */
	}
};
	
static int z_decoder[2][16] = {
	{ 
		{ -1 },		/* 0000 */
		{ D3 },		/* 0001 */
		{ D9 },		/* 0010 */
		{ D6 },		/* 0011 */
		{ D1 },		/* 0100 */
		{ D1 },		/* 0101 */
		{ D5 },		/* 0110 */
		{ D5 },		/* 0111 */
		{ D7 },		/* 1000 */
		{ D7 },		/* 1001 */
		{ D7 },		/* 1010 */
		{ D6 },		/* 1011 */
		{ D4 },		/* 1100 */
		{ D4 },		/* 1101 */
		{ D4 },		/* 1110 */
		{ D4 }		/* 1111 */
	},
	{
		{ -1 },		/* 0000 */
		{ D9 },		/* 0001 */
		{ D3 },		/* 0010 */
		{ D6 },		/* 0011 */
		{ D7 },		/* 0100 */
		{ D7 },		/* 0101 */
		{ D5 },		/* 0110 */
		{ D5 },		/* 0111 */
		{ D1 },		/* 1000 */
		{ D1 },		/* 1001 */
		{ D1 },		/* 1010 */
		{ D6 },		/* 1011 */
		{ D4 },		/* 1100 */
		{ D4 },		/* 1101 */
		{ D4 },		/* 1110 */
		{ D4 }		/* 1111 */
	}
};

struct stabilizer_qubit {
	int type;
	int sq;
	int dq_num;
	int dq_list[4];
};

struct logical_qubit {
	char name[16];
	int flavor;

	/* data qubits */
	int dq_num;
	int dq_list[9];

	/* logical operator */
	int logical_x[3];
	int logical_z[3];

	/* stabilizer qubits */
	int ax_num;
	struct stabilizer_qubit ax_list[4];

	int az_num;
	struct stabilizer_qubit az_list[4];

	/* x, z stabilazer value : 0000 - 1111 */
	int ax;
	int az;

	/* measure value : +1 or -1 */
	int measure_value;
};

struct logical_merge_qubit {
	int num_stabilizer_qubit;
	struct stabilizer_qubit merged_qubit[3];
	int measure_value[3];	/* +1 or -1 */
};

struct logical_qubit _AQ = {
	"AQ",											/* name */
	0,												/* flavor */
	9,												/* number of data qubit */
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8 },					/* data qubit list */
	{ 1, 4, 7 },									/* logical X operator */
	{ 3, 4, 5 },									/* logical Z operator */
	4,												/* number of x-stabilizer */
	{ 
		{ AX, 27, 2, { D1, D2 } },					/* AX1 */
		{ AX, 28, 4, { D2, D3, D5, D6 } },			/* AX2 */
		{ AX, 29, 4, { D4, D5, D7, D8 } },			/* AX3 */
		{ AX, 30, 2, { D8, D9 } }					/* AX4 */
	},
	4,												/* number of z-stabilizer */
	{ 
		{ AZ, 31, 2, { D4, D7 } },					/* AZ1 */
		{ AZ, 32, 4, { D1, D2, D4, D5 } },			/* AZ2 */
		{ AZ, 33, 4, { D5, D6, D8, D9 } },			/* AZ3 */
		{ AZ, 34, 2, { D3, D6 } }					/* AZ4 */
	}, 
	0,
	0,
	0
};

struct logical_qubit _TQ = {
	"TQ",											/* name */
	1,												/* flavor */
	9,												/* number of data qubit */
	{ 9, 10, 11, 12, 13, 14, 15, 16, 17 },			/* data qubit list */
	{ 10, 13, 16 },									/* logical X operator */
	{ 12, 13, 14 },									/* logical Z operator */
	4,												/* number of x-stabilizer */
	{ 
		{ AX, 38, 2, { D2, D3 } },					/* AX1 */
		{ AX, 39, 4, { D1, D2, D4, D5 } },			/* AX2 */
		{ AX, 40, 4, { D5, D6, D8, D9 } },			/* AX3 */
		{ AX, 41, 2, { D7, D8 } }					/* AX4 */
	},
	4,												/* number of z-stabilizer */
	{ 
		{ AZ, 34, 2, { D1, D4 } },					/* AZ1 */
		{ AZ, 35, 4, { D4, D5, D7, D8 } },			/* AZ2 */
		{ AZ, 36, 4, { D2, D3, D5, D6 } },			/* AZ3 */
		{ AZ, 37, 2, { D6, D9 } }					/* AZ4 */
	}, 
	0,
	0,
	0
};

struct logical_qubit _CQ = {
	"CQ",											/* name */
	1,												/* flavor */
	9,												/* number of data qubit */
	{ 18, 19, 20, 21, 22, 23, 24, 25, 26 },			/* data qubit list */
	{ 19, 22, 25 },									/* logical X operator */
	{ 21, 22, 23 },									/* logical Z operator */
	4,												/* number of x-stabilizer */
	{ 
		{ AX, 30, 2, { D2, D3 } },					/* AX1 */
		{ AX, 46, 4, { D1, D2, D4, D5 } },			/* AX2 */
		{ AX, 47, 4, { D5, D6, D8, D9 } },			/* AX3 */
		{ AX, 48, 2, { D7, D8 } }					/* AX4 */
	},
	4,												/* number of z-stabilizer */
	{ 
		{ AZ, 42, 2, { D1, D4 } },					/* AZ1 */
		{ AZ, 43, 4, { D4, D5, D7, D8 } },			/* AZ2 */
		{ AZ, 44, 4, { D2, D3, D5, D6 } },			/* AZ3 */
		{ AZ, 45, 2, { D6, D9 } }					/* AZ4 */
	}, 
	0,
	0,
	0
};

struct logical_merge_qubit _MAT = {
	2, 
	{
		{ AX, 50, 2, { 2, 9 } },					/* mxx1 */
		{ AX, 51, 4, { 5, 8, 12, 15 } },			/* mxx2 */
		{ AZ, 34, 4, { 2, 9, 5, 12 } }
	},
	{ 0, 0, 0 }
};

struct logical_merge_qubit _MAC = {
	2, 
	{
		{ AZ, 52, 4, { 6, 7, 18, 19 } },			/* mzz1 */
		{ AZ, 53, 2, { 8, 20 } },					/* mzz2 */
		{ AX, 30, 4, { 7, 8, 19, 20 } },
	},
	{ 0, 0, 0 }
};

class SC17_3LQ_CNOT {
private:
	QRegister *QReg;
	char bitString[512] = "";

private:
	struct logical_qubit *AQ;
	struct logical_qubit *CQ;
	struct logical_qubit *TQ;
	struct logical_merge_qubit *MAT;
	struct logical_merge_qubit *MAC;

public:
	SC17_3LQ_CNOT(void) {
		QReg = new QRegister(54);

		AQ = (struct logical_qubit *)(&_AQ);
		CQ = (struct logical_qubit *)(&_CQ);
		TQ = (struct logical_qubit *)(&_TQ);
		MAT = (struct logical_merge_qubit *)(&_MAT);
		MAC = (struct logical_merge_qubit *)(&_MAC);
	} 

	~SC17_3LQ_CNOT(void) {
		delete QReg;
	}

public:
	/*********************************************/
	/*             stabilizer handling           */
	/*********************************************/
	void buildAX(struct logical_qubit *LQ) {
		for(int i=0; i<LQ->ax_num; i++) {
			struct stabilizer_qubit *SQ = (struct stabilizer_qubit *)&LQ->ax_list[i];
			initZ(QReg, SQ->sq);
			H(QReg, SQ->sq);
			for(int i=0; i<SQ->dq_num; i++) {
				int dqidx = SQ->dq_list[i];
				CX(QReg, SQ->sq, LQ->dq_list[dqidx]);
			}
			H(QReg, SQ->sq);
		}
    }

	void buildAZ(struct logical_qubit *LQ) {
		for(int i=0; i<LQ->az_num; i++) {
			struct stabilizer_qubit *SQ = (struct stabilizer_qubit *)&LQ->az_list[i];
			initZ(QReg, SQ->sq);
			for(int i=0; i<SQ->dq_num; i++) {
				int dqidx = SQ->dq_list[i];
				CX(QReg, LQ->dq_list[dqidx], SQ->sq);
			}
		}
	}

	void buildXStabilizerState(struct logical_qubit *LQ) {
		int ax = 0;

		int m1 = M(QReg, LQ->ax_list[0].sq);
		int m2 = M(QReg, LQ->ax_list[1].sq);
		int m3 = M(QReg, LQ->ax_list[2].sq);
		int m4 = M(QReg, LQ->ax_list[3].sq);

		if(m4 != 0) ax |= (1 << 0);
		if(m3 != 0) ax |= (1 << 1);
		if(m2 != 0) ax |= (1 << 2);
		if(m1 != 0) ax |= (1 << 3);

		LQ->ax = ax;
	}

	void buildZStabilizerState(struct logical_qubit *LQ) {
		int az = 0;

		int m1 = M(QReg, LQ->az_list[0].sq);
		int m2 = M(QReg, LQ->az_list[1].sq);
		int m3 = M(QReg, LQ->az_list[2].sq);
		int m4 = M(QReg, LQ->az_list[3].sq);

		if(m4 != 0) az |= (1 << 0);
		if(m3 != 0) az |= (1 << 1);
		if(m2 != 0) az |= (1 << 2);
		if(m1 != 0) az |= (1 << 3);

		LQ->az = az;
	}

	void showStabilizer(struct logical_qubit *LQ) {
		int ax = LQ->ax;
		int az = LQ->az;
		int flag;

		printf("[%s] : AX=|", LQ->name);

		flag = 8;
		do {
			if(ax&flag) printf("1");
			else printf("0");
        	flag/=2;        
		} while(flag >= 1);
		printf(">  ");

		printf("AZ=|");
		flag = 8;
		do {
			if(az&flag) printf("1");
			else printf("0");
        	flag/=2;        
		} while(flag >= 1);
		printf(">\n");
	}

public:
	/*********************************************/
	/*          ESM & Error Correction           */
	/*********************************************/
	void buildCircuit(struct logical_qubit *LQ) {
		/* run stabilizer circuit */
		buildAX(LQ);
		buildAZ(LQ);

		buildXStabilizerState(LQ);
		buildZStabilizerState(LQ);
		showStabilizer(LQ);
	} 
	
	void errorCorrection(struct logical_qubit *LQ) {
		int round = 3;

		for(int i=0; i<round * 2; i++) {
			int flavor = LQ->flavor;
			int ax = LQ->ax;
			int az = LQ->az;

			if(LQ->ax == 0 && LQ->az == 0) 
				break;

			/* X error correction */
			if(LQ->ax != 0) {
				Z(QReg, LQ->dq_list[x_decoder[flavor][ax]]);
			}
	
			/* Z error correction */
			if(LQ->az != 0) {
				X(QReg, LQ->dq_list[z_decoder[flavor][az]]);
			}

			buildCircuit(LQ);
		}
	}

	void ESM(struct logical_qubit *LQ) {
		buildCircuit(LQ);
		errorCorrection(LQ);
	}

public:
	/*********************************************/
	/*             Logical Operation             */
	/*********************************************/
	void logicalX(struct logical_qubit *LQ) {
		for(int i=0; i<3; i++) {
			X(QReg, LQ->logical_x[i]);
		}
	}

	void logicalZ(struct logical_qubit *LQ) {
		for(int i=0; i<3; i++) {
			Z(QReg, LQ->logical_x[i]);
		}
	}

	void logicalH(struct logical_qubit *LQ) {
		for(int i=0; i<LQ->dq_num; i++) {
			H(QReg, LQ->dq_list[i]);
		}
	}

	int logicalMZ(struct logical_qubit *LQ) {
		int mv = 1;
		for(int i=0; i<LQ->dq_num; i++) {
			int m = M(QReg, LQ->dq_list[i]);
			if(m == 1) {
				mv *= -1;
			}
		}

		if(mv == 1) {
			LQ->measure_value = 1;
		} else {
			LQ->measure_value = -1;
		}

		return LQ->measure_value;
	}

	int logicalMX(struct logical_qubit *LQ) {
		logicalH(LQ);

		int mv = 1;
		for(int i=0; i<LQ->dq_num; i++) {
			int m = M(QReg, LQ->dq_list[i]);
			if(m == 1) {
				mv *= -1;
			}
		}

		if(mv == 1) {
			LQ->measure_value = 1;
		} else {
			LQ->measure_value = -1;
		}

		return LQ->measure_value;
	}

public:
	/*********************************************/
	/*         Initialize Logical Qubit          */
	/*********************************************/
	void clearLQ(struct logical_qubit *LQ) {
		for(int i=0; i<LQ->dq_num; i++) {
			initZ(QReg, LQ->dq_list[i]);
		}
		for(int i=0; i<LQ->ax_num; i++) {
			initZ(QReg, LQ->ax_list[i].sq);
		}
		for(int i=0; i<LQ->az_num; i++) {
			initZ(QReg, LQ->az_list[i].sq);
		}
	}

	void initLQZero(struct logical_qubit *LQ) {
		buildCircuit(LQ);
		errorCorrection(LQ);
	}

	void initLQOne(struct logical_qubit *LQ) {
		buildCircuit(LQ);
		errorCorrection(LQ);
		logicalX(LQ);
	}

	void initLQPlus(struct logical_qubit *LQ) {
		logicalH(LQ);
		buildCircuit(LQ);
		errorCorrection(LQ);
	}

	void initLQMinus(struct logical_qubit *LQ) {
		logicalH(LQ);
		buildCircuit(LQ);
		errorCorrection(LQ);
		logicalZ(LQ);
	}

public:
	/*********************************************/
	/*                Merge & Split              */
	/*********************************************/
	void MergeSplit(struct logical_merge_qubit *MQ, struct logical_qubit *AQ, struct logical_qubit *DQ) {
		for(int i=0; i<MQ->num_stabilizer_qubit; i++) {
			struct stabilizer_qubit *SQ = (struct stabilizer_qubit *)&MQ->merged_qubit[i];

			initZ(QReg, SQ->sq);
			if(SQ->type == AX) {
				H(QReg, SQ->sq);
				printf("H  : %d\n", SQ->sq);
			}

			for(int j=0; j<SQ->dq_num; j++) {
				if(SQ->type == AX) {
					CX(QReg, SQ->sq, SQ->dq_list[j]);
					printf("CX  : %d -> %d\n", SQ->sq, SQ->dq_list[j]);
				} else {
					CX(QReg, SQ->dq_list[j], SQ->sq);
					printf("CX  : %d -> %d\n", SQ->dq_list[j], SQ->sq);
				}
			}

			if(SQ->type == AX) {
				H(QReg, SQ->sq);
				printf("H  : %d\n", SQ->sq);
			}

			printf("\n");
		}

		for(int i=0; i<MQ->num_stabilizer_qubit; i++) {
			struct stabilizer_qubit *SQ = (struct stabilizer_qubit *)&MQ->merged_qubit[i];
			int mv = M(QReg, SQ->sq);
			if(mv == 0) {
				MQ->measure_value[i] = +1;
			} else {
				MQ->measure_value[i] = -1;
			}
			printf("M  : %d\n", SQ->sq);
		}

		ESM(AQ);
		ESM(DQ);
	}

	void postProcess(void) {
		int mxx1 = MAT->measure_value[0];
		int mxx2 = MAT->measure_value[1];
		int mzz1 = MAC->measure_value[0];
		int mzz2 = MAC->measure_value[1];
		int mAQ;
		int a;
		int b;
		int c;

		/* measure logical ancila qubit */
		mAQ = logicalMX(AQ);

		a = ((mxx1 * mxx2) == 1 ? 0 : 1);
		b = ((mzz1 * mzz2) == 1 ? 0 : 1);
		c = (mAQ == 1 ? 0 : 1);

		printf("mxx1=%d, mxx2=%d, mzz1=%d, mzz2=%d, mAQ=%d\n", mxx1, mxx2, mzz1, mzz2, mAQ);
		printf("a=%d, b=%d, c=%d\n", a, b, c);

	#if 0
		if(a == 1) {
			printf("Logical Z(a) : CQ\n");
			logicalZ(CQ);
		}

		if(c == 1) {
			printf("Logical Z(c) : CQ\n");
			logicalZ(CQ);
		}
	#else
		/*
		 * Only this combination transfers the phase of the target (|->) to 
		 * control (|+> or |->). I don't understand the principle of why. 
		 * So far, it has only been confirmed experimentally.
		 */
		if((a + c) != 1) {
			printf("Logical Z(a+c) : CQ\n");
			logicalZ(CQ);
		}
	#endif

	#if 0
		if(c == 1) {
			printf("Logical Z(c) : AQ\n");
			logicalZ(AQ);
		}
	#endif

		if(b == 1) {
			printf("Logical X : TQ\n");
			logicalX(TQ);
		}

		ESM(CQ);
		ESM(TQ);
	}

public:
	void run(void) {
		/* initialize logical qubit */
		initLQPlus(CQ);
		initLQMinus(TQ);
		initLQZero(AQ);

		/* merge & split */
		printf("\n========== MERGE AQ & TQ ==========\n");
		MergeSplit(MAT, AQ, TQ);

		printf("\n========== MERGE AQ & CQ ==========\n");
		MergeSplit(MAC, AQ, CQ);

		printf("\n========== POST PROCESS ==========\n");
		/* post process */
		postProcess();

		/* measure logical qubit */
		printf("---> number of States of C & T = %d\n", (int)QReg->numStates());
		logicalMX(TQ);

	#if 0
		QReg->dump();
		int t = QType(QReg, CQ->dq_list[0]);
		printf("T=%d\n", t);
		if(t == KET_PLUS || t == KET_SPLUS) {
			printf("logical Z...\n");
			logicalZ(CQ);
		}
	#endif

		QReg->dump();
		logicalMX(CQ);

		printf("CQ:%d, TQ:%d\n", CQ->measure_value, TQ->measure_value);
		QReg->dump();
	}
};

int main(int argc, char **argv)
{
    SC17_3LQ_CNOT *CNOT = new SC17_3LQ_CNOT();
    CNOT->run();
}

