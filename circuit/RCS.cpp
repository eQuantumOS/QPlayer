#include <iostream>
#include <cstring>
#include <bitset>
#include <vector>
#include <map>
#include <random>
#include <iostream>
#include <set>
#include <chrono>

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <getopt.h>

#include "register.h"
#include "gate.h"

using namespace std;

struct Gate {
	int qubit;
	int opcode;
	int target;
};

static QRegister *QReg = NULL;
static int qubits;
static int depth;
static int h_ratio;

static vector < vector < struct Gate >>circuits;

enum quantum_op {
	QOP_I=0,
	QOP_X,
	QOP_Z,
	QOP_CNOT,
	QOP_H
};

static void buildRandomCircuit(void)
{
	auto current = std::chrono::system_clock::now();
	auto duration = current.time_since_epoch();
	auto millis = std::chrono::duration_cast < std::chrono::milliseconds > (duration).count();

	std::mt19937_64 genMT(millis);
	std::uniform_int_distribution < int >uniformDist1(0, 2);
	std::uniform_int_distribution < int >uniformDist2(0, 3);

	circuits.clear();

	/* asign random gates */
	for (int i = 0; i < depth; i++) {
		vector < struct Gate >circuit;
		int cxNum = 0;
		int opcode = 0;

		for (int j = 0; j < qubits; j++) {
			struct Gate gate;

			while (1) {
				if(i == 0) {
					opcode = uniformDist1(genMT);
				} else {
					opcode = uniformDist2(genMT);
				}

				if (opcode == QOP_CNOT) {
					if (((cxNum + 1) * 2) > qubits) {
						continue;
					}
					cxNum++;
				}
				break;
			}

			gate.qubit = j;
			gate.opcode = opcode;
			gate.target = -1;
			circuit.push_back(gate);
		}

		/* assign hadamard depending on the ratio */
		if(i == 0) {
			int h_qubit = (qubits * h_ratio) / 100;
			for(int j=0; j<h_qubit; j++) {
				circuit[j].opcode = QOP_H;
			}
		}

		circuits.push_back(circuit);
	}

	/* reorganize gates for CNOT */
	vector < vector < struct Gate >>::iterator iter;
	for (iter = circuits.begin(); iter != circuits.end(); iter++) {
		vector < struct Gate >circuit = *iter;

		for (int i = 0; i < qubits; i++) {
			if (circuit[i].opcode == QOP_CNOT) {
				int target = -1;
				do {
					target = rand() % qubits;

					if (target == i)
						continue;
					else if (circuit[target].opcode == QOP_CNOT)
						continue;
					else if (circuit[target].opcode == -1)
						continue;
					else
						break;
				} while (1);
				circuit[i].target = target;
				circuit[target].opcode = -1;
			}
		}

		std::copy(circuit.begin(), circuit.end(), (*iter).begin());
	}
}

void printCircuits(void)
{
	if (circuits.size() == 0) {
		return;
	}

	for (int i = 0; i < depth; i++) {
		printf("--------");
	} printf("\n");
	for (int i = 0; i < depth; i++) {
		printf("D-%-6d", i + 1);
	} printf("\n");
	for (int i = 0; i < depth; i++) {
		printf("--------");
	} printf("\n");

	for (int i = 0; i < qubits; i++) {
		for (int j = 0; j < depth; j++) {
			struct Gate gate = circuits[j][i];
			if (gate.opcode == -1) {
				printf("-\t");
			} else if (gate.opcode == QOP_I) {
				printf("I\t");
			} else if (gate.opcode == QOP_X) {
				printf("X\t");
			} else if (gate.opcode == QOP_Z) {
				printf("Z\t");
			} else if (gate.opcode == QOP_H) {
				printf("H\t");
			} else if (gate.opcode == QOP_CNOT) {
				printf("CX(%d)\t", gate.target);
			}
		}
		printf("\n");
	}
	printf("\n");
}

void saveQASM(void)
{
	FILE *fp = NULL;
	char fname[64];

	sprintf(fname, "rcs-q%d-d%d-r%d.qasm", qubits, depth, h_ratio);
	
	fp = fopen(fname, "wt");

	fprintf(fp, "qreg q[%d]\n\n", qubits);
	for (int i = 0; i < depth; i++) {
		for (int j = 0; j < qubits; j++) {
			struct Gate gate = circuits[i][j];
			if (gate.opcode == -1) {
			} else if (gate.opcode == QOP_I) {
				fprintf(fp, "i q[%d]\n", gate.qubit);
			} else if (gate.opcode == QOP_X) {
				fprintf(fp, "x q[%d]\n", gate.qubit);
			} else if (gate.opcode == QOP_Z) {
				fprintf(fp, "z q[%d]\n", gate.qubit);
			} else if (gate.opcode == QOP_H) {
				fprintf(fp, "h q[%d]\n", gate.qubit);
			} else if (gate.opcode == QOP_CNOT) {
				fprintf(fp, "cx q[%d], q[%d]\n", gate.qubit, gate.target);
			}
		}

		fprintf(fp, "\n");
	}

	fclose(fp);
}

void test(void)
{
	for (int i = 0; i < depth; i++) {
		QTimer timer;
		timer.start();
		for (int j = 0; j < qubits; j++) {
			struct Gate gate = circuits[i][j];
			if (gate.opcode == -1) {
			} else if (gate.opcode == QOP_I) {
				I(QReg, gate.qubit);
			} else if (gate.opcode == QOP_X) {
				X(QReg, gate.qubit);
			} else if (gate.opcode == QOP_Z) {
				Z(QReg, gate.qubit);
			} else if (gate.opcode == QOP_H) {
				H(QReg, gate.qubit);
			} else if (gate.opcode == QOP_CNOT) {
				CX(QReg, gate.qubit, gate.target);
			}
		}
		timer.end();
		printf("%d/%d completes.. ", i+1, depth);
		checkMemory();
		printf("Elapsed Time : %s\n", timer.getTime());
	}
}

int main(int argc, char **argv)
{
	QTimer timer;

	if(argc != 4) {
		printf("%s <qubits> <depth> <ratio>\n", argv[0]);
		exit(0);
	}

	qubits = atoi(argv[1]);
	depth = atoi(argv[2]);
	h_ratio = atoi(argv[3]);

	QReg = new QRegister(qubits);

	buildRandomCircuit();
	printCircuits();

	timer.start();
#if 1
	test();
#else
	saveQASM();
#endif
	timer.end();

	printf("\nElapsed Time : %s\n", timer.getTime());
	checkMemory();
}
