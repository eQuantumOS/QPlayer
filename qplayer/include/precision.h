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
 * @file    precision.h
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @date    10-09-21
 * @brief       
 */

#ifndef _PRECISION_H
#define _PRECISION_H

#include <iostream>
#include <complex>

using namespace std;

#define SQRT_2   (1.4142135623730950488016887242096980785696718753769480731766797379f)
#define R_SQRT_2 (0.7071067811865475244008443621048490392848359376884740365883398690f)

#define Q_PI 3.141592653589793238462643383279502884L

#define AMPLITUDE_EPS 1e-13

#define __bit_test(x,pos) ((x) & (1ULL<<(pos)))
#define __bit_set(x,pos) ((x) | (1ULL<<(pos)))
#define __bit_flip(x,pos) ((x) ^ (1ULL<<(pos)))
#define __bit_reset(x,pos) ((x) & ~(1ULL<<(pos)))

#ifdef LARGE_SCALE
#include <boost/multiprecision/cpp_int.hpp>
using namespace boost::multiprecision;
#endif

#ifdef LARGE_SCALE
#if 1
typedef uint128_t qsize_t;
#define MAX_QUBITS	128
#else
//typedef uint256_t qsize_t;
//typedef uint512_t qsize_t;
//typedef uint1024_t qsize_t;
#endif
#else
typedef uint64_t qsize_t;
#define MAX_QUBITS	64
#endif

#define MAX_LQUBITS 20
#define QSTORE_PARTITION    1024

struct qubit_delimiter {
    int size;
    int qubits[128];
};

typedef complex<double> complex_t;

typedef enum {
    KET_ZERO=0,
    KET_ONE,
    KET_PLUS,
    KET_MINUS,
    KET_SPLUS,
    KET_SMINUS,
    KET_UNKNOWN
} QUBIT_TYPE_T;

extern qsize_t quantum_shiftL(qsize_t left, qsize_t right);
extern qsize_t quantum_shiftR(qsize_t left, qsize_t right);
extern bool stripe_lower(qsize_t index, int qubit);
extern bool stripe_upper(qsize_t index, int qubit);
extern void print_binary(int size, qsize_t value);

#endif
