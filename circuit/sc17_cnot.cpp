#include <stdio.h>

#include "qplayer.h"

using namespace std;

class SC17_3LQ_CNOT {
private:
	QRegister *QReg;

	int mx1, mx2, mx3, mx4;
	int mz1, mz2, mz3, mz4;

	int mxx1, mxx2;
	int mzz1, mzz2;

	int mAQ, mTQ, mCQ;

	int a, b, c;

public:
	SC17_3LQ_CNOT(void) {
		QReg = new QRegister(54);
	} 

	~SC17_3LQ_CNOT(void) {
		delete QReg;
	}

public:
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

public:
	void ESM_AQ(void) {
		/* AX */
		buildAX(27, 0, 1);
		buildAX(28, 1, 2, 4, 5);
		buildAX(29, 3, 4, 6, 7);
		buildAX(30, 7, 8);

		mx1 = M(QReg, 27);
		mx2 = M(QReg, 28);
		mx3 = M(QReg, 29);
		mx4 = M(QReg, 30);

		/* AZ */
		buildAZ(31, 3, 6);
		buildAZ(32, 0, 1, 3, 4);
		buildAZ(33, 4, 5, 7, 8);
		buildAZ(34, 2, 5);

		mz1 = M(QReg, 31);
		mz2 = M(QReg, 32);
		mz3 = M(QReg, 33);
		mz4 = M(QReg, 34);

		// printf("[AQ] AZ=%d%d%d%d, AX=%d%d%d%d\n", mz4, mz3, mz2, mz1, mx4, mx3, mx2, mx1);
	}

	void ESM_TQ(void) {
		/* AX */
		buildAX(38, 10, 11);
		buildAX(39, 9, 10, 12, 13);
		buildAX(40, 13, 14, 16, 17);
		buildAX(41, 15, 16);

		mx1 = M(QReg, 38);
		mx2 = M(QReg, 39);
		mx3 = M(QReg, 40);
		mx4 = M(QReg, 41);

		/* AZ */
		buildAZ(34, 9, 12);
		buildAZ(35, 12, 13, 15, 16);
		buildAZ(36, 10, 11, 13, 14);
		buildAZ(37, 14, 17);
	
		mz1 = M(QReg, 34);
		mz2 = M(QReg, 35);
		mz3 = M(QReg, 36);
		mz4 = M(QReg, 37);

		// printf("[TQ] AZ=%d%d%d%d, AX=%d%d%d%d\n", mz4, mz3, mz2, mz1, mx4, mx3, mx2, mx1);
	}

	void ESM_CQ(void) {
		/* AX */
		buildAX(30, 19, 20);
		buildAX(46, 18, 19, 21, 22);
		buildAX(47, 22, 23, 25, 26);
		buildAX(48, 24, 25);

		mx1 = M(QReg, 30);
		mx2 = M(QReg, 46);
		mx3 = M(QReg, 47);
		mx4 = M(QReg, 48);

		/* AZ */
		buildAZ(42, 18, 21);
		buildAZ(43, 21, 22, 24, 25);
		buildAZ(44, 19, 20, 22, 23);
		buildAZ(45, 23, 26);
	
		mz1 = M(QReg, 42);
		mz2 = M(QReg, 43);
		mz3 = M(QReg, 44);
		mz4 = M(QReg, 45);

		// printf("[CQ] AZ=%d%d%d%d, AX=%d%d%d%d\n", mz4, mz3, mz2, mz1, mx4, mx3, mx2, mx1);
	}

	void EC_AQ(int x1, int x2, int x3, int x4) {
		int z1, z2, z3, z4;
		
		z1 = z2 = z3 = z4 = 0;

		for(int i=0; i<3; i++) {
			ESM_AQ();

			if(mx1 != x1) {
				if(mx2 != x2) {
					Z(QReg, 1);
				} else {
					Z(QReg, 0);
				}
			} else if(mx2 != x2) {
				if(mx1 != x1) {
					Z(QReg, 1);
				} else if(mx3 != x3) {
					Z(QReg, 4);
				} else {
					Z(QReg, 2);
				}
			} else if(mx3 != x3) {
				if(mx2 != x2) {
					Z(QReg, 4);
				} else if(mx4 != x4) {
					Z(QReg, 7);
				} else {
					Z(QReg, 6);
				}
			} else if(mx4 != x4) {
				if(mx3 != x3) {
					Z(QReg, 7);
				} else {
					Z(QReg, 8);
				}
			}

			if(mz1 != z1) {
				if(mz2 != z2) {
					X(QReg, 3);
				} else {
					X(QReg, 6);
				}
			} else if(mz2 != z2) {
				if(mz1 != z1) {
					X(QReg, 3);
				} else if(mz3 != z3) {
					X(QReg, 4);
				} else {
					X(QReg, 0);
				}
			} else if(mz3 != z3) {
				if(mz2 != z2) {
					X(QReg, 4);
				} else if(mz4 != z4) {
					X(QReg, 5);
				} else {
					X(QReg, 8);
				}
			} else if(mz4 != z4) {
				if(mz3 != z3) {
					X(QReg, 5);
				} else {
					X(QReg, 2);
				}
			}
		}
	}

	void EC_TQ(int x1, int x2, int x3, int x4) {
		int z1, z2, z3, z4;
		
		z1 = z2 = z3 = z4 = 0;

		for(int i=0; i<3; i++) {
			ESM_TQ();

			if(mx1 != x1) {
				if(mx2 != x2) {
					Z(QReg, 10);
				} else {
					Z(QReg, 11);
				}
			} else if(mx2 != x2) {
				if(mx1 != x1) {
					Z(QReg, 10);
				} else if(mx3 != x3) {
					Z(QReg, 13);
				} else {
					Z(QReg, 9);
				}
			} else if(mx3 != x3) {
				if(mx2 != x2) {
					Z(QReg, 13);
				} else if(mx4 != x4) {
					Z(QReg, 16);
				} else {
					Z(QReg, 17);
				}
			} else if(mx4 != x4) {
				if(mx3 != x3) {
					Z(QReg, 16);
				} else {
					Z(QReg, 15);
				}
			}

			if(mz1 != z1) {
				if(mz2 != z2) {
					X(QReg, 12);
				} else {
					X(QReg, 9);
				}
			} else if(mz2 != z2) {
				if(mz1 != z1) {
					X(QReg, 12);
				} else if(mz3 != z3) {
					X(QReg, 13);
				} else {
					X(QReg, 15);
				}
			} else if(mz3 != z3) {
				if(mz2 != z2) {
					X(QReg, 13);
				} else if(mz4 != z4) {
					X(QReg, 14);
				} else {
					X(QReg, 11);
				}
			} else if(mz4 != z4) {
				if(mz3 != z3) {
					X(QReg, 14);
				} else {
					X(QReg, 17);
				}
			}
		}
	}

	void EC_CQ(int x1, int x2, int x3, int x4) {
		int z1, z2, z3, z4;
		
		z1 = z2 = z3 = z4 = 0;

		for(int i=0; i<3; i++) {
			ESM_CQ();

			if(mx1 != x1) {
				if(mx2 != x2) {
					Z(QReg, 19);
				} else {
					Z(QReg, 20);
				}
			} else if(mx2 != x2) {
				if(mx1 != x1) {
					Z(QReg, 19);
				} else if(mx3 != x3) {
					Z(QReg, 22);
				} else {
					Z(QReg, 18);
				}
			} else if(mx3 != x3) {
				if(mx2 != x2) {
					Z(QReg, 22);
				} else if(mx4 != x4) {
					Z(QReg, 25);
				} else {
					Z(QReg, 26);
				}
			} else if(mx4 != x4) {
				if(mx3 != x3) {
					Z(QReg, 25);
				} else {
					Z(QReg, 24);
				}
			}

			if(mz1 != z1) {
				if(mz2 != z2) {
					X(QReg, 21);
				} else {
					X(QReg, 18);
				}
			} else if(mz2 != z2) {
				if(mz1 != z1) {
					X(QReg, 21);
				} else if(mz3 != z3) {
					X(QReg, 22);
				} else {
					X(QReg, 24);
				}
			} else if(mz3 != z3) {
				if(mz2 != z2) {
					X(QReg, 22);
				} else if(mz4 != z4) {
					X(QReg, 23);
				} else {
					X(QReg, 20);
				}
			} else if(mz4 != z4) {
				if(mz3 != z3) {
					X(QReg, 23);
				} else {
					X(QReg, 26);
				}
			}
		}
	}

	void LX_AQ(void) {
		X(QReg, 0);
		X(QReg, 3);
		X(QReg, 6);
	}

	void LX_TQ(void) {
		X(QReg, 11);
		X(QReg, 14);
		X(QReg, 17);
	}

	void LX_CQ(void) {
		X(QReg, 20);
		X(QReg, 23);
		X(QReg, 26);
	}

	void LZ_AQ(void) {
		Z(QReg, 0);
		Z(QReg, 1);
		Z(QReg, 2);
	}

	void LZ_TQ(void) {
		Z(QReg, 9);
		Z(QReg, 10);
		Z(QReg, 11);
	}

	void LZ_CQ(void) {
		Z(QReg, 18);
		Z(QReg, 19);
		Z(QReg, 20);
	}

	void LH_TQ(void) {
		H(QReg, 9);
		H(QReg, 10);
		H(QReg, 11);
		H(QReg, 12);
		H(QReg, 13);
		H(QReg, 14);
		H(QReg, 15);
		H(QReg, 16);
		H(QReg, 17);
	}

	void Mz_AQ(void) {
		int d0, d1, d2, d3, d4, d5, d6, d7, d8;

		d0 = M(QReg, 0);
		d1 = M(QReg, 1);
		d2 = M(QReg, 2);
		d3 = M(QReg, 3);
		d4 = M(QReg, 4);
		d5 = M(QReg, 5);
		d6 = M(QReg, 6);
		d7 = M(QReg, 7);
		d8 = M(QReg, 8);

		mAQ = 1;
		if(d0 == 1) { mAQ *= -1; }
		if(d1 == 1) { mAQ *= -1; }
		if(d2 == 1) { mAQ *= -1; }
		if(d3 == 1) { mAQ *= -1; }
		if(d4 == 1) { mAQ *= -1; }
		if(d5 == 1) { mAQ *= -1; }
		if(d6 == 1) { mAQ *= -1; }
		if(d7 == 1) { mAQ *= -1; }
		if(d8 == 1) { mAQ *= -1; }

		if(mAQ == 1) {
			printf("\tMeasure AQ : |%d%d%d%d%d%d%d%d%d> --> |0>\n",
					d0, d1, d2, d3, d4, d5, d6, d7, d8);
		} else {
			printf("\tMeasure AQ : |%d%d%d%d%d%d%d%d%d> --> |1>\n",
					d0, d1, d2, d3, d4, d5, d6, d7, d8);
		}
	}

	void Mx_AQ(void) {
		for(int i=0; i<9; i++) {
			H(QReg, i);
		}
		Mz_AQ();
	}

	void Mz_TQ(void) {
		int d0, d1, d2, d3, d4, d5, d6, d7, d8;

		d0 = M(QReg, 9);
		d1 = M(QReg, 10);
		d2 = M(QReg, 11);
		d3 = M(QReg, 12);
		d4 = M(QReg, 13);
		d5 = M(QReg, 14);
		d6 = M(QReg, 15);
		d7 = M(QReg, 16);
		d8 = M(QReg, 17);

		mTQ = 1;
		if(d0 == 1) { mTQ *= -1; }
		if(d1 == 1) { mTQ *= -1; }
		if(d2 == 1) { mTQ *= -1; }
		if(d3 == 1) { mTQ *= -1; }
		if(d4 == 1) { mTQ *= -1; }
		if(d5 == 1) { mTQ *= -1; }
		if(d6 == 1) { mTQ *= -1; }
		if(d7 == 1) { mTQ *= -1; }
		if(d8 == 1) { mTQ *= -1; }

		if(mTQ == 1) {
			printf("\tMeasure TQ : |%d%d%d%d%d%d%d%d%d> --> |0>\n",
					d0, d1, d2, d3, d4, d5, d6, d7, d8);
		} else {
			printf("\tMeasure TQ : |%d%d%d%d%d%d%d%d%d> --> |1>\n",
					d0, d1, d2, d3, d4, d5, d6, d7, d8);
		}
	}

	void Mz_CQ(void) {
		int d0, d1, d2, d3, d4, d5, d6, d7, d8;

		d0 = M(QReg, 18);
		d1 = M(QReg, 19);
		d2 = M(QReg, 20);
		d3 = M(QReg, 21);
		d4 = M(QReg, 22);
		d5 = M(QReg, 23);
		d6 = M(QReg, 24);
		d7 = M(QReg, 25);
		d8 = M(QReg, 26);

		mCQ = 1;
		if(d0 == 1) { mCQ *= -1; }
		if(d1 == 1) { mCQ *= -1; }
		if(d2 == 1) { mCQ *= -1; }
		if(d3 == 1) { mCQ *= -1; }
		if(d4 == 1) { mCQ *= -1; }
		if(d5 == 1) { mCQ *= -1; }
		if(d6 == 1) { mCQ *= -1; }
		if(d7 == 1) { mCQ *= -1; }
		if(d8 == 1) { mCQ *= -1; }

		if(mCQ == 1) {
			printf("\tMeasure CQ : |%d%d%d%d%d%d%d%d%d> --> |0>\n",
					d0, d1, d2, d3, d4, d5, d6, d7, d8);
		} else {
			printf("\tMeasure CQ : |%d%d%d%d%d%d%d%d%d> --> |1>\n",
					d0, d1, d2, d3, d4, d5, d6, d7, d8);
		}
	}

	void SurgeryXX(void) {
		buildAX(50, 2, 9);
		buildAX(51, 5, 12, 8, 15);
		buildAZ(34, 2, 9, 5, 12);

		mxx1 = M(QReg, 50);
		mxx2 = M(QReg, 51);
		M(QReg, 34);

		if(mxx1 == 0) {
			printf("\tmxx1=+1, ");
			mxx1 = 1;
		} else {
			printf("\tmxx1=-1, ");
			mxx1 = -1;
		}

		if(mxx2 == 0) {
			printf("mxx2=+1\n");
			mxx2 = 1;
		} else {
			printf("mxx2=-1\n");
			mxx2 = -1;
		}
	}

	void SurgeryZZ(void) {
		buildAZ(52, 6, 7, 18, 19);
		buildAZ(53, 8, 20);
		buildAX(30, 7, 8, 19, 20);

		mzz1 = M(QReg, 52);
		mzz2 = M(QReg, 53);
		M(QReg, 30);

		if(mzz1 == 0) {
			printf("\tmzz1=+1, ");
			mzz1 = 1;
		} else {
			printf("\tmzz1=-1, ");
			mzz1 = -1;
		}

		if(mzz2 == 0) {
			printf("mzz2=+1\n");
			mzz2 = 1;
		} else {
			printf("mzz2=-1\n");
			mzz2 = -1;
		}
	}

public:
	void run(void) {
		int L1_X1 = 0;
		int L1_X2 = 0;
		int L1_X3 = 0;
		int L1_X4 = 0;

		int L2_X1 = 0;
		int L2_X2 = 0;
		int L2_X3 = 0;
		int L2_X4 = 0;

		int L3_X1 = 0;
		int L3_X2 = 0;
		int L3_X3 = 0;
		int L3_X4 = 0;

		printf("\n(STEP1) initialize 3 Logical Qubuts\n");
		EC_AQ(L1_X1, L1_X2, L1_X3, L1_X4);
		EC_TQ(L2_X1, L2_X2, L2_X3, L2_X4);
		EC_CQ(L3_X1, L3_X2, L3_X3, L3_X4);

		printf("\n(STEP2) set LX to control qubit\n");
		LX_CQ();

		printf("\n(STEP3) merge&split XX + ESM\n");
		SurgeryXX();
		EC_AQ(L1_X1, L1_X2, L1_X3, L1_X4);
		EC_TQ(L2_X1, L2_X2, L2_X3, L2_X4);

		printf("\n(STEP4) merge&split ZZ + ESM\n");
		SurgeryZZ();
		EC_AQ(L2_X1, L2_X2, L2_X3, L2_X4);
		EC_CQ(L3_X1, L3_X2, L3_X3, L3_X4);

		printf("\n(STEP5) measure AQ\n");
	#if 0
		Mz_AQ();
	#else
		Mx_AQ();
	#endif

		if(mxx1 != mxx2) {
			a = 1;
		} else {
			a = 0;
		}

		if(mzz1 != mzz2) {
			b = 1;
		} else {
			b = 0;
		}

		if(mAQ == 1) {
			c = 0;
		} else {
			c = 1;
		}

		printf("\t(STEP6) apply X or Z to the Target\n");

		if(a == 1) {
			printf("\tset LZ to Control\n");
			LZ_CQ();		
		}

		if(c == 1) {
			printf("\tset LZ to Control\n");
			LZ_CQ();		
		}

		if(b == 1) {
			printf("\tset LX to Target\n");
			LX_TQ();		
		}

		Mz_CQ();

		QReg->dump();
		for(int i=0; i<QReg->getNumQubits(); i++) {
            printf("[%2d] %s\n", i, QTypeStr(QReg, i));
        }

		Mz_TQ();
	}
};

int main(int argc, char **argv)
{
    SC17_3LQ_CNOT *CNOT = new SC17_3LQ_CNOT();
    CNOT->run();
}
