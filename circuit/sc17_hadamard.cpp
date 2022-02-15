#include <stdio.h>

#include "qplayer.h"

using namespace std;

#define LOG
#define DUMP

/* 
 * This class defined for Hadamard Type 4 moved from bottom left to top right 
 */
class HADAMARD_TYPE5 {
private:
	QRegister * QReg;

	int cq_type = 0;	/* 0=|0>, 1=|1>, 2=|+>, 3=|-> */
	int tq_type = 0;	/* 0=|0>, 1=|1> */
	int a = 1, b = 1, c = 1;

	int loopCnt = 1;
	int state_00 = 0, state_11 = 0, state_01 = 0, state_10 = 0, others = 0;

private:
	void log_printf(int begin1, int end1, int begin2, int end2, int begin3, int end3) {
			printf("Data Qubits : %d/%d/%d %d/%d/%d %d/%d/%d\n", end1, end1-1, begin1, end2, end2-1, begin2, end3, end3-1, begin3);
			QReg->dump(begin1, end1, begin2, end2, begin3, end3);
	}

	int X_Stabilizer(int X, int a, int b) {
		initZ(QReg, X);
		H(QReg, X);
		CX(QReg, X, a);
		CX(QReg, X, b);
		H(QReg, X);
		return M(QReg, X);
	}

	int X_Stabilizer(int X, int a, int b, int c) {
		initZ(QReg, X);
		H(QReg, X);
		CX(QReg, X, a);
		CX(QReg, X, b);
		CX(QReg, X, c);
		H(QReg, X);
		return M(QReg, X);
	}

	int X_Stabilizer(int X, int a, int b, int c, int d) {
		initZ(QReg, X);
		H(QReg, X);
		CX(QReg, X, b);
		CX(QReg, X, a);
		CX(QReg, X, d);
		CX(QReg, X, c);
		H(QReg, X);
		return M(QReg, X);
	}

	int Z_Stabilizer(int Z, int a, int b) {
		initZ(QReg, Z);
		CX(QReg, a, Z);
		CX(QReg, b, Z);
		return M(QReg, Z);
	}

	int Z_Stabilizer(int Z, int a, int b, int c, int d) {
		initZ(QReg, Z);
		CX(QReg, b, Z);
		CX(QReg, d, Z);
		CX(QReg, a, Z);
		CX(QReg, c, Z);
		return M(QReg, Z);
	}

	void Measure_LQ(bool bH, vector<int> &q) {
		if (bH == true) {
			for (int i = 0; i < 9; i++)
				H(QReg, q[i]);
			log_printf(q[0], q[2], q[3], q[5], q[6], q[8]);
		} else {
			printf("Measure_LQ - %d/%d/%d/%d/%d/%d/%d/%d/%d\n", q[2],q[1],q[0],q[5],q[4],q[3],q[8],q[7],q[6]);
#ifdef DUMP
			QReg->dump(q[0], q[2], q[3], q[5], q[6], q[8]);
#endif
		}

		int mz[9];
		for (int i = 0; i < 9; i++)
			mz[i] = M(QReg, q[i]);

		int ones = 0;
		for (int i = 0; i < 9; i++)
			if (mz[i] == 1)
				ones++;
		printf("Final data measurement : data=|q%dq%dq%d q%dq%dq%d q%dq%dq%d>\n", q[2], q[1], q[0], q[5], q[4], q[3], q[8], q[7], q[6]);
		printf("Final data measurement : data=|%d%d%d %d%d%d %d%d%d> [%c]\n", 
				mz[2], mz[1], mz[0], mz[5], mz[4], mz[3], mz[8], mz[7], mz[6], (ones%2 == 0? 'E' : 'O'));
		log_printf(q[0], q[2], q[3], q[5], q[6], q[8]);
	}

	void Measure_LQ(bool bH, vector<int> &q, int& result) {
		if (bH == true) {
			for (int i = 0; i < 9; i++)
				H(QReg, q[i]);
			log_printf(q[0], q[2], q[3], q[5], q[6], q[8]);
		} else {
			printf("Measure_LQ - %d/%d/%d/%d/%d/%d/%d/%d/%d\n", q[2],q[1],q[0],q[5],q[4],q[3],q[8],q[7],q[6]);
#ifdef DUMP
			QReg->dump(q[0], q[2], q[3], q[5], q[6], q[8]);
#endif
		}

		int mz[9];
		for (int i = 0; i < 9; i++)
			mz[i] = M(QReg, q[i]);

		int ones = 0;
		for (int i = 0; i < 9; i++)
			if (mz[i] == 1)
				ones++;
		result = ones%2;
		printf("Final data measurement : data=|q%dq%dq%d q%dq%dq%d q%dq%dq%d>\n", q[2], q[1], q[0], q[5], q[4], q[3], q[8], q[7], q[6]);
		printf("Final data measurement : data=|%d%d%d %d%d%d %d%d%d> [%c]\n", 
				mz[2], mz[1], mz[0], mz[5], mz[4], mz[3], mz[8], mz[7], mz[6], (ones%2 == 0? 'E' : 'O'));
		log_printf(q[0], q[2], q[3], q[5], q[6], q[8]);
	}

	void Logical_X(int q1, int q2, int q3) {
		X(QReg, q1);
		X(QReg, q2);
		X(QReg, q3);
		printf("\t--------> Logical X to %d/%d/%d\n", q1, q2, q3);
	}

	void Logical_Z(int q1, int q2, int q3) {
		Z(QReg, q1);
		Z(QReg, q2);
		Z(QReg, q3);
		printf("\t--------> Logical Z to %d/%d/%d\n", q1, q2, q3);
	}

	void Left_LQ_EC(int mx_stb0, int mx_stb7, int mx_stb13, int mx_stb21, int mz_stb6, int mz_stb8, int mz_stb12, int mz_stb14) {
		if(mx_stb0 == 1) {
			if(mx_stb7 == 1)
				Z(QReg, 3);
			else
				Z(QReg, 2);
		} else if(mx_stb7 == 1) {
			if(mx_stb0 == 1)
				Z(QReg, 3);
			else if(mx_stb13 == 1)
				Z(QReg, 10);
			else
				Z(QReg, 4);
		} else if(mx_stb13 == 1) {
			if(mx_stb7 == 1)
				Z(QReg, 10);
			else if(mx_stb21 == 1)
				Z(QReg, 17);
			else
				Z(QReg, 16);
		} else if(mx_stb21 == 1) {
			if(mx_stb13 == 1)
				Z(QReg, 17);
			else
				Z(QReg, 18);
		}

		if(mz_stb6 == 1) {
			if(mz_stb12 == 1)
				X(QReg, 9);
			else if(mz_stb14 == 1)
				X(QReg, 10);
			else
				X(QReg, 2);
		} else if(mz_stb8 == 1) {
			if(mz_stb14 == 1)
				X(QReg, 11);
			else
				X(QReg, 4);
		} else if(mz_stb12 == 1) {
			if(mz_stb6 == 1)
				X(QReg, 9);
			else
				X(QReg, 16);
		} else if(mz_stb14 == 1) {
			if(mz_stb6 == 1)
				X(QReg, 10);
			else if(mz_stb8 == 1)
				X(QReg, 11);
			else
				X(QReg, 18);
		}
	}

	void Left_LQ_ESM(int& mx_stb0, int& mx_stb7, int& mx_stb13, int& mx_stb21, int& mz_stb6, int& mz_stb8, int& mz_stb12, int& mz_stb14) {
		mx_stb0 = X_Stabilizer(0, 3, 2);
		mx_stb7 = X_Stabilizer(7, 3, 4, 10, 11);
		mx_stb13 = X_Stabilizer(13, 9, 10, 16, 17);
		mx_stb21 = X_Stabilizer(21, 18, 17);

		mz_stb6 = Z_Stabilizer(6, 2, 3, 9, 10);
		mz_stb8 = Z_Stabilizer(8, 4, 11);
		mz_stb12 = Z_Stabilizer(12, 9, 16);
		mz_stb14 = Z_Stabilizer(14, 10, 11, 17, 18);
#ifdef LOG
		printf("\tLeft_LQ_ESM : |x0x7x13x21> |z6z8z12z14>\n");
		printf("\t\t\tMxxxx=|%d%d%d%d>\tMzzzz=|%d%d%d%d>\n", 
				mx_stb0, mx_stb7, mx_stb13, mx_stb21, mz_stb6, mz_stb8, mz_stb12, mz_stb14);
#endif
	}

	void Create_Left_LQ(void) {
		int mx_stb0, mx_stb7, mx_stb13, mx_stb21, mz_stb6, mz_stb8, mz_stb12, mz_stb14;
		Left_LQ_ESM(mx_stb0, mx_stb7, mx_stb13, mx_stb21, mz_stb6, mz_stb8, mz_stb12, mz_stb14);
		while (mx_stb0 | mx_stb7 | mx_stb13 | mx_stb21 | mz_stb6 | mz_stb8 | mz_stb12 | mz_stb14) {
			Left_LQ_EC(mx_stb0, mx_stb7, mx_stb13, mx_stb21, mz_stb6, mz_stb8, mz_stb12, mz_stb14);
			Left_LQ_ESM(mx_stb0, mx_stb7, mx_stb13, mx_stb21, mz_stb6, mz_stb8, mz_stb12, mz_stb14);
		}
	}

	void Create_CQ(void) {
		switch (cq_type) {
			case 0:
				printf("\t--------> Input : Logical |0> state\n");
				break;
			case 1:
				printf("\t--------> Input : Logical |1> state\n");
				break;
			case 2:
				printf("\t--------> Input : Logical |+> state\n");
				for (int i = 2; i <= 4; i++)
					H(QReg, i);
				for (int i = 9; i <= 11; i++)
					H(QReg, i);
				for (int i = 16; i <= 18; i++)
					H(QReg, i);
				break;
			case 3:
				printf("\t--------> Input : Logical |-> state\n");
				Logical_X(3, 10, 17);

				for (int i = 2; i <= 4; i++)
					H(QReg, i);
				for (int i = 9; i <= 11; i++)
					H(QReg, i);
				for (int i = 16; i <= 18; i++)
					H(QReg, i);
				break;
		}

		Create_Left_LQ();

		/* flip to logical |1> states */
		if (cq_type == 1)
			Logical_X(3, 10, 17);

#ifdef DUMP
		printf("CQ: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);
#endif
	}

	void Z_Left_LQ_EC(int mx_stb6, int mx_stb8, int mx_stb12, int mx_stb14, int mz_stb0, int mz_stb7, int mz_stb13, int mz_stb21) { 
		if(mx_stb6 == 1) {
			if(mx_stb12 == 1)
				Z(QReg, 9);
			else if(mx_stb14 == 1)
				Z(QReg, 10);
			else
				Z(QReg, 2);
		} else if(mx_stb8 == 1) {
			if(mx_stb14 == 1)
				Z(QReg, 11);
			else
				Z(QReg, 4);
		} else if(mx_stb12 == 1) {
			if(mx_stb6 == 1)
				Z(QReg, 9);
			else
				Z(QReg, 16);
		} else if(mx_stb14 == 1) {
			if(mx_stb6 == 1)
				Z(QReg, 10);
			else if(mx_stb8 == 1)
				Z(QReg, 11);
			else
				Z(QReg, 18);
		}

		if(mz_stb0 == 1) {
			if(mz_stb7 == 1)
				X(QReg, 3);
			else
				X(QReg, 2);
		} else if(mz_stb7 == 1) {
			if(mz_stb0 == 1)
				X(QReg, 3);
			else if(mz_stb13 == 1)
				X(QReg, 10);
			else
				X(QReg, 4);
		} else if(mz_stb13 == 1) {
			if(mz_stb7 == 1)
				X(QReg, 10);
			else if(mz_stb21 == 1)
				X(QReg, 17);
			else
				X(QReg, 16);
		} else if(mz_stb21 == 1) {
			if(mz_stb13 == 1)
				X(QReg, 17);
			else
				X(QReg, 18);
		}
	}

	void Z_Left_LQ_ESM(int& mx_stb6, int& mx_stb8, int& mx_stb12, int& mx_stb14, int& mz_stb0, int& mz_stb7, int& mz_stb13, int& mz_stb21) { 
		mx_stb6 = X_Stabilizer(6, 2, 3, 9, 10);
		mx_stb8 = X_Stabilizer(8, 4, 11);
		mx_stb12 = X_Stabilizer(12, 9, 16);
		mx_stb14 = X_Stabilizer(14, 10, 11, 17, 18);

		mz_stb0 = Z_Stabilizer(0, 3, 2);
		mz_stb7 = Z_Stabilizer(7, 3, 4, 10, 11);
		mz_stb13 = Z_Stabilizer(13, 9, 10, 16, 17);
		mz_stb21 = Z_Stabilizer(21, 18, 17);
#ifdef LOG
		printf("\tZ_Left_LQ_ESM : |x6x8x12x14> |z0z7z13z21> \n");
		printf("\t\t\tMxxxx=|%d%d%d%d>\tMzzzz=|%d%d%d%d>\n", 
				mx_stb6, mx_stb8, mx_stb12, mx_stb14, mz_stb0, mz_stb7, mz_stb13, mz_stb21); 
#endif
	}

	void Transversal_hadamard(void) {
#if 1
		for (int i = 2; i <= 4; i++)
			H(QReg, i);
		for (int i = 9; i <= 11; i++)
			H(QReg, i);
		for (int i = 16; i <= 18; i++)
			H(QReg, i);
#else
		printf("---------> H to D2\n");
		H(QReg, 2);
		printf("DQ1: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);

		printf("---------> H to D3\n");
		H(QReg, 3);
		printf("DQ1: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);

		printf("---------> H to D4\n");
		H(QReg, 4);
		printf("DQ1: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);

		printf("---------> H to D9\n");
		H(QReg, 9);
		printf("DQ1: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);

		printf("---------> H to D10\n");
		H(QReg, 10);
		printf("DQ1: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);

		printf("---------> H to D11\n");
		H(QReg, 11);
		printf("DQ1: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);

		printf("---------> H to D16\n");
		H(QReg, 16);
		printf("DQ1: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);

		printf("---------> H to D17\n");
		H(QReg, 17);
		printf("DQ1: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);

		printf("---------> H to D18\n");
		H(QReg, 18);
		printf("DQ1: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);
#endif

		int mx_stb6, mx_stb8, mx_stb12, mx_stb14, mz_stb0, mz_stb7, mz_stb13, mz_stb21;
		Z_Left_LQ_ESM(mx_stb6, mx_stb8, mx_stb12, mx_stb14, mz_stb0, mz_stb7, mz_stb13, mz_stb21);
		while (mx_stb6 | mx_stb8 | mx_stb12 | mx_stb14 | mz_stb0 | mz_stb7 | mz_stb13 | mz_stb21) {
			Z_Left_LQ_EC(mx_stb6, mx_stb8, mx_stb12, mx_stb14, mz_stb0, mz_stb7, mz_stb13, mz_stb21);
			Z_Left_LQ_ESM(mx_stb6, mx_stb8, mx_stb12, mx_stb14, mz_stb0, mz_stb7, mz_stb13, mz_stb21);
		}
		/* measure 9 data qubits */ 
#ifdef DUMP
		printf("CQ: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);
#endif
	}

	void Right_LQ_EC(int mx_stb1, int mx_stb6, int mx_stb14, int mx_stb20, int mz_stb5, int mz_stb7, int mz_stb13, int mz_stb15) {
		if(mx_stb1 == 1) {
			if(mx_stb6 == 1)
				Z(QReg, 3);
			else
				Z(QReg, 4);
		} else if(mx_stb6 == 1) {
			if(mx_stb1 == 1)
				Z(QReg, 3);
			else if(mx_stb14 == 1)
				Z(QReg, 10);
			else
				Z(QReg, 2);
		} else if(mx_stb14 == 1) {
			if(mx_stb6 == 1)
				Z(QReg, 10);
			else if(mx_stb20 == 1)
				Z(QReg, 17);
			else
				Z(QReg, 18);
		} else if(mx_stb20 == 1) {
			if(mx_stb14 == 1)
				Z(QReg, 17);
			else
				Z(QReg, 16);
		}

		if(mz_stb5 == 1) {
			if(mz_stb13 == 1)
				X(QReg, 9);
			else
				X(QReg, 2);
		} else if(mz_stb7 == 1) {
			if(mz_stb13 == 1)
				X(QReg, 10);
			else if(mz_stb15 == 1)
				X(QReg, 11);
			else
				X(QReg, 4);
		} else if(mz_stb13 == 1) {
			if(mz_stb5 == 1)
				X(QReg, 9);
			else if(mz_stb7 == 1)
				X(QReg, 10);
			else
				X(QReg, 16);
		} else if(mz_stb15 == 1) {
			if(mz_stb7 == 1)
				X(QReg, 11);
			else
				X(QReg, 18);
		}
	}

	void Right_LQ_ESM(int& mx_stb1, int& mx_stb6, int& mx_stb14, int& mx_stb20, int& mz_stb5, int& mz_stb7, int& mz_stb13, int& mz_stb15) {
		mx_stb1 = X_Stabilizer(1, 4, 3);
		mx_stb6 = X_Stabilizer(6, 2, 3, 9, 10);
		mx_stb14 = X_Stabilizer(14, 10, 11, 17, 18);
		mx_stb20 = X_Stabilizer(20, 17, 16);

		mz_stb5 = Z_Stabilizer(5, 2, 9);
		mz_stb7 = Z_Stabilizer(7, 3, 4, 10, 11);
		mz_stb13 = Z_Stabilizer(13, 9, 10, 16, 17);
		mz_stb15 = Z_Stabilizer(15, 11, 18);
#ifdef LOG
		printf("\tRight_LQ_ESM : |x1x6x14x20> |z5z7z13z15>\n");
		printf("\t\t\tMxxxx=|%d%d%d%d>\tMzzzz=|%d%d%d%d>\n", 
				mx_stb1, mx_stb6, mx_stb14, mx_stb20, mz_stb5, mz_stb7, mz_stb13, mz_stb15);
#endif
	}

	void Convert_To_Right_LQ(void) {
#ifdef LOG
		printf("\tRight flavour qubit ESM & EC\n");
#endif
		/* run ESM and then stabilize all stabilizers to |0> states by correction */
		int mx_stb1, mx_stb6, mx_stb14, mx_stb20, mz_stb5, mz_stb7, mz_stb13, mz_stb15;
		Right_LQ_ESM(mx_stb1, mx_stb6, mx_stb14, mx_stb20, mz_stb5, mz_stb7, mz_stb13, mz_stb15);
		int mx_val = (mx_stb1 == 1 ? -1 : 1) * (mx_stb20 == 1 ? -1 : 1);
		int mz_val = (mz_stb5 == 1 ? -1 : 1) * (mz_stb15 == 1 ? -1 : 1);

		while (mx_stb1 | mx_stb6 | mx_stb14 | mx_stb20 | mz_stb5 | mz_stb7 | mz_stb13 | mz_stb15) {
			Right_LQ_EC(mx_stb1, mx_stb6, mx_stb14, mx_stb20, mz_stb5, mz_stb7, mz_stb13, mz_stb15);
			Right_LQ_ESM(mx_stb1, mx_stb6, mx_stb14, mx_stb20, mz_stb5, mz_stb7, mz_stb13, mz_stb15);
		}
		if (mx_val == -1)
			Logical_Z(9, 10, 11);
		if (mz_val == -1)
			Logical_X(3, 10, 17);
#ifdef DUMP
		printf("CQ: 4/3/2 11/10/9 18/17/16\n");
		QReg->dump(2, 4, 9, 11, 16, 18);
#endif
#if 0
		vector<int> q;
		for (int i = 2; i <= 4; i++)
			q.push_back(i);
		for (int i = 9; i <= 11; i++)
			q.push_back(i);
		for (int i = 16; i <= 18; i++)
			q.push_back(i);
		Measure_LQ(false, q);
#endif
	}

	void Left_AQ_EC(int mx_stb20, int mx_stb32, int mx_stb43, int mx_stb55, int mz_stb31, int mz_stb33, int mz_stb42, int mz_stb44) {
		if(mx_stb20 == 1) {
			if(mx_stb32 == 1)
				Z(QReg, 25);
			else
				Z(QReg, 24);
		} else if(mx_stb32 == 1) {
			if(mx_stb20 == 1)
				Z(QReg, 25);
			else if(mx_stb43 == 1)
				Z(QReg, 37);
			else
				Z(QReg, 26);
		} else if(mx_stb43 == 1) {
			if(mx_stb32 == 1)
				Z(QReg, 37);
			else if(mx_stb55 == 1)
				Z(QReg, 50);
			else
				Z(QReg, 49);
		} else if(mx_stb55 == 1) {
			if(mx_stb43 == 1)
				Z(QReg, 50);
			else
				Z(QReg, 51);
		}

		if(mz_stb31 == 1) {
			if(mz_stb42 == 1)
				X(QReg, 36);
			else if(mz_stb44 == 1)
				X(QReg, 37);
			else
				X(QReg, 24);
		} else if(mz_stb33 == 1) {
			if(mz_stb44 == 1)
				X(QReg, 38);
			else
				X(QReg, 26);
		} else if(mz_stb42 == 1) {
			if(mz_stb31 == 1)
				X(QReg, 36);
			else
				X(QReg, 49);
		} else if(mz_stb44 == 1) {
			if(mz_stb31 == 1)
				X(QReg, 37);
			else if(mz_stb33 == 1)
				X(QReg, 38);
			else
				X(QReg, 51);
		}
	}

	void Left_AQ_ESM(int& mx_stb20, int& mx_stb32, int& mx_stb43, int& mx_stb55, int& mz_stb31, int& mz_stb33, int& mz_stb42, int& mz_stb44) {
		mx_stb20 = X_Stabilizer(20, 25, 24);
		mx_stb32 = X_Stabilizer(32, 25, 26, 37, 38);
		mx_stb43 = X_Stabilizer(43, 36, 37, 49, 50);
		mx_stb55 = X_Stabilizer(55, 51, 50);

		mz_stb31 = Z_Stabilizer(31, 24, 25, 36, 37);
		mz_stb33 = Z_Stabilizer(33, 26, 38);
		mz_stb42 = Z_Stabilizer(42, 36, 49);
		mz_stb44 = Z_Stabilizer(44, 37, 38, 50, 51);
#ifdef LOG
		printf("\tLeft_AQ_ESM : |x20x32x43x55> |z31z33z42z44>\n");
		printf("\t\t\tMxxxx=|%d%d%d%d>\tMzzzz=|%d%d%d%d>\n", 
				mx_stb20, mx_stb32, mx_stb43, mx_stb55, mz_stb31, mz_stb33, mz_stb42, mz_stb44);
#endif
	}

	void Create_Left_AQ(void) {
		int mx_stb20, mx_stb32, mx_stb43, mx_stb55, mz_stb31, mz_stb33, mz_stb42, mz_stb44;
		Left_AQ_ESM(mx_stb20, mx_stb32, mx_stb43, mx_stb55, mz_stb31, mz_stb33, mz_stb42, mz_stb44);
		while (mx_stb20 | mx_stb32 | mx_stb43 | mx_stb55 | mz_stb31 | mz_stb33 | mz_stb42 | mz_stb44) {
			Left_AQ_EC(mx_stb20, mx_stb32, mx_stb43, mx_stb55, mz_stb31, mz_stb33, mz_stb42, mz_stb44);
			Left_AQ_ESM(mx_stb20, mx_stb32, mx_stb43, mx_stb55, mz_stb31, mz_stb33, mz_stb42, mz_stb44);
		}
	}

	void Create_AQ(void) {
		Create_Left_AQ();
	}

	void Mzz_Merge_Split(void) {
		printf("\tSTEP 7-1. Mzz merge \n");
		int mx_stb1 = X_Stabilizer(1, 4, 3);
		int mx_stb6 = X_Stabilizer(6, 2, 3, 9, 10);
		int mx_stb14 = X_Stabilizer(14, 10, 11, 17, 18);
		int mx_stb20 = X_Stabilizer(20, 16, 17, 24, 25);
		int mx_stb32 = X_Stabilizer(32, 25, 26, 37, 38);
		int mx_stb43 = X_Stabilizer(43, 36, 37, 49, 50);
		int mx_stb55 = X_Stabilizer(55, 51, 50);

		int mz_stb5 = Z_Stabilizer(5, 2, 9);
		int mz_stb7 = Z_Stabilizer(7, 3, 4, 10, 11);
		int mz_stb13 = Z_Stabilizer(13, 9, 10, 16, 17);
		int mz_stb15 = Z_Stabilizer(15, 11, 18);
		int mz_stb19 = Z_Stabilizer(19, 16, 24);
		int mz_stb21 = Z_Stabilizer(21, 17, 18, 25, 26);
		int mz_stb31 = Z_Stabilizer(31, 24, 25, 36, 37); 
		int mz_stb33 = Z_Stabilizer(33, 26, 38);
		int mz_stb42 = Z_Stabilizer(42, 36, 49);
		int mz_stb44 = Z_Stabilizer(44, 37, 38, 50, 51);
#ifdef LOG
		printf("\tMzz_CQ_AQ_ESM : |x1x6x14x20x32x43x55> |z5z7z13z15z19z21z31z33z42z44>\n");
		printf("\t\t\tMxxxx=|%d%d%d%d%d%d%d>\tMzzzz=|%d%d%d%d%d%d%d%d%d%d> --> new |z19z21> = |%d%d>\n", 
				mx_stb1, mx_stb6, mx_stb14, mx_stb20, mx_stb32, mx_stb43, mx_stb55, 
				mz_stb5, mz_stb7, mz_stb13, mz_stb15, mz_stb19, mz_stb21, mz_stb31, mz_stb33, mz_stb42, mz_stb44, mz_stb19, mz_stb21);
#endif
		b = (mz_stb19 == 1 ? -1 : 1) * (mz_stb21 == 1 ? -1 : 1);
#ifdef DUMP
		printf("(CQ+AQ)+TQ: 4/3/2 11/10/9 18/17/16 26/25/24 29/28/27 38/37/36 41/40/39 51/50/49 54/53/52\n");
		QReg->dump1(2, 4, 9, 11, 16, 18, 24, 26, 27, 29, 36, 38, 39, 41, 49, 51, 52, 54);
#endif

		printf("\tSTEP 7-2. Mzz split \n");
		/* CQ */
		Right_LQ_ESM(mx_stb1, mx_stb6, mx_stb14, mx_stb20, mz_stb5, mz_stb7, mz_stb13, mz_stb15);
		while (mx_stb1 | mx_stb6 | mx_stb14 | mx_stb20 | mz_stb5 | mz_stb7 | mz_stb13 | mz_stb15) {
			Right_LQ_EC(mx_stb1, mx_stb6, mx_stb14, mx_stb20, mz_stb5, mz_stb7, mz_stb13, mz_stb15);
			Right_LQ_ESM(mx_stb1, mx_stb6, mx_stb14, mx_stb20, mz_stb5, mz_stb7, mz_stb13, mz_stb15);
		}
		/* AQ */
		Create_Left_AQ();
	}

	void Right_DQ2_EC(int mx_stb23, int mx_stb34, int mx_stb47, int mx_stb56, int mz_stb33, int mz_stb35, int mz_stb46, int mz_stb48) {
		if(mx_stb23 == 1) {
			if(mx_stb34 == 1)
				Z(QReg, 28);
			else
				Z(QReg, 29);
		} else if(mx_stb34 == 1) {
			if(mx_stb23 == 1)
				Z(QReg, 28);
			else if(mx_stb47 == 1)
				Z(QReg, 40);
			else
				Z(QReg, 27);
		} else if(mx_stb47 == 1) {
			if(mx_stb34 == 1)
				Z(QReg, 40);
			else if(mx_stb56 == 1)
				Z(QReg, 53);
			else
				Z(QReg, 54);
		} else if(mx_stb56 == 1) {
			if(mx_stb47 == 1)
				Z(QReg, 53);
			else
				Z(QReg, 52);
		}

		if(mz_stb33 == 1) {
			if(mz_stb46 == 1)
				X(QReg, 39);
			else
				X(QReg, 27);
		} else if(mz_stb35 == 1) {
			if(mz_stb46 == 1)
				X(QReg, 40);
			else if(mz_stb48 == 1)
				X(QReg, 41);
			else
				X(QReg, 29);
		} else if(mz_stb46 == 1) {
			if(mz_stb33 == 1)
				X(QReg, 39);
			else if(mz_stb35 == 1)
				X(QReg, 40);
			else
				X(QReg, 52);
		} else if(mz_stb48 == 1) {
			if(mz_stb35 == 1)
				X(QReg, 41);
			else
				X(QReg, 54);
		}
	}

	void Right_DQ2_ESM(int& mx_stb23, int& mx_stb34, int& mx_stb47, int& mx_stb56, int& mz_stb33, int& mz_stb35, int& mz_stb46, int& mz_stb48) {
		mx_stb23 = X_Stabilizer(23, 29, 28);
		mx_stb34 = X_Stabilizer(34, 27, 28, 39, 40); 
		mx_stb47 = X_Stabilizer(47, 40, 41, 53, 54);
		mx_stb56 = X_Stabilizer(56, 53, 52);

		mz_stb33 = Z_Stabilizer(33, 27, 39);
		mz_stb35 = Z_Stabilizer(35, 28, 29, 40, 41);
		mz_stb46 = Z_Stabilizer(46, 39, 40, 52, 53);
		mz_stb48 = Z_Stabilizer(48, 41, 54);
#ifdef LOG
		printf("\tRight_TQ_ESM : |x23x34x47x56> |z33z35z46z48>\n");
		printf("\t\t\tMxxxx=|%d%d%d%d>\tMzzzz=|%d%d%d%d>\n", 
				mx_stb23, mx_stb34, mx_stb47, mx_stb56, mz_stb33, mz_stb35, mz_stb46, mz_stb48);
#endif
	}

	void Create_Right_LQ(void) {
		int mx_stb23, mx_stb34, mx_stb47, mx_stb56, mz_stb33, mz_stb35, mz_stb46, mz_stb48;
		Right_DQ2_ESM(mx_stb23, mx_stb34, mx_stb47, mx_stb56, mz_stb33, mz_stb35, mz_stb46, mz_stb48);
		while (mx_stb23 | mx_stb34 | mx_stb47 | mx_stb56 | mz_stb33 | mz_stb35 | mz_stb46 | mz_stb48) {
			Right_DQ2_EC(mx_stb23, mx_stb34, mx_stb47, mx_stb56, mz_stb33, mz_stb35, mz_stb46, mz_stb48);
			Right_DQ2_ESM(mx_stb23, mx_stb34, mx_stb47, mx_stb56, mz_stb33, mz_stb35, mz_stb46, mz_stb48);
		}
#ifdef DUMP
	//	printf("DQ2: 4/3/2 11/10/9 18/17/16 29/28/27 41/40/39 54/53/52\n");
	//	QReg->dump(2, 4, 9, 11, 16, 18, 27, 29, 39, 41, 52, 54);
#endif
	}

	void Create_TQ(void) {
		Create_Right_LQ();

		if (tq_type == 0)
			printf("\t--------> Set TQ in |0> state\n");
		else {
			printf("\t--------> Set TQ in |1> state\n");
			/* flip to logical |1> states */
			Logical_X(28, 40, 53);
		}
	}

	void Mxx_Merge_Split(void) {
		printf("\tSTEP 6-1. Mxx merge \n");
		int mx_stb20 = X_Stabilizer(20, 25, 24);
		int mx_stb22 = X_Stabilizer(22, 27, 26);
		int mx_stb23 = X_Stabilizer(23, 29, 28);
		int mx_stb32 = X_Stabilizer(32, 25, 26, 37, 38);
		int mx_stb34 = X_Stabilizer(34, 27, 28, 39, 40); 
		int mx_stb43 = X_Stabilizer(43, 36, 37, 49, 50);
		int mx_stb45 = X_Stabilizer(45, 38, 39, 51, 52);
		int mx_stb47 = X_Stabilizer(47, 40, 41, 53, 54);
		int mx_stb55 = X_Stabilizer(55, 51, 50);
		int mx_stb56 = X_Stabilizer(56, 53, 52);

		int mz_stb31 = Z_Stabilizer(31, 24, 25, 36, 37);
		int mz_stb33 = Z_Stabilizer(33, 26, 27, 38, 39);
		int mz_stb35 = Z_Stabilizer(35, 28, 29, 40, 41);
		int mz_stb42 = Z_Stabilizer(42, 36, 49);
		int mz_stb44 = Z_Stabilizer(44, 37, 38, 50, 51);
		int mz_stb46 = Z_Stabilizer(46, 39, 40, 52, 53);
		int mz_stb48 = Z_Stabilizer(48, 41, 54);
#ifdef LOG
		printf("\tMxx_TQ_AQ_ESM : |x20x22x23x32x34x43x45x47x55x56> |z31z33z35z42z44z46z48>\n");
		printf("\t\t\tMxxxx=|%d%d%d%d%d%d%d%d%d%d>\tMzzzz=|%d%d%d%d%d%d%d> ---> new |x22x45> = |%d%d>\n", 
				mx_stb20, mx_stb22, mx_stb23, mx_stb32, mx_stb34, mx_stb43, mx_stb45, mx_stb47, mx_stb55, mx_stb56,
				mz_stb31, mz_stb33, mz_stb35, mz_stb42, mz_stb44, mz_stb46, mz_stb48, mx_stb22, mx_stb45);
#endif
		a = (mx_stb22 == 1 ? -1 : 1) * (mx_stb45 == 1 ? -1 : 1);
#ifdef DUMP
		printf("CQ+(AQ+TQ): 4/3/2 11/10/9 18/17/16 29/28/27/26/25/24 41/40/39/38/37/36 54/53/52/51/50/49 \n");
		QReg->dump(2, 4, 9, 11, 16, 18, 24, 29, 36, 41, 49, 54);
#endif

		printf("\tSTEP 6-2. Mxx split \n");
		Create_Right_LQ();
		Create_Left_AQ();
	}

	void Measurement(void) {
		int result1, result2;

		printf("\tSTEP 9-1. Measure CQ \n");
		vector<int> q;
		for (int i = 2; i <= 4; i++)
			q.push_back(i);
		for (int i = 9; i <= 11; i++)
			q.push_back(i);
		for (int i = 16; i <= 18; i++)
			q.push_back(i);
		Measure_LQ(false, q, result1);

		printf("\tSTEP 9-2. Measure TQ \n");
		vector<int> q1;
		for (int i = 27; i <= 29; i++)
			q1.push_back(i);
		for (int i = 39; i <= 41; i++)
			q1.push_back(i);
		for (int i = 52; i <= 54; i++)
			q1.push_back(i);
		Measure_LQ(false, q1, result2);

		if (!result1 && !result2)
			state_00++;
		else if (result1 && result2)
			state_11++;
		else if (!result1 && result2)
			state_01++;
		else if (result1 && !result2)
			state_10++;
		else
			others++;
	}

public:
	HADAMARD_TYPE5(void) { 
		/* initialize quantum register */
		QReg = new QRegister(57);
	} 

	~HADAMARD_TYPE5(void) { 
		delete QReg;
	}

	void run(void) {
		int runCnt = 0;

		while (runCnt++ < loopCnt) {
			printf("\n--------------------- LOOP-%d ---------------------\n\n", runCnt-1);

			/* initialize LQ in |0> state */  
			for (int i = 0; i < QReg->getNumQubits(); i++) 
				initZ(QReg, i);

			printf("STEP 1. Create Control(CQ) with X-LEFT\n");
			Create_CQ();

			printf("\nSTEP 2. Transveral Hadamard CQ\n");
			Transversal_hadamard();

			printf("\nSTEP 3. Convert CQ from X-LEFT to X-RIGHT\n");
			Convert_To_Right_LQ();

			printf("\nSTEP 4. Create Target(TQ) with X-RIGHT\n");
			Create_TQ();
#ifdef DUMP
			printf("CQ+TQ: 4/3/2 11/10/9 18/17/16 29/28/27 41/40/39 54/53/52\n");
			QReg->dump(2, 4, 9, 11, 16, 18, 27, 29, 39, 41, 52, 54);
#endif

			printf("\nSTEP 5. Create AQ with X-LEFT in |0> state\n");
			Create_AQ();
#ifdef DUMP
			printf("CQ+AQ+TQ: 4/3/2 11/10/9 18/17/16 26/25/24 29/28/27 38/37/36 41/40/39 51/50/49 54/53/52\n");
			QReg->dump(2, 4, 9, 11, 16, 18, 24, 26, 27, 29, 36, 38, 39, 41, 49, 51, 52, 54);
#endif

			printf("\nSTEP 6. Mxx AQ and TQ : ESM round\n");
			Mxx_Merge_Split();
#ifdef DUMP
			printf("CQ+AQ+TQ: 4/3/2 11/10/9 18/17/16 26/25/24 29/28/27 38/37/36 41/40/39 51/50/49 54/53/52\n");
			QReg->dump(2, 4, 9, 11, 16, 18, 24, 26, 27, 29, 36, 38, 39, 41, 49, 51, 52, 54);
#endif

			printf("\nSTEP 7. Mzz CQ and AQ : ESM round\n");
			Mzz_Merge_Split();
#ifdef DUMP
			printf("CQ+AQ+TQ: 4/3/2 11/10/9 18/17/16 26/25/24 29/28/27 38/37/36 41/40/39 51/50/49 54/53/52\n");
			QReg->dump(2, 4, 9, 11, 16, 18, 24, 26, 27, 29, 36, 38, 39, 41, 49, 51, 52, 54);
#endif

			/* measure AQ in X-basis */
			printf("\nSTEP 8. Measure AQ in X-basis\n");
			for (int i = 24; i <= 26; i++)
				H(QReg, i);
			for (int i = 36; i <= 38; i++)
				H(QReg, i);
			for (int i = 49; i <= 51; i++)
				H(QReg, i);
			for (int i = 24; i <= 26; i++)
				c *= (M(QReg, i) == 1 ? -1 : 1);
			for (int i = 36; i <= 38; i++)
				c *= (M(QReg, i) == 1 ? -1 : 1);
			for (int i = 49; i <= 51; i++)
				c *= (M(QReg, i) == 1 ? -1 : 1);
			if (a < 0) {
				printf("		Logical Z to CQ\n");
				Logical_Z(9, 10, 11);
			}
			if (c < 0) {
				printf("		-- Logical Z to CQ\n");
				Logical_Z(9, 10, 11);
			}
			if (b < 0) {
				printf("		Logical X to TQ\n");
				Logical_X(28, 40, 53);
			}

#ifdef DUMP
			printf("CQ+TQ: 4/3/2 11/10/9 18/17/16 29/28/27 41/40/39 54/53/52\n");
			QReg->dump(2, 4, 9, 11, 16, 18, 27, 29, 39, 41, 52, 54);
#endif

			printf("\nSTEP 9. Measurement\n");
			Measurement();

			a = b = c = 1;
		}

		printf("\nTotal states : state_00(%d), state_01(%d), state_10(%d), state_11(%d), others(%d)\n", 
				state_00, state_01, state_10, state_11, others);
	}
};

int main(int argc, char **argv)
{
    HADAMARD_TYPE5 *Hadamard = new HADAMARD_TYPE5();
    Hadamard->run();
}
