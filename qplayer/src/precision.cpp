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
 * @file    precision.cpp
 * @author  Kisung Jin - ksjin@etri.re.kr
 * @date    10-09-21
 * @brief       
 */

#include "precision.h"

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

void print_binary(int size, qsize_t value) 
{
	for(int i=size-1; i>=0; i--) {
		cout << ((value >> i) & 1L);
	}
	printf("\n");
}
