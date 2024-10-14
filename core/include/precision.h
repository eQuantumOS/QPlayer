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
 * @brief       
 */

#ifndef _PRECISION_H
#define _PRECISION_H

#include <iostream>
#include <complex>
#include <cstdint>
#include <list>

using namespace std;

#define SQRT_2   (1.4142135623730950488016887242096980785696718753769480731766797379f)
#define R_SQRT_2 (0.7071067811865475244008443621048490392848359376884740365883398690f)

#define Q_PI 3.141592653589793238462643383279502884L

#define AMPLITUDE_EPS 1e-13

typedef __uint128_t qsize_t;

#define MAX_QUBITS			128
#define QSTORE_PARTITION    1171
#define MAX_CORES			256

typedef complex<double> complex_t;

#endif
