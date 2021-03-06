/*
 * Copyright (c) 2021 Electronics and Telecommunications Research Institute 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/**
 * @file    misc.cpp
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @brief       
 */

#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#include "precision.h"
#include "misc.h"

using namespace std;

qsize_t quantum_shiftL(qsize_t left, qsize_t right)
{
#ifdef LARGE_SCALE
    return left << right.convert_to<size_t>();
#else
    return left << right;
#endif
}

qsize_t quantum_shiftR(qsize_t left, qsize_t right)
{
#ifdef LARGE_SCALE
    return left >> right.convert_to<size_t>();
#else
    return left >> right;
#endif
}

/*
 * Check if the qubit in the lower bound of stripe. The meaning of 
 * lower/upper bound is illustrated in the example below. 
 *
 *            Q1     Q0
 *   |00>    lower  lower
 *   |01>    lower  upper
 *   |10>    upper  lower
 *   |11>    upper  upper
 */
bool stripe_lower(qsize_t index, int qubit)
{
	qsize_t stride = quantum_shiftL(1, (qsize_t)qubit);
	qsize_t stripe_width = stride * 2ULL;

	if((index % stripe_width) < stride) {
		return true;
	}

	return false;
}

/*
 * Check if the qubit in the upper bound of stripe
 */
bool stripe_upper(qsize_t index, int qubit)
{
	qsize_t stride = quantum_shiftL(1, (qsize_t)qubit);
	qsize_t stripe_width = stride * 2ULL;

	if((index % stripe_width) < stride) {
		return false;
	}

	return true;
}

/* 
 * Displays quantum state. Each qubit is printed in reverse order.
 *
 * param state : index of quantum states
 * param nq    : total number of qubits
 *
 * Let's suppose a mixed state of two qubits as follows. 
 *  - number of qubit : 2
 *  - quantum states  : 4 2^2
 *  - outputs are...
 *    if(state == 0)      --> |00>
 *    else if(state == 1) --> |01>   --> q0=1, q1=0
 *    else if(state == 2) --> |10>   --> q0=0, q1=1
 *    else if(state == 3) --> |11>
*/
void to_binary(qsize_t state, qsize_t nq, char *qstring)
{
	int pos = 0;
	while (nq--) {
		qstring[pos++] = ((quantum_shiftR(state, nq)) & 1) ? '1' : '0';
	}
}

void print_binary(int size, qsize_t value) 
{
	for(int i=size-1; i>=0; i--) {
		cout << ((value >> i) & 1L);
	}
	printf("\n");
}

void print_binary(int size, qsize_t value, struct qubit_delimiter *qd) 
{
	for(int i=size-1; i>=0; i--) {
		if(qd != NULL) {
			for(int j=0; j<qd->size; j++) {
				if(i == qd->qubits[j]) {
					printf(" ");
					break;
				}
			}
		}
		cout << ((value >> i) & 1L);
	}
	printf("\n");
}

char *modeString(int mode) {
	if(mode == KET_ZERO) {
		return "|0>";
	} else if(mode == KET_ONE) {
		return "|1>";
	} else if(mode == KET_PLUS) {
		return "|+>";
	} else if(mode == KET_MINUS) {
		return "|->";
	} else if(mode == KET_LZERO) {
		return "|L0>";
	} else if(mode == KET_LONE) {
		return "|L1>";
	} else if(mode == KET_LPLUS) {
		return "|L+>";
	} else if(mode == KET_LMINUS) {
		return "|L->";
	}

	return "UNKNOWN";
}

char *relationString(int type) {
	if(type == TENSOR) {
		return "TENSOR PRODUCT";
	} else if(type == ENTANGLE) {
		return "ENTANGLE STATE";
	}

	return "UNKNOWN";
}


static void OuterProduct(complex_t S[4], complex_t **PT) 
{
	complex_t OM[16];

	for(int i=0; i<4; i++) 
		for(int j=0; j<4; j++) 
			 OM[i*4+j] = S[i] * S[j];

#if 0
	printf("--> density matrix\n");
	for(int i=0; i<16; i++) {
		cout << OM[i] << " ";
	}
	cout << endl << endl;
#endif

	(*PT)[0] = OM[0] + OM[9];
	(*PT)[1] = OM[2] + OM[11];
	(*PT)[2] = OM[4] + OM[13];
	(*PT)[3] = OM[6] + OM[15];
}

static int QRDecompositionMethod(complex_t M[2][2]) 
{
	int SchmidtNum = 0;
	int maxIteration = 1000;
	int iteration = 0;
	double eps = 1E-13;

	while (iteration < maxIteration) {
		complex_t MR[2][2];
		complex_t MQ[2][2];

		for(int i=0; i<2; i++) 
			for(int j=0; j<2; j++) 
				MR[i][j] = MQ[i][j] = 0;

		for (int j = 0; j < 2; j++) {
			for (int i = 0; i < j; i++)
				for (int k = 0; k < 2; k++)
					MR[i][j] += M[k][j] * MQ[k][i];
			
			for (int i = 0; i < 2; i++) {
				MQ[i][j] = M[i][j];

				for (int k = 0; k < j; k++)
					MQ[i][j] -= MR[k][j] * MQ[i][k];

				MR[j][j] += MQ[i][j] * MQ[i][j];
			}

			MR[j][j] = sqrt(MR[j][j]);

			for (int i = 0; i < 2; i++) {
				MQ[i][j] /= MR[j][j];
			}
		}

		double max = 0;

		for (int i = 0; i < 2; i++)
			for (int j = i + 1; j < 2; j++)
				if (norm(MQ[i][j]) > max)
					max = norm(MQ[i][j]);

		if (max < eps)
			break;

		M[0][0] = (MR[0][0] * MQ[0][0]) + (MR[0][1] * MQ[1][0]);
		M[0][1] = (MR[0][0] * MQ[0][1]) + (MR[0][1] * MQ[1][1]);
		M[1][0] = (MR[1][0] * MQ[0][0]) + (MR[1][1] * MQ[1][0]);
		M[1][1] = (MR[1][0] * MQ[0][1]) + (MR[1][1] * MQ[1][1]);
		iteration++;
	}

	if (iteration != maxIteration) {
		complex_t eigenvalues[2];
		for (int i = 0; i < 2; i++) {
			bool found = false;

			if (norm(M[i][i]) < eps) {
				continue;
			}

			eigenvalues[SchmidtNum++] = M[i][i];
		}
	}

#if 0
	cout << "SchmidtNum = " << SchmidtNum << endl;
#endif

	return SchmidtNum;
}

int getSchmidtNumber(complex_t M[4])
{
	complex_t *PT = (complex_t *)malloc(sizeof(complex_t) * 4);
	complex_t MT[2][2];

	OuterProduct(M, &PT);

#if 0
	printf("--> partial traces\n");
	for(int i=0; i<4; i++) {
		cout << PT[i] << " ";
	}
	cout << endl << endl;
#endif

	MT[0][0] = PT[0];
	MT[0][1] = PT[1];
	MT[1][0] = PT[2];
	MT[1][1] = PT[3];

	free(PT);

	return QRDecompositionMethod(MT);
}

/* 
 * Returns maximum allocated physical memory size. QPlayer uses VmHWM 
 * values because the memory size changes dynamically depending on the 
 * number of non-zero amplitude states. 
 */
void checkMemory(void) {
	FILE *fp = NULL;
	char fname[256] = "";
	char cmd[256] = "";
	char size[32] = "";

	sprintf(fname, "/proc/%d/status", getpid());

	fp = fopen(fname, "r");
	if(fp == NULL) {
		return;
	}

	while(fgets(cmd, 256, fp) != NULL) {
		if(strstr(cmd, "VmHWM")) {
			char t[32] = "";
			sscanf(cmd, "%s%s", t, size);
			break;
		}
	}

	printf("allocated memory = %s KB\n", size);

	fclose(fp);

	return;
}

