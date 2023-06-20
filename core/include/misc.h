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
 * @file    misc.h
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @brief       
 */

#ifndef _MISC_H
#define _MISC_H

#include <iostream>
#include <complex>

#include "precision.h"

#define __bit_test(x,pos) ((x) & (1ULL<<(pos)))
#define __bit_set(x,pos) ((x) | (1ULL<<(pos)))
#define __bit_flip(x,pos) ((x) ^ (1ULL<<(pos)))
#define __bit_reset(x,pos) ((x) & ~(1ULL<<(pos)))

struct qubit_delimiter {
    int size;
    int qubits[128];
};

typedef enum {
    KET_ZERO=0,
    KET_ONE,
    KET_PLUS,
    KET_MINUS,
    KET_LZERO,
    KET_LONE,
    KET_LPLUS,
    KET_LMINUS,
    KET_SUPERPOSED,
    KET_UNKNOWN
} QUBIT_TYPE_T;

typedef enum { 
    TENSOR=0, 
    ENTANGLE 
} QUBIT_RELATION;

typedef enum {
	QGATE_ID = 0,
	QGATE_U1,
	QGATE_U2,
	QGATE_U3,
	QGATE_X,
	QGATE_Y,
	QGATE_Z,
	QGATE_H,
	QGATE_S,
	QGATE_P,
	QGATE_SX,
	QGATE_SDG,
	QGATE_T,
	QGATE_TDG,
	QGATE_RX,
	QGATE_RY,
	QGATE_RZ,
	QGATE_CX,
	QGATE_CZ,
	QGATE_CY,
	QGATE_CH,
	QGATE_CCX,
	QGATE_CRZ,
	QGATE_CU1,
	QGATE_CU2,
	QGATE_CU3,
	QGATE_SWAP,
	QGATE_CSWAP,
	QGATE_MEASURE
} QUANTUM_GATE;

#define MAX_GATES 29 

struct qregister_stat {
	/* Quantum Circuit */
	int qubits;
	int totalGateCalls;
	int gateCalls[MAX_GATES];

	/* Execution Time */
	double totalTime;
	double maxGateTime;
	double minGateTime;
	double avgGateTime;

	/* Quantum Circuit */
	qsize_t maxQStates;
	qsize_t finalQStates;

	/* System Resources */
	uint64_t usedMemory;
};

#define NUM_SIZES   8
static inline char big_size_map(int idx)
{
    char big_size_map[NUM_SIZES] = { 'B', 'K', 'M', 'G', 'T', 'P', 'E', 'Z' };

    return big_size_map[idx];
}

static inline char small_size_map(int idx)
{
    char small_size_map[NUM_SIZES] = { 'b', 'k', 'm', 'g', 't', 'p', 'e', 'z' };

    return small_size_map[idx];
}

static inline char *human_readable_size(unsigned long long size, char *buf)
{
    int i;
    float remain_ratio = size;

    for (i = 0; i < NUM_SIZES; i++) {
        if ((remain_ratio / 1024) < 1) {
            break;
        }
        remain_ratio = remain_ratio / 1024;
    }

    if (i >= NUM_SIZES) {
        sprintf(buf, "-");
    } else {
		if(big_size_map(i) == 'B') {
	        sprintf(buf, "%4.1f %c", remain_ratio, big_size_map(i));
		} else {
	        sprintf(buf, "%4.1f %cB", remain_ratio, big_size_map(i));
		}
	}

    return buf;
}

extern qsize_t quantum_shiftL(qsize_t left, qsize_t right);
extern qsize_t quantum_shiftR(qsize_t left, qsize_t right);
extern bool stripe_lower(qsize_t index, int qubit);
extern bool stripe_upper(qsize_t index, int qubit);
extern void print_binary(int size, qsize_t value);
extern void print_binary(int size, qsize_t value, struct qubit_delimiter *qd);
extern void to_binary(qsize_t state, qsize_t nq, char *qstring);
extern char *modeString(int mode);
extern char *relationString(int mode);
extern int getSchmidtNumber(complex_t M[4]);
extern char *gateString(int gate);
extern void getTotalMem(int *memTotal, int *memAvail);
extern int getUsedMem(void);
extern char *getUsedMemHuman(char *buf);
extern void showMemoryInfo(void);

#endif
