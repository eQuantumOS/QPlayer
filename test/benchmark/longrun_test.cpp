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

static QRegister *QReg = NULL;
static char *prog = NULL;
static int opt_qubits = 0;
static int opt_runs = 0;
static bool opt_qasm = false;
static bool opt_states = false;
static bool opt_task = false;
static FILE *fp1 = NULL;
static FILE *fp2 = NULL;

struct gate_params {
	int gate;
	int qubit;
	int control1;
	int control2;
	double angle;
	double theta;
	double phi;
	double rambda;
};

typedef enum {
	GATE_U1 = 0,
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

void showGate(struct gate_params *p) 
{
	if(p->gate == GATE_U1) { 
		fprintf(fp1, "u1(%f) q[%d];\n", p->rambda, p->qubit);
		fprintf(fp2, "\tU1(QReg, %d, %f);\n", p->qubit, p->rambda); 
	} else if(p->gate == GATE_U2) { 
		fprintf(fp1, "u2(%f, %f) q[%d];\n", p->phi, p->rambda, p->qubit); 
		fprintf(fp2, "\tU2(QReg, %d, %f, %f);\n", p->qubit, p->phi, p->rambda); 
	} else if(p->gate == GATE_U3) { 
		fprintf(fp1, "u3(%f, %f, %f) q[%d];\n", p->theta, p->phi, p->rambda, p->qubit); 
		fprintf(fp2, "\tU3(QReg, %d, %f, %f, %f);\n", p->qubit, p->theta, p->phi, p->rambda); 
	} else if(p->gate == GATE_X) { 
		fprintf(fp1, "x q[%d];\n", p->qubit); 
		fprintf(fp2, "\tX(QReg, %d);\n", p->qubit); 
	} else if(p->gate == GATE_Y) { 
		fprintf(fp1, "y q[%d];\n", p->qubit); 
		fprintf(fp2, "\tY(QReg, %d);\n", p->qubit); 
	} else if(p->gate == GATE_Z) { 
		fprintf(fp1, "z q[%d];\n", p->qubit); 
		fprintf(fp2, "\tZ(QReg, %d);\n", p->qubit); 
	} else if(p->gate == GATE_H) { 
		fprintf(fp1, "h q[%d];\n", p->qubit); 
		fprintf(fp2, "\tH(QReg, %d);\n", p->qubit); 
	} else if(p->gate == GATE_S) { 
		fprintf(fp1, "s q[%d];\n", p->qubit); 
		fprintf(fp2, "\tS(QReg, %d);\n", p->qubit); 
	} else if(p->gate == GATE_SDG) { 
		fprintf(fp1, "sdg q[%d];\n", p->qubit); 
		fprintf(fp2, "\tSDG(QReg, %d);\n", p->qubit); 
	} else if(p->gate == GATE_T) { 
		fprintf(fp1, "t q[%d];\n", p->qubit); 
		fprintf(fp2, "\tT(QReg, %d);\n", p->qubit); 
	} else if(p->gate == GATE_TDG) { 
		fprintf(fp1, "tdg q[%d];\n", p->qubit); 
		fprintf(fp2, "\tTDG(QReg, %d);\n", p->qubit); 
	} else if(p->gate == GATE_RX) { 
		fprintf(fp1, "rx(%f) q[%d];\n", p->angle, p->qubit); 
		fprintf(fp2, "\tRX(QReg, %d, %f);\n", p->qubit, p->angle); 
	} else if(p->gate == GATE_RY) { 
		fprintf(fp1, "ry(%f) q[%d];\n", p->angle, p->qubit); 
		fprintf(fp2, "\tRY(QReg, %d, %f);\n", p->qubit, p->angle); 
	} else if(p->gate == GATE_RZ) { 
		fprintf(fp1, "rz(%f) q[%d];\n", p->angle, p->qubit); 
		fprintf(fp2, "\tRZ(QReg, %d, %f);\n", p->qubit, p->angle); 
	} else if(p->gate == GATE_CX) { 
		fprintf(fp1, "cx q[%d],q[%d];\n", p->control1, p->qubit);
		fprintf(fp2, "\tCX(QReg, %d, %d);\n", p->control1, p->qubit);
	} else if(p->gate == GATE_CZ) { 
		fprintf(fp1, "cz q[%d],q[%d];\n", p->control1, p->qubit);
		fprintf(fp2, "\tCZ(QReg, %d, %d);\n", p->control1, p->qubit); 
	} else if(p->gate == GATE_CY) { 
		fprintf(fp1, "cy q[%d],q[%d];\n", p->control1, p->qubit);
		fprintf(fp2, "\tCY(QReg, %d, %d);\n", p->control1, p->qubit); 
	} else if(p->gate == GATE_CH) { 
		fprintf(fp1, "ch q[%d],q[%d];\n", p->control1, p->qubit);
		fprintf(fp2, "\tCH(QReg, %d, %d);\n", p->control1, p->qubit); 
	} else if(p->gate == GATE_CCX) { 
		fprintf(fp1, "ccx q[%d],q[%d],q[%d];\n", p->control1, p->control2, p->qubit); 
		fprintf(fp2, "\tCCX(QReg, %d, %d, %d);\n", p->control1, p->control2, p->qubit); 
	} else if(p->gate == GATE_CRZ) { 
		fprintf(fp1, "crz(%f) q[%d],q[%d];\n", p->angle, p->control1, p->control2); 
		fprintf(fp2, "\tCRZ(QReg, %d, %d, %f);\n", p->control1, p->control2, p->angle); 
	} else if(p->gate == GATE_CU1) { 
		fprintf(fp1, "cu1(%f) q[%d],q[%d];\n", p->rambda, p->control1, p->qubit); 
		fprintf(fp2, "\tCU1(QReg, %d, %d, %f);\n", p->control1, p->qubit, p->rambda); 
	} else if(p->gate == GATE_CU3) { 
		fprintf(fp1, "cu3(%f, %f, %f) q[%d],q[%d];\n", p->theta, p->phi, p->rambda, p->control1, p->qubit); 
		fprintf(fp2, "\tCU3(QReg, %d, %d, %f, %f, %f);\n", p->control1, p->qubit, p->theta, p->phi, p->rambda); 
	} else if(p->gate == GATE_MEASURE) { 
		fprintf(fp1, "measure q[%d] -> c[%d];\n", p->qubit, p->qubit); 
		// fprintf(fp2, "\tM(QReg, %d);\n", p->qubit); 
	}
}

int select_gate(void) 
{
	int ratio_U1 = 100;
	int ratio_U2 = 100;
	int ratio_U3 = 100;
	int ratio_X = 100;
	int ratio_Y = 100;
	int ratio_Z = 100;
	int ratio_H = 100;
	int ratio_S = 100;
	int ratio_SDG = 100;
	int ratio_T = 100;
	int ratio_TDG = 100;
	int ratio_RX = 100;
	int ratio_RY = 100;
	int ratio_RZ = 100;
	int ratio_CCX = 0;
#if 1
	int ratio_CX = 100;
	int ratio_CY = 100;
	int ratio_CZ = 100;
	int ratio_CH = 100;
	int ratio_CRZ = 100;
	int ratio_CU1 = 100;
	int ratio_CU3 = 100;
#else
	int ratio_CX = 0;
	int ratio_CY = 0;
	int ratio_CZ = 0;
	int ratio_CH = 0;
	int ratio_CRZ = 0;
	int ratio_CU1 = 0;
	int ratio_CU3 = 0;
#endif
#if 1
	int ratio_MEASURE = 100;
#else
	int ratio_MEASURE = 0;
#endif
	int ratio;
	int gate;

	while(1) {
		gate = get_rand() % MAX_GATE;
		ratio = get_rand() % 100;
		if(gate == GATE_U1) { 
			if(ratio >= ratio_U1) { continue; }
		} else if(gate == GATE_U2) { 
			if(ratio >= ratio_U2) { continue; }
		} else if(gate == GATE_U3) { 
			if(ratio >= ratio_U3) { continue; }
		} else if(gate == GATE_X) { 
			if(ratio >= ratio_X) { continue; }
		} else if(gate == GATE_Y) { 
			if(ratio >= ratio_Y) { continue; }
		} else if(gate == GATE_Z) { 
			if(ratio >= ratio_Z) { continue; }
		} else if(gate == GATE_H) { 
			if(ratio >= ratio_H) { continue; }
		} else if(gate == GATE_S) { 
			if(ratio >= ratio_S) { continue; }
		} else if(gate == GATE_SDG) { 
			if(ratio >= ratio_SDG) { continue; }
		} else if(gate == GATE_T) { 
			if(ratio >= ratio_T) { continue; }
		} else if(gate == GATE_TDG) { 
			if(ratio >= ratio_TDG) { continue; }
		} else if(gate == GATE_RX) { 
			if(ratio >= ratio_RX) { continue; }
		} else if(gate == GATE_RY) { 
			if(ratio >= ratio_RY) { continue; }
		} else if(gate == GATE_RZ) { 
			if(ratio >= ratio_RZ) { continue; }
		} else if(gate == GATE_CX) { 
			if(ratio >= ratio_CX) { continue; }
		} else if(gate == GATE_CZ) { 
			if(ratio >= ratio_CZ) { continue; }
		} else if(gate == GATE_CY) { 
			if(ratio >= ratio_CY) { continue; }
		} else if(gate == GATE_CH) { 
			if(ratio >= ratio_CH) { continue; }
		} else if(gate == GATE_CCX) { 
			if(ratio >= ratio_CCX) { continue; }
		} else if(gate == GATE_CRZ) { 
			if(ratio >= ratio_CRZ) { continue; }
		} else if(gate == GATE_CU1) { 
			if(ratio >= ratio_CU1) { continue; }
		} else if(gate == GATE_CU3) { 
			if(ratio >= ratio_CU3) { continue; }
		} else if(gate == GATE_MEASURE) { 
			if(ratio >= ratio_MEASURE) { continue; }
		}
		break;
	} 

	return gate;
}

void get_params(struct gate_params *param)
{
	param->gate = select_gate();
	param->qubit = get_rand() % opt_qubits;
	param->angle = M_PI / ((get_rand() % 100)+1);
	param->theta = M_PI / ((get_rand() % 100)+1);
	param->phi = M_PI / ((get_rand() % 100)+1);
	param->rambda = M_PI / ((get_rand() % 100)+1);

	do {
		param->control1 = get_rand() % opt_qubits;
		if(param->control1 != param->qubit) {
			break;
		}
	} while(1);

	do {
		param->control2 = get_rand() % opt_qubits;
		if(param->control2 != param->qubit && param->control2 != param->control1) {
			break;
		}
	} while(1);
}

void long_run_test(void) 
{
	struct gate_params param;
	struct gate_params *p;
	int count = 0;

	for(int i=0; i<opt_runs; i++) {
		get_params(&param);	

		if(opt_qasm) {
			showGate(&param);
		}

		p = &param;

		switch(p->gate) {
		case GATE_U1:
			U1(QReg, p->qubit, p->rambda);
			break;
		case GATE_U2:
			U2(QReg, p->qubit, p->phi, p->rambda);
			break;
		case GATE_U3:
			U3(QReg, p->qubit, p->theta, p->phi, p->rambda);
			break;
		case GATE_X:
			X(QReg, p->qubit);
			break;
		case GATE_Y:
			Y(QReg, p->qubit);
			break;
		case GATE_Z:
			Z(QReg, p->qubit);
			break;
		case GATE_H:
			H(QReg, p->qubit);
			break;
		case GATE_S:
			S(QReg, p->qubit);
			break;
		case GATE_SDG:
			SDG(QReg, p->qubit);
			break;
		case GATE_T:
			T(QReg, p->qubit);
			break;
		case GATE_TDG:
			TDG(QReg, p->qubit);
			break;
		case GATE_RX:
			RX(QReg, p->qubit, p->angle);
			break;
		case GATE_RY:
			RY(QReg, p->qubit, p->angle);
			break;
		case GATE_RZ:
			RZ(QReg, p->qubit, p->angle);
			break;
		case GATE_CX:
			CX(QReg, p->control1, p->qubit);
			break;
		case GATE_CZ:
			CZ(QReg, p->control1, p->qubit);
			break;
		case GATE_CY:
			CY(QReg, p->control1, p->qubit);
			break;
		case GATE_CH:
			CH(QReg, p->control1, p->qubit);
			break;
		case GATE_CCX:
			CCX(QReg, p->control1, p->control2, p->qubit);
			break;
		case GATE_CRZ:
			CRZ(QReg, p->control1, p->qubit, p->angle);
			break;
		case GATE_CU1:
			CU1(QReg, p->control1, p->qubit, p->rambda);
			break;
		case GATE_CU3:
			CU3(QReg, p->control1, p->qubit, p->theta, p->phi, p->rambda);
			break;
		case GATE_MEASURE:
			double probZero = QReg->getQubitProb(p->qubit, 0);
			if(probZero >= 0.5) {
				MF(QReg, p->qubit, 0);
				if(opt_qasm) {
					fprintf(fp2, "\tMF(QReg, %d, 0);\n", p->qubit); 
				}
			} else {
				MF(QReg, p->qubit, 1);
				if(opt_qasm) {
					fprintf(fp2, "\tMF(QReg, %d, 1);\n", p->qubit); 
				}
			}
			break;
		}

		if(QReg->getNumStates() == 0) {
			break;
		}

		if((++count % 100) == 0) {
			printf("%d/%d done... (%d %%)\n", count, opt_runs, (count * 100) / opt_runs);
		}
	} 
}

void usage(void) 
{
	printf("usage: %s <options>\n", prog);
	printf("\n");
	printf("options(mandatory):\n");
	printf(" -q <number>   : number of qubits\n");
	printf("\n");
	printf("options(optional):\n");
	printf(" -r <number>   : number of gate runs (default: 100)\n");
	printf(" --qasm        : store QASM gate to file\n");
	printf(" --states      : display quantum states\n");
	printf(" --task        : display execution information\n");
}

int main(int argc, char **argv)
{
	int c;

	prog = argv[0];
	opt_qubits = 0;
	opt_runs = 100;

	static const struct option options[] = {
		{"qasm", 0, 0, '1'},
		{"states", 0, 0, '2'},
		{"task", 0, 0, '3'}
	};

	while ((c = getopt_long(argc, argv, "q:r:", options, NULL)) != -1) {
		switch(c) {
		case 'q':
			if(!optarg) usage();
			opt_qubits = atoi(optarg);
			break;		
		case 'r':
			if(!optarg) usage();
			opt_runs = atoi(optarg);
			break;		
		case '1':
			opt_qasm = true;
			break;
		case '2':
			opt_states = true;
			break;
		case '3':
			opt_task = true;
			break;
		default:
			usage();
			exit(0);
		}
	}

	if(opt_qubits == 0) {
		usage();
		exit(0);
	}

	if(opt_qubits > 30) {
		printf("We only support less than 30 qubits\n");
		exit(0);
	}

 	QReg = new QRegister(opt_qubits);

	if(opt_qasm) {
		fp1 = fopen("test.qasm", "wt");
		fp2 = fopen("test.cpp", "wt");

		fprintf(fp1, "OPENQASM 2.0;\n");
		fprintf(fp1, "include \"qelib1.inc\";\n");
		fprintf(fp1, "qreg q[%d]\n", opt_qubits);
		fprintf(fp1, "creg c[%d]\n", opt_qubits);

		fprintf(fp2, "#include \"qplayer.h\"\n\n");
		fprintf(fp2, "int main(int argc, char **argv)\n");
		fprintf(fp2, "{\n");
		fprintf(fp2, "\tQRegister *QReg = new QRegister(%d);\n\n", opt_qubits);
	}


	long_run_test();

	if(opt_states) {
		dump(QReg);
	}

	if(opt_task) {
		QReg->showQRegStat();
	}

	if(opt_qasm) {
	#if 0
		fprintf(fp2, "\n\tQReg->showQRegStat();\n");
	#else
		fprintf(fp2, "\n\tdump(QReg);\n");
	#endif
		fprintf(fp2, "}\n");

		fclose(fp1);
		fclose(fp2);
	}
}
