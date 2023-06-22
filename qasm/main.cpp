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

#include "QASMparser.h"

#define MAX_LENG 512

static char *prog;
static char *f_qasm = "/tmp/xyz";
static char f_in[MAX_LENG];
static char f_out[MAX_LENG];
static int shots;
static int verbose;
static char *version = "QPlayer v-1.0-Leopard";

static void usage(void) 
{
	printf("%s <options>\n", prog);
	printf("options:\n" );
	printf("  -f <string>      : input QASM file\n");
	printf("  -o <string>      : output result file(default: ./log/run.res)\n");
	printf("  -s <number>      : number of shots\n");
	printf("  -v               : QPlayer version\n");
	printf("  -h               : print help\n");
	printf("  --verbose        : print detailed simulation tasks\n");

	exit(0);
}

static void sig_handler(int sig)
{
	printf ("signal(%d) catched..\n", sig);
	exit(0);
}

void convertQASM(void) 
{
	FILE *in = NULL;
	FILE *out = NULL;

	in = fopen(f_in, "rt");
	out = fopen(f_qasm, "wt");

	while(!feof(in)) {
		char line[128] = "";

		fgets(line, sizeof(line), in);
		fprintf(out, "%s\n", line);
	}
	fprintf(out, "\n");

	fclose(in);
	fclose(out);
}

void runQASM(void)
{
	map<string, int> cregMap;
	QASMparser* parser = NULL;

	/* STEP1: execute QASM file for shot-round */
	for(int i=0; i<shots; i++) {
		parser = new QASMparser(f_qasm);
		vector<string> cregStr;

		parser->resetQReg();	
		parser->Parse();
		parser->get_cregStr(cregStr);

		for(auto entry : cregStr) {
			auto it = cregMap.find(entry);
			if(it == cregMap.end()) {
				cregMap[entry] = 1;
			} else {
				it->second++;
			}
		}
	}

	/* STEP2: generate output */

	/* make output directory */
	char *dirc = strdup(f_out);
	char *dname = dirname(dirc);
	char cmd[256] = "";
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
			fprintf(fp, "%d%%, %d/%d, |%s>\n", (int)(entry.second*100/shots), entry.second, shots, entry.first.c_str());
		}
	}

	fclose(fp);

#if 0
	if(!is_log) {
		printf("\033[1;32m*************************************************************\033[0;39m\n");
		printf("\033[1;32m             quantum states in quantum register              \033[0;39m\n");
		printf("\033[1;32m*************************************************************\033[0;39m\n");
		parser->dumpQReg();

		printf("\n");
		printf("\033[1;32m*************************************************************\033[0;39m\n");
		printf("\033[1;32m            measured states of classical register            \033[0;39m\n");
		printf("\033[1;32m*************************************************************\033[0;39m\n");
		printf("Total Shots: %d\n", shots);
		printf("Measured States: %ld\n", cregMap.size());
	
		if(cregMap.size() > 0) {
			for(auto entry : cregMap) {
				printf("%d%%, %d/%d, |%s>\n", (int)(entry.second*100/shots), entry.second, shots, entry.first.c_str());
			}
		}
	}
#endif

	if(verbose) {
		parser->dumpQRegStat();
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
	strcpy(f_out, "./log/run.res");
	shots = 1;	/* default number of shot */
	verbose = 0;

	static const struct option options[] = {
		{"verbose", 0, 0, '1'}
	};

	while ((c = getopt_long(argc, argv, "f:o:s:vh", options, NULL)) != -1) {
		switch(c) {
		case 'f':
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
		case 'v':
			printf("%s\n", version);
			exit(0);
			break;
		case '1':
			verbose = 1;
			break;
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
