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
#include <fcntl.h>
#include <signal.h>
#include <libgen.h>

#include "include/Parser.h"

#define MAX_LENG 512

static char *prog;
static char *f_qasm = "/tmp/xyz";
static char f_in[MAX_LENG];
static char f_out[MAX_LENG];
static char j_out[MAX_LENG];
static int shots;
static int is_verbose;
static int is_json;
static char *version = "QPlayer v2.3-Cougar";
static char *dirc = NULL;
static char *dname = NULL;

static void usage(void) 
{
	printf("%s <options>\n", prog);
	printf("options:\n" );
	printf("  -i <string>      : input QASM file (mandatory)\n");
	printf("  -o <string>      : output result file (mandatory)\n");
	printf("  -j <string>      : output simulation statistics to json file\n");
	printf("  -s <number>      : number of shots\n");
	printf("  -h               : help\n");
	printf("  --version        : version\n");
	printf("  --verbose        : show simulation statistics\n");

	exit(0);
}

static void sig_handler(int sig)
{
	printf ("signal(%d) catched..\n", sig);
	exit(0);
}

void showStat(struct qregister_stat *stat)
{
	char memTotalStr[32] = "";
	char memAvailStr[32] = "";
	char memUsedStr[32] = "";
	char totalTimeStr[32] = "";

	human_readable_size(stat->memTotal, memTotalStr);
	human_readable_size(stat->memAvail, memAvailStr);
	human_readable_size(stat->memUsed, memUsedStr);
	usec2str(stat->tm_total, totalTimeStr);

	printf("\n");
	printf("\033[1;34m1. Circuit Information\033[0;39m\n");
	printf("+--------------------------+-----------------+\n");
	printf("| 1. used qubits           |  %7d        |\n", stat->qubits);
	printf("+--------------------------+-----------------+\n");
	printf("| 2. used gates            |  %7d        |\n", stat->usedGates);
	printf("+--------------------------+-----------------+\n");
	printf("| 3. total gate calls      |  %7d        |\n", stat->totalGateCalls);
	printf("+--------------------------+-----------------+\n");
	printf("| 4. individual gate calls |                 |\n");
	printf("+--------------------------+                 |\n");
	for(int i=0; i<MAX_GATES; i++) {
		if(stat->gateCalls[i] != 0) {
			printf("|              %10s  |  %7d (%2d %%) |\n", 
					gateString(i), stat->gateCalls[i], 
					(stat->gateCalls[i] * 100) / stat->totalGateCalls);
		}
	}
	printf("+--------------------------+-----------------+\n");

	printf("\n");
	printf("\033[1;34m2. Runtime (micro seconds)\033[0;39m\n");
	printf("+-----------------------------+---------------------------------------------+\n");
	printf("| 1. total simulation time    |   %11.f %29s |\n", stat->tm_total, totalTimeStr);
	printf("+-----------------------------+---------------+-----------+-------+---------+\n");
	printf("| 2. each gate execution time |     total     |    max    |  min  |   avg   |\n");
	printf("+-----------------------------+---------------+-----------+-------+---------+\n");
	for(int i=0; i<MAX_GATES; i++) {
		if(stat->gateCalls[i] != 0) {
			printf("| %27s | %13.0f | %9.0f | %5.0f | %7.0f |\n", gateString(i),
					stat->tm_gates_total[i],
					stat->tm_gates_max[i],
					stat->tm_gates_min[i],
					stat->tm_gates_avg[i]);
			}
	}
	printf("+-----------------------------+---------------+-----------+-------+---------+\n");

	printf("\n");
	printf("\033[1;34m3. Simulation Jobs\033[0;39m\n");
	printf("+-----------------------------------+---------------+\n");
	printf("| 1. max number of quantum states   | %13ld |\n", (uint64_t)stat->maxQStates);
	printf("+-----------------------------------+---------------+\n");
	printf("| 2. final number of quantum states | %13ld |\n", (uint64_t)stat->finalQStates);
	printf("+-----------------------------------+---------------+\n");
	printf("| 3. used memory                    | %13s |\n", memUsedStr);
	printf("+-----------------------------------+---------------+\n");

	printf("\n");
	printf("\033[1;34m4. System Information\033[0;39m\n");
	printf("+-----------+---------+-------------------------------------------------+\n");
	printf("|           | name    | %47s | \n", stat->os_name);
	printf("|    OS     |---------+-------------------------------------------------+\n");
	printf("|           | version | %47s | \n", stat->os_version);
	printf("+-----------+---------+-------------------------------------------------+\n");
	printf("|           | model   | %47s | \n", stat->cpu_model);
	printf("|           |---------+-------------------------------------------------+\n");
	printf("|    CPU    | cores   | %47d | \n", stat->cpu_cores);
	printf("|           |---------+-------------------------------------------------+\n");
	printf("|           | herz    | %47s | \n", stat->cpu_herz);
	printf("+-----------+---------+-------------------------------------------------+\n");
	printf("|           | total   | %47s | \n", memTotalStr);
	printf("|    MEM    |---------+-------------------------------------------------+\n");
	printf("|           | avail   | %47s | \n", memAvailStr);
	printf("+-----------+---------+-------------------------------------------------+\n");
}

void genStatJson(struct qregister_stat *stat)
{
	char cmd[256] = "";

	dirc = strdup(j_out);
	dname = dirname(dirc);
	sprintf(cmd, "mkdir -p %s", dname);
	system(cmd);

	FILE *fp = fopen(j_out, "wt");

	char memTotalStr[32] = "";
	char memAvailStr[32] = "";
	char memUsedStr[32] = "";
	int pos = 0;

	human_readable_size(stat->memTotal, memTotalStr);
	human_readable_size(stat->memAvail, memAvailStr);
	human_readable_size(stat->memUsed, memUsedStr);

	fprintf(fp, "{\n");
	fprintf(fp, " \"circuit\" : {\n");
	fprintf(fp, "  \"used qubits\" : %d,\n", stat->qubits);
	fprintf(fp, "  \"used gates\" : %d,\n", stat->usedGates);
	fprintf(fp, "  \"gate calls\" : {\n");
	pos = 0;
	for(int i=0; i<MAX_GATES; i++) {
		if(stat->gateCalls[i] != 0) {
			if(++pos < stat->usedGates) {
				fprintf(fp, "   \"%s\" : %d,\n", gateString(i), stat->gateCalls[i]);
			} else {
				fprintf(fp, "   \"%s\" : %d\n", gateString(i), stat->gateCalls[i]);
			}
		}
	}
	fprintf(fp, "  }\n");
	fprintf(fp, " },\n");

	fprintf(fp, " \"runtime\" : {\n");
	fprintf(fp, "  \"total simulation time\" : %.f,\n", stat->tm_total);
	fprintf(fp, "  \"individual gate time\" : {\n");
	pos = 0;
	for(int i=0; i<MAX_GATES; i++) {
		if(stat->gateCalls[i] != 0) {
			if(++pos < stat->usedGates) {
				fprintf(fp, "   \"%s\" : [%.f, %.f, %.f, %.f],\n", gateString(i), 
							stat->tm_gates_total[i],
							stat->tm_gates_max[i],
							stat->tm_gates_min[i],
							stat->tm_gates_avg[i]);
			} else {
				fprintf(fp, "   \"%s\" : [%.f, %.f, %.f, %.f]\n", gateString(i), 
							stat->tm_gates_total[i],
							stat->tm_gates_max[i],
							stat->tm_gates_min[i],
							stat->tm_gates_avg[i]);
			}
		}
	}
	fprintf(fp, "  }\n");
	fprintf(fp, " },\n");

	fprintf(fp, " \"simulation jobs\" : {\n");
	fprintf(fp, "  \"max states\" : %ld,\n", (uint64_t)stat->maxQStates);
	fprintf(fp, "  \"final states\" : %ld,\n", (uint64_t)stat->finalQStates);
	fprintf(fp, "  \"used memory\" : \"%s\"\n", memUsedStr);
	fprintf(fp, " },\n");

	fprintf(fp, " \"system\" : {\n");
	fprintf(fp, "  \"OS\" : {\n");
	fprintf(fp, "   \"name\" : \"%s\",\n", stat->os_name);
	fprintf(fp, "   \"version\" : \"%s\"\n", stat->os_version);
	fprintf(fp, "  },\n");
	fprintf(fp, "  \"CPU\" : {\n");
	fprintf(fp, "   \"model\" : \"%s\",\n", stat->cpu_model);
	fprintf(fp, "   \"cores\" : %d,\n", stat->cpu_cores);
	fprintf(fp, "   \"herz\" : \"%s\"\n", stat->cpu_herz);
	fprintf(fp, "  },\n");
	fprintf(fp, "  \"Memory\" : {\n");
	fprintf(fp, "   \"total\" : \"%s\",\n", memTotalStr);
	fprintf(fp, "   \"avail\" : \"%s\"\n", memAvailStr);
	fprintf(fp, "  }\n");
	fprintf(fp, " }\n");

	fprintf(fp, "}\n");

	fclose(fp);
}

void convertQASM(void) 
{
	FILE *in = NULL;
	FILE *out = NULL;

	in = fopen(f_in, "rt");
	out = fopen(f_qasm, "wt");

	while(!feof(in)) {
		char line[1024] = "";

		fgets(line, sizeof(line), in);
		fprintf(out, "%s", line);
	}
	fprintf(out, "\n\n");

	fclose(in);
	fclose(out);
}

void runQASM(void)
{
	map<std::string, int> cregMap;

	/*********************************************/
	/* STEP1: parsing qasm codes                 */
	/*********************************************/
	Parser *parser = new Parser(f_qasm);
	parser->parse();

	/*********************************************/
	/* STEP2: run qasm codes for nshots          */
	/*********************************************/
	for(int i=0; i<shots; i++) {
		std::string str;

		parser->run();
		parser->get_measure(str);

		auto it = cregMap.find(str);
		if(it == cregMap.end()) {
			cregMap[str] = 1;
		} else {
			it->second++;
		}

		parser->reset();
	};

	/*********************************************/
	/* STEP3: generate measured output           */
	/*********************************************/
	char cmd[256]= "";
	dirc = strdup(f_out);
	dname = dirname(dirc);
	sprintf(cmd, "mkdir -p %s", dname);
	system(cmd);
	
	FILE *fp = fopen(f_out, "wt");
	if(fp == NULL) {
		printf("error: cannot open '%s'\n", f_out);
	}
	
	fprintf(fp, "Total Shots: %d\n", shots);
	fprintf(fp, "Measured States: %ld\n", cregMap.size());
	
	if(cregMap.size() > 0) {
		fprintf(fp, "\n");
		for(auto entry : cregMap) {
			fprintf(fp, "%d%%, %d/%d, |%s>\n", 
				(int)(entry.second*100/shots), 
				entry.second, shots, entry.first.c_str());
		}
	}

	fclose(fp);

	/*********************************************/
	/* STEP4: generate simulation stat to json   */
	/*********************************************/
	struct qregister_stat stat = parser->getQRegStat();

	if(is_verbose) {
		showStat(&stat);
	}

	if(is_json) {
		genStatJson(&stat);
	}

	delete parser;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int c;

	prog = argv[0];

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	sigaction(SIGABRT, &sa, NULL);

	strcpy(f_in, "");
	strcpy(f_out, "");
	strcpy(j_out, "");
	shots = 1;	/* default number of shot */
	is_verbose = 0;
	is_json = 0;

	static const struct option options[] = {
		{"verbose", 0, 0, '1'},
		{"version", 0, 0, '2'}
	};

	while ((c = getopt_long(argc, argv, "i:o:s:j:h", options, NULL)) != -1) {
		switch(c) {
		case 'i':
			if(!optarg) 
				usage();
			strcpy(f_in, optarg);
			break;
		case 'o':
			if(!optarg) 
				usage();
			strcpy(f_out, optarg);
			break;
		case 's':
			if(!optarg) 
				usage();
			shots = atoi(optarg);
			break;
		case 'j':
			if(!optarg) 
				usage();
			strcpy(j_out, optarg);
			is_json = 1;
			break;
		case '1':
			is_verbose = 1;
			break;
		case '2':
			printf("%s\n", version);
			exit(0);
			break;
		default:
		case 'h':
			usage();
			break;
		}
	}

	if(strlen(f_in) == 0 || strlen(f_out) == 0) {
		usage();
	}

	convertQASM();
	runQASM();
}
