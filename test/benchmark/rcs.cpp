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

#include "qplayer.h"

#define S_ZERO			0
#define S_ONE			1
#define S_SUPERPOSED	2

static QRegister *QReg = NULL;
static char *prog = NULL;
static int opt_qubits = 0;
static int opt_depth = 0;
static int opt_dratio = 0;
static int opt_task = 0;

typedef enum {
    GATE_ID = 0,
    GATE_U1,
    GATE_U2,
    GATE_U3,
    GATE_X,
    GATE_Y,
    GATE_Z,
    GATE_H,
    GATE_CX,
    GATE_S,
    GATE_SDG,
    GATE_T,
    GATE_TDG,
    GATE_RX,
    GATE_RY,
    GATE_RZ,
    GATE_CZ,
    GATE_CY,
    GATE_CH,
    GATE_CCX,
    GATE_CRZ,
    GATE_CU1,
    GATE_CU3,
    GATE_MEASURE
} GATE_TYPE;
#define MAX_GATE  23

struct gate_param {
	int gate;
	int control;
	int target;
	double angle1;
	double angle2;
	double angle3;
};

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
	printf(" -d <number>   : circuit depth\n");
	printf(" -r <number>   : diagonal gate ratio(0-100)\n");
	printf(" --task        : show task info.\n");
}

void setHadamard(void)
{
	for(int i=0; i<opt_qubits; i++) {
		H(QReg, i);
	}
//	printf("\n\nqubits(%d) prepare done....\n", opt_qubits);
//	fflush(stdout);
}

void gen_gate(struct gate_param *param, int qubit) 
{
	param->target = qubit;
	do {
		param->control = rand() % opt_qubits;
	} while(param->control == qubit);
	param->angle1 = M_PI / ((get_rand() % 10)+1);
	param->angle2 = M_PI / ((get_rand() % 10)+1);
	param->angle3 = M_PI / ((get_rand() % 10)+1);

#if 1
	if((rand() % 100 < opt_dratio)) {
		int gate = rand() % 9;
		if(gate == 0) {
			param->gate = GATE_Z;
		} else if(gate == 1) {
			param->gate = GATE_S;
		} else if(gate == 2) {
			param->gate = GATE_T;
		} else if(gate == 3) {
			param->gate = GATE_SDG;
		} else if(gate == 4) {
			param->gate = GATE_TDG;
		} else if(gate == 5) {
			param->gate = GATE_RZ;
		} else if(gate == 6) {
			param->gate = GATE_CZ;
		} else if(gate == 7) {
			param->gate = GATE_CU1;
		} else if(gate == 8) {
			param->gate = GATE_ID;
		}
	} else {
		int gate = rand() % 9;
		if(gate == 0) {
			param->gate = GATE_X;
		} else if(gate == 1) {
			param->gate = GATE_Y;
		} else if(gate == 2) {
			param->gate = GATE_CX;
		} else if(gate == 3) {
			param->gate = GATE_CY;
		} else if(gate == 4) {
			param->gate = GATE_RX;
		} else if(gate == 5) {
			param->gate = GATE_RY;
		} else if(gate == 6) {
			param->gate = GATE_U2;
		} else if(gate == 7) {
			param->gate = GATE_U3;
		} else if(gate == 8) {
			param->gate = GATE_ID;
		}
	}
#else
	if((rand() % 100 < opt_dratio)) {
		int gate = rand() % 8;
		if(gate == 0) {
			param->gate = GATE_Z;
		} else if(gate == 1) {
			param->gate = GATE_S;
		} else if(gate == 2) {
			param->gate = GATE_T;
		} else if(gate == 3) {
			param->gate = GATE_SDG;
		} else if(gate == 4) {
			param->gate = GATE_TDG;
		} else if(gate == 5) {
			param->gate = GATE_RZ;
		} else if(gate == 6) {
			param->gate = GATE_CZ;
		} else if(gate == 7) {
			param->gate = GATE_CU1;
		}
	} else {
		int gate = rand() % 8;
		if(gate == 0) {
			param->gate = GATE_H;
		} else if(gate == 1) {
			param->gate = GATE_X;
		} else if(gate == 2) {
			param->gate = GATE_Y;
		} else if(gate == 3) {
			param->gate = GATE_RX;
		} else if(gate == 4) {
			param->gate = GATE_RY;
		} else if(gate == 5) {
			param->gate = GATE_U2;
		} else if(gate == 6) {
			param->gate = GATE_U3;
		} else if(gate == 7) {
			param->gate = GATE_CX;
		}
	}
#endif
}

void run_gate(struct gate_param *param) 
{
	int gate = param->gate;
	int control = param->control;
	int target = param->target;
	double angle1 = param->angle1;
	double angle2 = param->angle2;
	double angle3 = param->angle3;

	if(gate == GATE_ID) {
        I(QReg, target);
	} else if(gate == GATE_X) {
        X(QReg, target);
    } else if(gate == GATE_Y) {
        Y(QReg, target);
    } else if(gate == GATE_Z) {
        Z(QReg, target);
    } else if(gate == GATE_H) {
        H(QReg, target);
    } else if(gate == GATE_S) {
        S(QReg, target);
    } else if(gate == GATE_T) {
        T(QReg, target);
    } else if(gate == GATE_SDG) {
        SDG(QReg, target);
    } else if(gate == GATE_TDG) {
        TDG(QReg, target);
    } else if(gate == GATE_RX) {
        RX(QReg, target, angle1);
    } else if(gate == GATE_RY) {
        RY(QReg, target, angle1);
    } else if(gate == GATE_RZ) {
        RZ(QReg, target, angle1);
    } else if(gate == GATE_U1) {
        U1(QReg, target, angle1);
    } else if(gate == GATE_U2) {
        U2(QReg, target, angle1, angle2);
    } else if(gate == GATE_U3) {
        U3(QReg, target, angle1, angle2, angle3);
    } else if(gate == GATE_CX) {
        CX(QReg, control, target);
    } else if(gate == GATE_CY) {
        CY(QReg, control, target);
    } else if(gate == GATE_CZ) {
        CZ(QReg, control, target);
    } else if(gate == GATE_CH) {
        CH(QReg, control, target);
    } else if(gate == GATE_CRZ) {
        CRZ(QReg, control, target, angle1);
    } else if(gate == GATE_CU1) {
        CU1(QReg, control, target, angle1);
    } else if(gate == GATE_CU3) {
        CU3(QReg, control, target, angle1, angle2, angle3);
    }
}

void test(void)
{
	struct gate_param param;

	for(int i=0; i<opt_depth; i++) {
		for(int j=0; j<opt_qubits; j++) {
			gen_gate(&param, j);
			run_gate(&param);
		}
	}
}

int main(int argc, char **argv)
{
	int c;
	QTimer timer;

	prog = argv[0];
	opt_qubits = -1;
	opt_depth = -1;
	opt_dratio = -1;

	static const struct option options[] = {
		{"task", 0, 0, '1'}
	};

	while ((c = getopt_long(argc, argv, "q:d:r:", options, NULL)) != -1) {
		switch(c) {
		case 'q':
			if(!optarg) usage();
			opt_qubits = atoi(optarg);
			break;		
		case 'd':
			if(!optarg) usage();
			opt_depth = atoi(optarg);
			break;		
		case 'r':
			if(!optarg) usage();
			opt_dratio = atoi(optarg);
			break;		
		case '1':
			opt_task = 1;
			break;		
		default:
			usage();
			exit(0);
		}
	}

	if(opt_qubits == -1 || opt_depth == -1 || opt_dratio == -1) {
		usage();
		exit(0);
	}

	if(opt_qubits > 30) {
		printf("We only support less than 30 qubits\n");
		exit(0);
	}

 	QReg = new QRegister(opt_qubits);

#if 1
	for(int i=0; i<=100; i++) {
		QReg->reset();
		opt_dratio = i * 1;
		setHadamard();
		timer.start();
		test();
		timer.end();
		printf("dratio=%02d -> %20s (%.0f sec)\n", opt_dratio, timer.getTime(), timer.getElapsedSec());
		fflush(stdout);
	}
#else
	setHadamard();
	timer.start();
	test();
	timer.end();
	printf("dratio=%02d -> %s\n", opt_dratio, timer.getTime());
	fflush(stdout);
#endif

	if(opt_task) {
		QReg->showQRegStat();
	}
}
