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

static qsize_t strides[MAX_QUBITS];

std::ostream&
operator<<( std::ostream& dest, qsize_t value)
{
    std::ostream::sentry s( dest );
    if ( s ) {
        __uint128_t tmp = value < 0 ? -value : value;
        char buffer[ 128 ];
        char* d = std::end( buffer );
        do
        {
            -- d;
            *d = "0123456789"[ tmp % 10 ];
            tmp /= 10;
        } while ( tmp != 0 );
        if ( value < 0 ) {
            -- d;
            *d = '-';
        }
        int len = std::end( buffer ) - d;
        if ( dest.rdbuf()->sputn( d, len ) != len ) {
            dest.setstate( std::ios_base::badbit );
        }
    }
    return dest;
}

void init_strides(void)
{
	for(int i=0; i<MAX_QUBITS; i++) {
		strides[i] = quantum_shiftL(1, (qsize_t)i);
	}
}

qsize_t get_stride(int qubit)
{
	return strides[qubit];
}

qsize_t quantum_shiftL(qsize_t left, qsize_t right)
{
    return left << right;
}

qsize_t quantum_shiftR(qsize_t left, qsize_t right)
{
    return left >> right;
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
	qsize_t stride = strides[qubit];

	if((index & stride) != stride) {
		return true;
	}

	return false;
}

/*
 * Check if the qubit in the upper bound of stripe
 */
bool stripe_upper(qsize_t index, int qubit)
{
	qsize_t stride = strides[qubit];

	if((index & stride) == stride) {
		return true;
	}

	return false;
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

char *gateString(int gate) 
{
	switch(gate) {
	case QGATE_ID: return "ID";
	case QGATE_U1: return "U1";
	case QGATE_U2: return "U2";
	case QGATE_U3: return "U3";
	case QGATE_X: return "Pauli-X";
	case QGATE_Y: return "Pauli-Y";
	case QGATE_Z: return "Pauli-Z";
	case QGATE_H: return "H";
	case QGATE_P: return "P";
	case QGATE_S: return "S";
	case QGATE_T: return "T";
	case QGATE_SDG: return "SDG";
	case QGATE_TDG: return "TDG";
	case QGATE_RX: return "RX";
	case QGATE_RY: return "RY";
	case QGATE_RZ: return "RZ";
	case QGATE_CX: return "CX";
	case QGATE_CZ: return "CZ";
	case QGATE_CY: return "CY";
	case QGATE_CH: return "CH";
	case QGATE_CCX: return "CCX";
	case QGATE_CRZ: return "CRZ";
	case QGATE_CU1: return "CU1";
	case QGATE_CU2: return "CU2";
	case QGATE_CU3: return "CU3";
	case QGATE_SWAP: return "SWAP";
	case QGATE_CSWAP: return "CSWAP";
	case QGATE_SX: return "SX";
	case QGATE_SXDG: return "SXDG";
	case QGATE_ISWAP: return "ISWAP";
	case QGATE_MEASURE: return "MEASURE";
	}

	return "UNKNOWN GATE";
}

/* 
 * get pysical memory size(bytes) for this system 
 */
void getTotalMem(uint64_t *memTotal, uint64_t *memAvail) {
	FILE *fp = NULL;

	*memTotal = 0;
	*memAvail = 0;
	fp = fopen("/proc/meminfo", "rt");
	if(fp == NULL) {
		return;
	}

	while(!feof(fp)) {
		char cmd[256] = "";
		char kb[256] = "";
		char unit[256] = "";

		fscanf(fp, "%s %s %s\n", cmd, kb, unit);
		if(strcmp(cmd, "MemTotal:") == 0) {
			*memTotal = (uint64_t)atoi(kb) * 1024ULL;
		} else if(strcmp(cmd, "MemAvailable:") == 0) {
			*memAvail = (uint64_t)atoi(kb) * 1024ULL;
		}
	}

	fclose(fp);
}

/* 
 * get free memory size(bytes) for this system 
 */
uint64_t getUsedMem(void) {
	FILE *fp = NULL;
	char fname[256] = "";
	char cmd[256] = "";
	char size[32] = "";
	uint64_t memFree = 0;

	sprintf(fname, "/proc/%d/status", getpid());

	fp = fopen(fname, "r");
	if(fp == NULL) {
		return 0;
	}

	while(fgets(cmd, 256, fp) != NULL) {
		if(strstr(cmd, "VmHWM")) {
			char t[32] = "";
			sscanf(cmd, "%s%s", t, size);
			memFree = (uint64_t)atoi(size) * 1024ULL;
			break;
		}
	}

	fclose(fp);

	return memFree;
}

/* 
 * get pysical memory size(bytes) for this system 
 */
void getMemory(uint64_t *memTotal, uint64_t *memAvail, uint64_t *memUsed)
{
	getTotalMem(memTotal, memAvail);
	*memUsed = getUsedMem();
}


void getCPU(char *cpu, int *cores, char *herz)
{
	FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
	char line[256];

	if(cpuinfo == NULL) {
		return;
	}

	*cores = *herz = 0;

	while(fgets(line, sizeof(line), cpuinfo)) {
		if(strncmp(line, "model name", 10) == 0) {
			char *str = strchr(line, ':');
			if(str != NULL) {
				str++;
				while(*str == ' ') {
					++str;
				}
				strcpy(cpu, str);
				cpu[strlen(cpu)-1] = '\0';
			}
		} else if(strncmp(line, "cpu MHz", 7) == 0) {
			char *str = strchr(line, ':');
			if(str != NULL) {
				str++;
				while(*str == ' ') {
					++str;
				}
				strcpy(herz, str);
				herz[strlen(herz)-1] = '\0';
			}
		} else if(strncmp(line, "processor", 9) == 0) {
			(*cores)++;
		}
	}

	fclose(cpuinfo);
}

void getOS(char *name, char *version)
{
	FILE* osinfo = fopen("/etc/os-release", "r");
	char line[256];

	if(osinfo == NULL) {
		return;
	}

	while(fgets(line, sizeof(line), osinfo)) {
		if(strncmp(line, "NAME=", 5) == 0) {
			char *str = strchr(line, '"');
			if(str != NULL) {
				str++;
				strcpy(name, str);
				name[strlen(name)-2] = '\0';
			}
		} else if(strncmp(line, "VERSION=", 8) == 0) {
			char *str = strchr(line, '"');
			if(str != NULL) {
				str++;
				strcpy(version, str);
				version[strlen(version)-2] = '\0';
			}
		}
	}

	fclose(osinfo);
}

char *getUsedMemHuman(char *buf) 
{
	uint64_t mem = getUsedMem();

	human_readable_size(mem, buf);

	return buf;
}

void showMemoryInfo(void) 
{
	uint64_t memTotal = 0;
	uint64_t memAvail = 0;
	uint64_t memUsed = 0;

	char memTotalStr[32] = "";
	char memAvailStr[32] = "";
	char memUsedStr[32] = "";

	getTotalMem(&memTotal, &memAvail);
	memUsed = getUsedMem();

	human_readable_size(memTotal, memTotalStr);
	human_readable_size(memAvail, memAvailStr);
	human_readable_size(memUsed, memUsedStr);

	printf("Total:%s, Avail=%s --> Used:%s\n", memTotalStr, memAvailStr, memUsedStr);
}

void sec2str(double tm, char *str)
{
	return nsec2str(tm * 1000000000LL, str);
}

void msec2str(double tm, char *str)
{
	return nsec2str(tm * 1000000LL, str);
}

void usec2str(double tm, char *str)
{
	return nsec2str(tm * 1000LL, str);
}

void nsec2str(double tm, char *str)
{
	double elapsedNSec = tm;
	double elapsedUSec = elapsedNSec / 1000LL;
	double elapsedMSec = elapsedNSec / 1000000LL;
	double elapsedSec  = elapsedNSec / 1000000000LL;

	int h = (int)elapsedSec / 3600;
	int m = (int)(((long)elapsedSec / 60) % 60);
	int s = (int)((long)elapsedSec % 60);

	if(s > 0) {
		double remainingMSec = elapsedMSec - ((uint64_t)elapsedSec * 1000LL);
		sprintf(str, "%02d:%02d:%02d + %dms", h, m, s, (uint32_t)remainingMSec);
	} else if(elapsedMSec >= 1) {
		sprintf(str, "%f ms", elapsedMSec);
	} else if(elapsedUSec >= 1 ) {
		sprintf(str, "%f us", elapsedUSec);
	} else if(elapsedNSec > 0) {
		sprintf(str, "%f ns", elapsedNSec);
	}
}

void set_affinity(int coreid)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(coreid, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

bool isRealizedState(complex_t amp)
{
#if 1
	if(norm(amp) > AMPLITUDE_EPS) {
		return true;
	} else {
		return false;
	}
#else
	if(abs(amp.real()) > AMPLITUDE_EPS || abs(amp.imag()) > AMPLITUDE_EPS) {
		return true;
	} else {
		return false;
	}
#endif
}
