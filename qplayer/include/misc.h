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

extern qsize_t quantum_shiftL(qsize_t left, qsize_t right);
extern qsize_t quantum_shiftR(qsize_t left, qsize_t right);
extern bool stripe_lower(qsize_t index, int qubit);
extern bool stripe_upper(qsize_t index, int qubit);
extern void print_binary(int size, qsize_t value);
extern void print_binary(int size, qsize_t value, struct qubit_delimiter *qd);
extern void to_binary(qsize_t state, qsize_t nq, char *qstring);
extern char *modeString(int mode);
extern char *relationString(int mode);
extern void checkMemory(void);

#endif
