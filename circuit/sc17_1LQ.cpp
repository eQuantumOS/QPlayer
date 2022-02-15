#include <stdio.h>

#include "qplayer.h"

using namespace std;

class SC17_1LQ {
private:
	QRegister *QReg;

	int ax1, ax2, ax3, ax4;		/* target X-stabilizer */
	int az1, az2, az3, az4;		/* target Z-stabilizer */

	int mx1, mx2, mx3, mx4;
	int mz1, mz2, mz3, mz4;
	int hmx1, hmx2, hmx3, hmx4;
	int hmz1, hmz2, hmz3, hmz4;

	int d0, d1, d2, d3, d4, d5, d6, d7, d8;

public:
	SC17_1LQ(void) { 
		/* initialize quantum register */
		QReg = new QRegister(17);
	} 

	~SC17_1LQ(void) { 
		delete QReg;
	}

private:
	void buildAX(int AQ, int PQ1, int PQ2) {
        initZ(QReg, AQ);
        H(QReg, AQ);
        CX(QReg, AQ, PQ1);
        CX(QReg, AQ, PQ2);
        H(QReg, AQ);
    }

    void buildAX(int AQ, int PQ1, int PQ2, int PQ3, int PQ4) {
        initZ(QReg, AQ);
        H(QReg, AQ);
        CX(QReg, AQ, PQ1);
        CX(QReg, AQ, PQ2);
        CX(QReg, AQ, PQ3);
        CX(QReg, AQ, PQ4);
        H(QReg, AQ);
    }

    void buildAZ(int AQ, int PQ1, int PQ2) {
        initZ(QReg, AQ);
        CX(QReg, PQ1, AQ);
        CX(QReg, PQ2, AQ);
    }

    void buildAZ(int AQ, int PQ1, int PQ2, int PQ3, int PQ4) {
        initZ(QReg, AQ);
        CX(QReg, PQ1, AQ);
        CX(QReg, PQ2, AQ);
        CX(QReg, PQ3, AQ);
        CX(QReg, PQ4, AQ);
    }

    int buildAXM(int AQ, int PQ1, int PQ2) {
        buildAX(AQ, PQ1, PQ2);
        return M(QReg, AQ);
    }

    int buildAXM(int AQ, int PQ1, int PQ2, int PQ3, int PQ4) {
        buildAX(AQ, PQ1, PQ2, PQ3, PQ4);
        return M(QReg, AQ);
    }

    int buildAZM(int AQ, int PQ1, int PQ2) {
        buildAZ(AQ, PQ1, PQ2);
        return M(QReg, AQ);
    }

    int buildAZM(int AQ, int PQ1, int PQ2, int PQ3, int PQ4) {
        buildAZ(AQ, PQ1, PQ2, PQ3, PQ4);
        return M(QReg, AQ);
    }

private:
	void ESM(void) {
		/* measure X Qubits */
		buildAX(9, 1, 2);
		buildAX(10, 0, 1, 3, 4);
		buildAX(11, 4, 5, 7, 8);
		buildAX(12, 6, 7);

		/* measure Z Qubits */
		buildAZ(13, 0, 3);
		buildAZ(14, 3, 4, 6, 7);
		buildAZ(15, 1, 2, 4, 5);
		buildAZ(16, 5, 8);

		mx1 = M(QReg, 9);
		mx2 = M(QReg, 10);
		mx3 = M(QReg, 11);
		mx4 = M(QReg, 12);

		mz1 = M(QReg, 13);
		mz2 = M(QReg, 14);
		mz3 = M(QReg, 15);
		mz4 = M(QReg, 16);

		printf("AZ=%d%d%d%d, AX=%d%d%d%d\n", mz4, mz3, mz2, mz1, mx4, mx3, mx2, mx1);
	}

	void EC(int x1, int x2, int x3, int x4, int z1, int z2, int z3, int z4) {
		for(int i=0; i<3; i++) {
			ESM();

			/* we consider 1 qubit error only */
			if(mx1 != x1) {
				if(mx2 != x2) {
					Z(QReg, 1);
				} else {
					Z(QReg, 2);
				}
			} else if(mx2 != x2) {
				if(mx1 != x1) {
					Z(QReg, 1);
				} else if(mx3 != x3) {
					Z(QReg, 4);
				} else {
					Z(QReg, 0); 
				}
			} else if(mx3 != x3) {
				if(mx2 != x2) {
					Z(QReg, 4);
				} else if(mx4 != x4) {
					Z(QReg, 7);
				} else {
					Z(QReg, 8); 
				}
			} else if(mx4 != x4) {
				if(mx3 != x3) {
					Z(QReg, 7);
				} else {
					Z(QReg, 6);
				}
			} else if(mz1 != z1) {
				if(mz2 != z2) {
					X(QReg, 3);
				} else {
					X(QReg, 0);
				}
			} else if(mz2 != z2) {
				if(mz1 != z1) {
					X(QReg, 3);
				} else if(mz3 != z3) {
					X(QReg, 4);
				} else {
					X(QReg, 6); 
				}
			} else if(mz3 != z3) {
				if(mz2 != z2) {
					X(QReg, 4);
				} else if(mz4 != z4) {
					X(QReg, 5);
				} else {
					X(QReg, 2);
				}
			} else if(mz4 != z4) {
				if(mz3 != z3) {
					X(QReg, 5);
				} else {
					X(QReg, 8);
				}
			}
		}
	}

	/* 
	 * This function is differnt from ESM(). Keep in mind that 
	 * the placement of the qubits has changed.
	 */
	void ESM_H(void) {
		/* measure X Qubits */
		buildAX(13, 0, 3);
		buildAX(14, 3, 4, 6, 7);
		buildAX(15, 1, 2, 4, 5);
		buildAX(16, 5, 8);

		/* measure Z Qubits */
		buildAZ(9, 1, 2);
		buildAZ(10, 0, 1, 3, 4);
		buildAZ(11, 4, 5, 7, 8);
		buildAZ(12, 6, 7);

		hmx1 = M(QReg, 13);
		hmx2 = M(QReg, 14);
		hmx3 = M(QReg, 15);
		hmx4 = M(QReg, 16);

		hmz1 = M(QReg, 9);
		hmz2 = M(QReg, 10);
		hmz3 = M(QReg, 11);
		hmz4 = M(QReg, 12);

		/* 
		 * It is a very strange phenomenon that when Hadamard is applied, 
		 * the values of X and Y are exchanged with each other.
		 */
		printf("AZ=%d%d%d%d, AX=%d%d%d%d\n", hmz4, hmz3, hmz2, hmz1, hmx4, hmx3, hmx2, hmx1);
	}

	void MeasureData(void) {
		d0 = M(QReg, 0);
		d1 = M(QReg, 1);
		d2 = M(QReg, 2);
		d3 = M(QReg, 3);
		d4 = M(QReg, 4);
		d5 = M(QReg, 5);
		d6 = M(QReg, 6);
		d7 = M(QReg, 7);
		d8 = M(QReg, 8);

		printf("data qubit is collapsed : |%d%d%d%d%d%d%d%d%d>\n", 
			d0, d1, d2, d3, d4, d5, d6, d7, d8);
	}

	void LX(void) {
		X(QReg, 0);
		X(QReg, 3);
		X(QReg, 6);
	}

	void LZ(void) {
		Z(QReg, 0);
		Z(QReg, 1);
		Z(QReg, 2);
	}

	void LH(void) {
		H(QReg, 0);
		H(QReg, 1);
		H(QReg, 2);
		H(QReg, 3);
		H(QReg, 4);
		H(QReg, 5);
		H(QReg, 6);
		H(QReg, 7);
		H(QReg, 8);
	}

	void XERROR(void) {
		int qubit = rand() % 9;
		X(QReg, qubit);

		printf("---> data qubit[%d] meets X-ERROR\n\n", qubit);
	}

	void ZERROR(void) {
		int qubit = rand() % 9;
		Z(QReg, qubit);

		printf("---> data qubit[%d] meets Z-ERROR\n\n", qubit);
	}

private:
	void CASE_NORMAL_ESM(void) {
		/* 1st round */
		printf("---> 1st ESM round.\n");
		ESM();
		QReg->dump();
	
		RX(QReg, 0, M_PI/4);
		RY(QReg, 2, M_PI/8);
		QReg->dump();

		/* 2nd round */
		printf("---> 2st ESM round.\n");
		ESM();
		QReg->dump();

		/* measure data qubit */
		MeasureData();
		QReg->dump();
	}

	void CASE_HADAMARD(void) {
		/* 1st round */
		printf("---> 1st ESM round.\n");
		ESM();
		QReg->dump();

		/* Apply H gate */
		printf("---> apply logical Hadamard.\n\n");
		LH();  

		/* 2nd round */
		printf("---> 2st ESM round.\n");
		ESM_H();
		QReg->dump();

		/* measure data qubit */
		MeasureData();
	}

	void CASE_LOGICAL_OP(void) {
		/* 1st round */
		printf("---> 1st ESM round.\n");
		ESM();
		QReg->dump();

		/* Apply X or Z gate */
		printf("---> apply logical X.\n\n");
		LX();
		// LZ();

		/* 2nd round */
		printf("---> 2st ESM round.\n");
		ESM();
		QReg->dump();

		/* measure data qubit */
		MeasureData();
	}

	void CASE_XERROR_NO_CORRECTION(void) {
		/* 1st round */
		printf("---> 1st ESM round.\n");
		ESM();
		QReg->dump();

		/* X error is set for any qubit out of 9 data qubits */
		XERROR();

		/* 2st round after error for watching stabilizer changes */
		printf("---> 2nd ESM round after X-ERROR.\n");
		ESM();
		QReg->dump();

		/* measure data qubit */
		MeasureData();
	}

	void CASE_ZERROR_NO_CORRECTION(void) {
		/* 1st round */
		printf("---> 1st ESM round.\n");
		ESM();
		QReg->dump();

		/* Z error is set for any qubit out of 9 data qubits */
		ZERROR();

		/* 2st round after error */
		printf("---> 2nd ESM round after Z-ERROR.\n");
		ESM();
		QReg->dump();

		/* measure data qubit */
		MeasureData();
	}

	void CASE_XERROR_CORRECTION(void) {
		/* 1st round */
		printf("---> 1st ESM round.\n");
		ESM();
		QReg->dump();

		/* X error is set for any qubit out of 9 data qubits */
		XERROR();

		EC(mx1, mx2, mx3, mx4, mz1, mz2, mz3, mz4);
		printf("Error correction is completed...\n\n");

		/* 
		 * quantum state after error correction. 
		 * please compare to quantum state of 1st round.
		 */
		printf("---> 2nd ESM round after X-ERROR fixing.\n");
		ESM();
		QReg->dump();

		/* measure data qubit */
		MeasureData();
	}

	void CASE_ZERROR_CORRECTION(void) {
		/* 1st round */
		printf("---> 1st ESM round.\n");
		ESM();
		QReg->dump();

		/* Z error is set for any qubit out of 9 data qubits */
		ZERROR();

		EC(mx1, mx2, mx3, mx4, mz1, mz2, mz3, mz4);
		printf("Error correction is completed...\n\n");

		/* 
		 * quantum state after error correction. 
		 * please compare to quantum state of 1st round.
		 */
		printf("---> 2nd ESM round after Z-ERROR fixing.\n");
		ESM();
		QReg->dump();

		/* measure data qubit */
		MeasureData();
	}

	void CASE_DEPHASING_T1(void) {
	}

	void CASE_DEPHASING_T2(void) {
		int x1, x2, x3, x4;
		int z1, z2, z3, z4;

		x1 = 0;
		x2 = 0;
		x3 = 0;
		x4 = 0;
		z1 = z2 = z3 = z4 = 0;

		printf("STEP1 : initialize LQ\n");
		EC(x1, x2, x3, x4, z1, z2, z3, z4);
		for(int i=0; i<QReg->getNumQubits(); i++) {
			printf("[%2d] %s\n", i, QTypeStr(QReg, i));
		}
		QReg->dump();

		printf("\nSTEP2 : measure DQ0 <-- dephasing effect\n");
		initZ(QReg, 0);
		for(int i=0; i<QReg->getNumQubits(); i++) {
			printf("[%2d] %s\n", i, QTypeStr(QReg, i));
		}
		QReg->dump();

		printf("\nSTEP3 : ESM\n");
		ESM();
		for(int i=0; i<QReg->getNumQubits(); i++) {
			printf("[%2d] %s\n", i, QTypeStr(QReg, i));
		}
		QReg->dump();

		printf("\nSTEP4 : error correction\n");
		EC(x1, x2, x3, x4, z1, z2, z3, z4);
		for(int i=0; i<QReg->getNumQubits(); i++) {
			printf("[%2d] %s\n", i, QTypeStr(QReg, i));
		}
		QReg->dump();
	}

public:
	void run(void) {
		do {
			printf("\n======================================\n");
			printf("           select test case           \n");
			printf("======================================\n");
			printf("  1. normal surface code cycle\n");
			printf("  2. apply logical Hadamard\n");
			printf("  3. apply logical X or Z\n");
			printf("  4. data qubit X error\n");
			printf("  5. data qubit Z error\n");
			printf("  6. data qubit X error correction\n");
			printf("  7. data qubit Z error correction\n");
			printf("  8. dephasing T1\n");
			printf("  9. dephasing T2\n");
			printf("  0. quit\n");

			/* Initialize QRegister */
			QReg->reset();

			char c = getchar();
			switch(c) {
			case '0' :
				return;
			case '1' :
				CASE_NORMAL_ESM();
				break;
			case '2' :
				CASE_HADAMARD();
				break;
			case '3' :
				CASE_LOGICAL_OP();
				break;
			case '4' :
				CASE_XERROR_NO_CORRECTION();
				break;
			case '5' :
				CASE_ZERROR_NO_CORRECTION();
				break;
			case '6' :
				CASE_XERROR_CORRECTION();
				break;
			case '7' :
				CASE_ZERROR_CORRECTION();
				break;
			case '8' :
				CASE_DEPHASING_T1();
				break;
			case '9' :
				CASE_DEPHASING_T2();
				break;
			}
	
			/* this is for ignoring carrige return */
			c = getchar();
		} while(1);
	}
};

int main(int argc, char **argv)
{
	SC17_1LQ *LQ = new SC17_1LQ();
	LQ->run();
}
