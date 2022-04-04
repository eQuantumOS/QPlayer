#include <iostream>
#include <cstring>
#include <bitset>
#include <vector>
#include <map>

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <getopt.h>

#include "qplayer.h"

using namespace std;

static QRegister *QReg = NULL; 
static char qasmf[256];

enum qasm_op {
	QASM_INITZ = 0,
	QASM_X,
	QASM_Z,
	QASM_H,
	QASM_S,
	QASM_T,
	QASM_SDG,
	QASM_TDG,
	QASM_CX,
	QASM_CCX,
	QASM_RZ,
	QASM_RX,
	QASM_RY,
	QASM_M
};

static double getAngle(char *token)
{
	char buf[32] = "";
	double angle = 0;
	int size=strlen(token);
	int pos = 0;
	int sign = 1;
	bool t_py = false;
	int i = 0;

	for(i=0; i<size; i++) {
		if(token[i] == ')') 
			break;

		if(token[i] == '-') {
			sign = -1;
			continue;
		}

		if(strncmp(token+i, "pi", 2) == 0) {
			t_py = true;
			i = i + 2;
		}

		if((token[i] >= 48 && token[i] <= 57) || token[i] == '.') {
			buf[pos++] = token[i];
		}
	} 

	if(t_py == true) {
		angle = Q_PI / atof(buf);
	} else {
		angle = atof(buf);
	}

	angle *= sign;

	return angle;
}

static int getQubit(char *token)
{
	char buf[8] = "";
	int size=strlen(token);
	int pos = 0;
	int i = 0;

	for(i=0; i<size; i++) {
		if(token[i] == ']') 
			break;

		if(token[i] < 48 || token[i] > 57) {
			continue;
		}
		
		buf[pos++] = token[i];
	} 

	return atoi(buf);
}

static int getQubitNum(void)
{
	int qubits = 0;

	FILE *fp = fopen(qasmf, "rt");
	if(fp == NULL) {
		printf("'%s' open failed\n", qasmf);
		exit(1);
	}

	while(!feof(fp)) {
		char line[256] = "";
		char tk1[32] = "";
		char tk2[32] = "";

		fgets(line, sizeof(line), fp);
		sscanf(line, "%s %s", tk1, tk2);

		if(strcmp(tk1, "qreg") == 0) {
			qubits = getQubit(tk2);
			break;
		}
	}

	fclose(fp);

	return qubits;
}

static void parse(char *line, int *op, int *q1, int *q2, int *q3, double *angle) 
{
	char tk1[32] = "";
	char tk2[32] = "";
	char tk3[32] = "";
	char tk4[32] = "";
	char tk5[32] = "";
	char tk6[32] = "";

	sscanf(line, "%s %s %s %s %s %s", tk1, tk2, tk3, tk4, tk5, tk6);

	*op = -1;

	if(strcmp(tk1, "h") == 0) {
		*op = QASM_H;
		*q1 = getQubit(tk2);
	} else if(strcmp(tk1, "x") == 0) {
		*op = QASM_X;
		*q1 = getQubit(tk2);
	} else if(strcmp(tk1, "z") == 0) {
		*op = QASM_Z;
		*q1 = getQubit(tk2);
	} else if(strcmp(tk1, "reset") == 0) {
		*op = QASM_INITZ;
		*q1 = getQubit(tk2);
	} else if(strcmp(tk1, "t") == 0) {
		*op = QASM_T;
		*q1 = getQubit(tk2);
	} else if(strcmp(tk1, "s") == 0) {
		*op = QASM_S;
		*q1 = getQubit(tk2);
	} else if(strcmp(tk1, "tdg") == 0) {
		*op = QASM_TDG;
		*q1 = getQubit(tk2);
	} else if(strcmp(tk1, "sdg") == 0) {
		*op = QASM_SDG;
		*q1 = getQubit(tk2);
	} else if(strncmp(tk1, "rx", 2) == 0) {
		*op = QASM_RX;
		*angle = getAngle(tk1);
	} else if(strncmp(tk1, "rz", 2) == 0) {
		*op = QASM_RX;
		*angle = getAngle(tk1);
		*q1 = getQubit(tk2);
	} else if(strcmp(tk1, "measure") == 0) {
		*op = QASM_M;
		*q1 = getQubit(tk2);
	} else if(strcmp(tk1, "cx") == 0) {
		*op = QASM_CX;
		*q1 = getQubit(tk2);
		*q2 = getQubit(tk3);
	} else if(strcmp(tk1, "ccx") == 0) {
		*op = QASM_CCX;
		*q1 = getQubit(tk2);
		*q2 = getQubit(tk3);
		*q3 = getQubit(tk4);
	} 
}

static void test(void)
{
	int op = 0;
	int q1 = 0;
	int q2 = 0;
	int q3 = 0;
	double angle = 0;

	FILE *fp = fopen(qasmf, "rt");
	if(fp == NULL) {
		printf("'%s' open failed\n", qasmf);
		exit(1);
	}

	while(!feof(fp)) {
		char line[256] = "";

		fgets(line, sizeof(line), fp);
		parse(line, &op, &q1, &q2, &q3, &angle);

		switch(op) {
		case QASM_INITZ:
			// printf("INITZ %d\n", q1);
			initZ(QReg, q1);
			break;
		case QASM_Z:
			// printf("Z %d\n", q1);
			Z(QReg, q1);
			break;
		case QASM_X:
			// printf("X %d\n", q1);
			X(QReg, q1);
			break;
		case QASM_H:
			// printf("H %d\n", q1);
			H(QReg, q1);
			break;
		case QASM_S:
			// printf("S %d\n", q1);
			S(QReg, q1);
			break;
		case QASM_T:
			// printf("T %d\n", q1);
			T(QReg, q1);
			break;
		case QASM_SDG:
			// printf("SDG %d\n", q1);
			SDG(QReg, q1);
			break;
		case QASM_TDG:
			// printf("TDG %d\n", q1);
			TDG(QReg, q1);
			break;
		case QASM_RX:
			// printf("RX %d %f\n", q1, angle);
			RX(QReg, q1, angle);
			break;
		case QASM_RZ:
			// printf("RZ %d %f\n", q1, angle);
			RZ(QReg, q1, angle);
			break;
		case QASM_M:
			// printf("M %d\n", q1);
			M(QReg, q1);
			break;
		case QASM_CX:
			// printf("CX %d %d\n", q1, q2);
			CX(QReg, q1, q2);
			break;
		case QASM_CCX:
			// printf("CCX %d %d %d\n", q1, q2, q3);
			CCX(QReg, q1, q2, q3);
			break;
		}
	}

	fclose(fp);
}

int main(int argc, char **argv)
{
	if(argc != 2) {
		printf("Usage: %s <qasm file>\n", argv[0]);
		exit(0);
	}

	strcpy(qasmf, argv[1]);

	QTimer timer;

	QReg = new QRegister(getQubitNum());

	timer.start();
	test();
	timer.end();

	QReg->dump();

	printf("Elapsed Time : %s\n", timer.getTime());
	QReg->checkMemory();
}
