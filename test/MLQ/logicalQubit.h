#ifndef __LOGICAL_QUBIT_H__
#define __LOGICAL_QUBIT_H__

#include <stdio.h>

#include "qplayer.h"

class logicalQubit {
public:
	QRegister *QReg;

	struct lqubits_info *LQs;					/* all logical qubits info */

	int flavor;									/* flavor of this LQ */
	int lq_id;									/* identifier of this LQ */
	int lq_num;									/* number of total LQ */
	int row;									/* row number in lattice */
	int col;									/* col number in lattice */

	int dq_list[9];								/* data PQ list in this LQ */
	int logical_x[3];							/* logical X operator */
	int logical_z[3];							/* logical Z operator */

	struct stabilizer_circuit xs_circuit[4];	/* X-stabizer circuit */
	struct stabilizer_circuit zs_circuit[4];	/* Z-stabizer circuit */

	qsize_t bitMask;
	qsize_t bitMaskOne;
	qsize_t bitMaskZero;

public:
	logicalQubit(int id, struct lqubits_info *lqinfo) {
		/*************************************/
		/* STEP1: set global parameters      */
		/*************************************/
		LQs = (struct lqubits_info *)malloc(sizeof(struct lqubits_info));
		memcpy(LQs, lqinfo, sizeof(struct lqubits_info));

		lq_id = id;
		flavor = LQs->lqubits[lq_id].flavor;
		row = LQs->lqubits[lq_id].row;
		col = LQs->lqubits[lq_id].col;

		/*************************************/
		/* STEP2: allocate quantum register  */
		/*************************************/
		QReg = new QRegister(LQs->pq_num);

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
		} else if(flavor == X_RIGHT) {	
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

		/*************************************/
		/* STEP6: generate magic bit mask    */
		/*************************************/
		bitMask = 0;
		bitMaskOne = 0;
		bitMaskZero = 0;

		/* 000000000...111111111...000000000 */
		for(int i=0; i<9; i++) {
			bitMask |= quantum_shiftL(1, getPQ(dq_list[i]));
		}

		/* 111111111...000010000...111111111 */
		bitMaskOne = quantum_shiftL(1, getPQ(dq_list[4]));

		/* 111111111...000000000...111111111 */
		bitMaskZero = ~bitMask;
	}

	~logicalQubit(void) { 
		free(LQs);
		delete QReg; 
	}

	int getId(void) { return lq_id; }
	int getTotalPQSize(void) { return LQs->pq_num; }
	int getPQSize(void) { return LQs->lqubits[lq_id].pq_num; }
	int getPQ(int index) { return LQs->lqubits[lq_id].qubits[index]; }
	int getMagicPQ(void) { return getPQ(dq_list[4]); }
	qsize_t getBitMask(void) { return bitMask; }
	qsize_t getBitMaskOne(void) { return bitMaskOne; }
	qsize_t getBitMaskZero(void) { return bitMaskZero; }

public:
	/******************************************/
	/* Logical qubit initialization functions */
	/******************************************/
	void clear(QRegister *QR) {
		for(int i=0; i<getPQSize(); i++) {
			initZ(QR, getPQ(i));
		}
	}

	void init(QRegister *QR, int mode) {
		for(int i=0; i<getPQSize(); i++) {
			initZ(QR, getPQ(i));
		}

		if(mode == KET_LZERO) {
			runStabilizer(QR);
		} else if(mode == KET_LONE) {
			runStabilizer(QR);
			LX(QR);
		} else if(mode == KET_LPLUS) {
			for(int i=0; i<9; i++) {
				H(QR, getPQ(dq_list[i]));
			}
			runStabilizer(QR);
		} else if(mode == KET_LMINUS) {
			for(int i=0; i<9; i++) {
				H(QR, getPQ(dq_list[i]));
			}
			runStabilizer(QR);
			LZ(QR);
		} 
	}

	void init(int mode) { 
		init(QReg, mode);
	}

public:
	void inject(double rx, double ry, double rz) {
		/* initialize all PQs */
		for(int i=0; i<getPQSize(); i++) {
			initZ(QReg, getPQ(i));
		}

		/* prepare seed state */
		RX(QReg, getPQ(Q12), rx);
		RY(QReg, getPQ(Q12), ry);
		RZ(QReg, getPQ(Q12), rz);

		/* prapagate seed state to neighbor X operator qubits */
		CX(QReg, getPQ(Q12), getPQ(Q5));
		CX(QReg, getPQ(Q12), getPQ(Q19));

		/* ESM */
		runStabilizer();
	}

public:
	/******************************************/
	/* Magic qubit operation functions        */
	/******************************************/
	void magic_initZ(int id) {
		int magicPQ = LQs->lqubits[id].qubits[Q12];
		initZ(QReg, magicPQ);
	}

	void magic_X(int id) {
		int magicPQ = LQs->lqubits[id].qubits[Q12];
		X(QReg, magicPQ);
	}

	void magic_Z(int id) {
		int magicPQ = LQs->lqubits[id].qubits[Q12];
		Z(QReg, magicPQ);
	}

	void magic_H(int id) {
		int magicPQ = LQs->lqubits[id].qubits[Q12];
		H(QReg, magicPQ);
	}

	void magic_S(int id) {
		int magicPQ = LQs->lqubits[id].qubits[Q12];
		S(QReg, magicPQ);
	}

	void magic_T(int id) {
		int magicPQ = LQs->lqubits[id].qubits[Q12];
		T(QReg, magicPQ);
	}

	void magic_CNOT(int control, int target) {
		int controlMagicPQ = LQs->lqubits[control].qubits[Q12];
		int targetMagicPQ = LQs->lqubits[target].qubits[Q12];
		CX(QReg, controlMagicPQ, targetMagicPQ);
	}

	void magic_inject(int id, double rx, double ry, double rz) {
		int magicPQ = LQs->lqubits[id].qubits[Q12];
		RX(QReg, magicPQ, rx);
		RY(QReg, magicPQ, ry);
		RZ(QReg, magicPQ, rz);
	}

	void magic_collapse(int id, int state) {
		int magicPQ = LQs->lqubits[id].qubits[Q12];
		MF(QReg, magicPQ, state);
	}

public:
	/******************************************/
	/* Logical qubit operation functions      */
	/******************************************/
	void LX(QRegister *QR) {
		/* logical-x operation */
		for(int i=0; i<3; i++) {
			X(QR, getPQ(logical_x[i]));
		}
	}

	void LZ(QRegister *QR) {
		/* logical-z operation */
		for(int i=0; i<3; i++) {
			Z(QR, getPQ(logical_z[i]));
		}
	}

	void LH(QRegister *QR) {
		/* logical hadamard operation */
		for(int i=0; i<9; i++) {
			H(QR, getPQ(dq_list[i]));
		}

		/* switch stabilizer circuit */
		struct stabilizer_circuit tmp_circuit[4];
		memcpy(tmp_circuit, xs_circuit, sizeof(tmp_circuit));
		memcpy(xs_circuit, zs_circuit, sizeof(tmp_circuit));
		memcpy(zs_circuit, tmp_circuit, sizeof(tmp_circuit));

		flavor = (flavor == X_LEFT ? X_RIGHT : X_LEFT);

		runStabilizer(QR);
	}

	int LMZV(QRegister *QR) {
		int mv = 1;
		for(int i=0; i<9; i++) {
			if(M(QR, getPQ(dq_list[i])) == 1) {
				mv *= -1;
			}
		}

		return mv;
	}

	int LMXV(QRegister *QR) {
		for(int i=0; i<9; i++) {
			H(QR, getPQ(dq_list[i]));
		}
		return LMZV(QR);
	}

	int LMZ(QRegister *QR) {
		return LMZV(QR) == 1 ? 0 : 1;
	}

	int LMX(QRegister *QR) {
		return LMXV(QR) == 1 ? 0 : 1;
	}

	void LX(void) { LX(QReg); }
	void LZ(void) { LZ(QReg); }
	void LH(void) { LH(QReg); }
	int LMZ(void) { return LMZ(QReg); }
	int LMX(void) { return LMX(QReg); }
	int LMZV(void) { return LMZV(QReg); }
	int LMXV(void) { return LMXV(QReg); }

public:
	/******************************************/
	/* Surface code ESM functions             */
	/******************************************/
	int genStabilizerStates(struct stabilizer_circuit *circuit) {
		qsize_t states = 0;

		for(int i=0; i<4; i++) {
			if(circuit[i].sq_mval == 1) {
				states |= quantum_shiftL(1, 3-i);
			}
		}

		return (int)states;
	}

	void ESM(QRegister *QR) {
		/* build x-stabilizer circuit */
		for(int i=0; i<4; i++) {
			initZ(QR, getPQ(xs_circuit[i].sq_index));
			H(QR, getPQ(xs_circuit[i].sq_index));
			for(int j=0; j<xs_circuit[i].dq_size; j++) {
				CX(QR, getPQ(xs_circuit[i].sq_index), getPQ(xs_circuit[i].dq_index[j]));
			}
			H(QR, getPQ(xs_circuit[i].sq_index));
			xs_circuit[i].sq_mval = M(QR, getPQ(xs_circuit[i].sq_index));
		}

		/* build z-stabilizer circuit */
		for(int i=0; i<4; i++) {
			initZ(QR, getPQ(zs_circuit[i].sq_index));
			for(int j=0; j<zs_circuit[i].dq_size; j++) {
				CX(QR, getPQ(zs_circuit[i].dq_index[j]), getPQ(zs_circuit[i].sq_index));
			}
			zs_circuit[i].sq_mval = M(QR, getPQ(zs_circuit[i].sq_index));
		}
	}

	void ESM_forced(QRegister *QR) {
		/* build x-stabilizer circuit */
		for(int i=0; i<4; i++) {
			initZ(QR, getPQ(xs_circuit[i].sq_index));
			H(QR, getPQ(xs_circuit[i].sq_index));
			for(int j=0; j<xs_circuit[i].dq_size; j++) {
				CX(QR, getPQ(xs_circuit[i].sq_index), getPQ(xs_circuit[i].dq_index[j]));
			}
			H(QR, getPQ(xs_circuit[i].sq_index));
			xs_circuit[i].sq_mval = MF(QR, getPQ(xs_circuit[i].sq_index), 0);
		}

		/* build z-stabilizer circuit */
		for(int i=0; i<4; i++) {
			initZ(QR, getPQ(zs_circuit[i].sq_index));
			for(int j=0; j<zs_circuit[i].dq_size; j++) {
				CX(QR, getPQ(zs_circuit[i].dq_index[j]), getPQ(zs_circuit[i].sq_index));
			}
			zs_circuit[i].sq_mval = MF(QR, getPQ(zs_circuit[i].sq_index), 0);
		}
	}

	void ErrorCorrection(QRegister *QR) {
		int round = 3;

		for(int i=0; i<round; i++) {
			int xs_states = genStabilizerStates(xs_circuit);
			int zs_states = genStabilizerStates(zs_circuit);

			if(xs_states == 0 && zs_states == 0) {
				break;
			}

			/* Z error correction */
			if(xs_states != 0) {
				Z(QR, getPQ(x_decoder[flavor][xs_states]));
			}

			/* X error correction */
			if(zs_states != 0) {
				X(QR, getPQ(z_decoder[flavor][zs_states]));
			}

			ESM(QR);
		}
	}

	void runStabilizer(QRegister *QR) {
		ESM(QR);
		ErrorCorrection(QR);
	}

	void runStabilizer_forced(QRegister *QR) {
		ESM_forced(QR);
		ErrorCorrection(QR);
	}

	void runStabilizer() {
		runStabilizer(QReg);
	}

public:
	/******************************************/
	/* misc functions                         */
	/******************************************/
	void showQState(void) { 
		int maxPQ = 0;

		for(int i=0; i<qd.size; i++) {
			if(maxPQ < qd.qubits[i]) {
				maxPQ = qd.qubits[i];
			}
		}

		showQState(0, maxPQ); 
	}

	void showQState(int begin, int end) { 
		char msg[32] = "";
		sprintf(msg, "LQ%d", lq_id);
		dump(QReg, msg, begin, end, qd); 
		// printf("%s: %lu\n", msg, (uint64_t)QReg->getNumStates());
	}
};

#endif
