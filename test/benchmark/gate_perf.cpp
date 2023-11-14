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

#include <random>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>

#include "qplayer.h"

#define S_ZERO			0
#define S_ONE			1
#define S_SUPERPOSED	2

static QRegister *QReg = NULL;
static char *prog = NULL;
static int opt_qubits = 0;
static int opt_runs = 0;
static int control = 0;
static int target = 0;
static int baseMode = 0;

int get_rand(void)
{
	static std::random_device rd;
#if 0
	static std::mt19937 gen(rd());
#else
	static std::mt19937 gen(1234567);
#endif
	static std::uniform_int_distribution<int> dist(0, INT_MAX);

	return dist(gen);
}

void usage(void) 
{
	printf("usage: %s <options>\n", prog);
	printf("\n");
	printf("options:\n");
	printf(" -q <number>   : number of qubits\n");
	printf(" -r <number>   : number of gate runs\n");
}

double get_angle(void) 
{
	return (get_rand() % 100)+1;
}

void init(void)
{
 	QReg = new QRegister(opt_qubits);
	target = opt_qubits - 1;
	control = opt_qubits - 2;

	for(int i=0; i<opt_qubits-1; i++) {
		U3(QReg, i, M_PI/5, M_PI/5, M_PI/5);
	}
	printf("\n\nqubits(%d) prepare done....\n", opt_qubits);
	fflush(stdout);
}

void set_targetState(int state)
{
	if(state == S_ZERO) {
		initZ(QReg, target);
	} else if(state == S_ONE) {
		initZ(QReg, target);
		X(QReg, target);
	} else {
		initX(QReg, target);
	}
//	printf("totalStates = %ld\n", (uint64_t)QReg->getNumStates());
//	fflush(stdout);
}

void run_gate(int gtype)
{
	if(gtype == QGATE_X) {
		X(QReg, target);
	} else if(gtype == QGATE_Y) {
		Y(QReg, target);
	} else if(gtype == QGATE_Z) {
		Z(QReg, target);
	} else if(gtype == QGATE_H) {
		H(QReg, target);
	} else if(gtype == QGATE_S) {
		S(QReg, target);
	} else if(gtype == QGATE_T) {
		T(QReg, target);
	} else if(gtype == QGATE_SDG) {
		SDG(QReg, target);
	} else if(gtype == QGATE_TDG) {
		TDG(QReg, target);
	} else if(gtype == QGATE_RX) {
		RX(QReg, target, M_PI / get_angle());
	} else if(gtype == QGATE_RY) {
		RY(QReg, target, M_PI / get_angle());
	} else if(gtype == QGATE_RZ) {
		RZ(QReg, target, M_PI / get_angle());
	} else if(gtype == QGATE_U1) {
		U1(QReg, target, M_PI / get_angle());
	} else if(gtype == QGATE_U2) {
		U2(QReg, target, M_PI / get_angle(), get_angle());
	} else if(gtype == QGATE_U3) {
		U3(QReg, target, M_PI / get_angle(), get_angle(), get_angle());
	} else if(gtype == QGATE_CX) {
		CX(QReg, control, target);
	} else if(gtype == QGATE_CY) {
		CY(QReg, control, target);
	} else if(gtype == QGATE_CZ) {
		CZ(QReg, control, target);
	} else if(gtype == QGATE_CH) {
		CH(QReg, control, target);
	} else if(gtype == QGATE_CRZ) {
		CRZ(QReg, control, target, get_angle());
	} else if(gtype == QGATE_CU1) {
		CU1(QReg, control, target, get_angle());
	} else if(gtype == QGATE_CU3) {
		CU3(QReg, control, target, get_angle(), get_angle(), get_angle());
	}
}

void run_gate_loop(int gtype)
{
	QTimer timer;

	if(baseMode == KET_ZERO) {
		set_targetState(KET_ZERO);
	} else if(baseMode == KET_ONE) {
		set_targetState(KET_ONE);
	} else {
		set_targetState(KET_SUPERPOSED);
	}

	timer.start();
	for(int i=0; i<opt_runs; i++) { 
		run_gate(gtype); 
	}
	timer.end();

	double tm_sec = timer.getElapsedSec() / (double)(opt_runs);
	double tm_msec = timer.getElapsedMSec() / (double)(opt_runs);
	printf("%-10s : %10.1f secs, %10.1f ms\n", gateString(gtype), tm_sec, tm_msec);
	fflush(stdout);
}

void run(void)
{
	run_gate_loop(QGATE_Z);
	run_gate_loop(QGATE_S);
	run_gate_loop(QGATE_T);
	run_gate_loop(QGATE_SDG);
	run_gate_loop(QGATE_TDG);
	run_gate_loop(QGATE_U1);
	run_gate_loop(QGATE_RZ);
#if 0
	run_gate_loop(QGATE_CZ);
	run_gate_loop(QGATE_CRZ);
	run_gate_loop(QGATE_CU1);
#endif

	printf("\n");

	run_gate_loop(QGATE_X);
	run_gate_loop(QGATE_Y);
	run_gate_loop(QGATE_H);
	run_gate_loop(QGATE_RX);
	run_gate_loop(QGATE_RY);
	run_gate_loop(QGATE_U2);
	run_gate_loop(QGATE_U3);
#if 0
	run_gate_loop(QGATE_CX);
	run_gate_loop(QGATE_CY);
	run_gate_loop(QGATE_CH);
	run_gate_loop(QGATE_CU3);
#endif
}

void run_single_zero(void)
{
	printf("\nRun under target state is |0>\n");
	baseMode = KET_ZERO;
	run();
}

void run_single_one(void)
{
	printf("\nRun under target state is |1>\n");
	baseMode = KET_ZERO;
	run();
}

void run_superposed(void)
{
	printf("\nRun under target state is |S>\n");
	baseMode = KET_SUPERPOSED;
	run();
}

int main(int argc, char **argv)
{
	int c;

	prog = argv[0];
	opt_qubits = -1;
	opt_runs = -1;

	while ((c = getopt_long(argc, argv, "q:r:", NULL, NULL)) != -1) {
		switch(c) {
		case 'q':
			if(!optarg) usage();
			opt_qubits = atoi(optarg);
			break;		
		case 'r':
			if(!optarg) usage();
			opt_runs = atoi(optarg);
			break;		
		default:
			usage();
			exit(0);
		}
	}

	if(opt_qubits == -1 || opt_runs == -1) {
		usage();
		exit(0);
	}

	if(opt_qubits > 30) {
		printf("We only support less than 30 qubits\n");
		exit(0);
	}

	init();
//	run_single_zero();
	run_single_one();
	run_superposed();
}
