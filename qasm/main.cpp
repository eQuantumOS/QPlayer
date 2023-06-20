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
static int is_log;
static int shots;
static int verbose;

static void usage(void) 
{
	printf("%s <options>\n", prog);
	printf("options:\n" );
	printf("  -f <input file>  : input QASM file\n");
	printf("  -o <output file> : output result file\n");
	printf("  -s <number>      : number of shots\n");
	printf("  -h\n");

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

	/* STEP1: execute QASM file for shot-round */
	for(int i=0; i<shots; i++) {
		QASMparser* parser = new QASMparser(f_qasm);
		vector<string> cregStr;

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

		if(verbose) {
			printf("************************************************\n");
			printf("Dump #1: show quantum states in quantum register\n");
			printf("************************************************\n");
			parser->dumpQReg();
		}

		parser->resetQReg();	

		delete parser;
	}

	/* STEP2: generate output */

	/* make output directory */
	if(is_log) {
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
	}

	if(verbose) {
		printf("\n");
		printf("***************************************************\n");
		printf("Dump #2: show measured states of classical register\n");
		printf("***************************************************\n");
		printf("Total Shots: %d\n", shots);
		printf("Measured States: %ld\n", cregMap.size());
	
		if(cregMap.size() > 0) {
			for(auto entry : cregMap) {
				printf("%d%%, %d/%d, |%s>\n", (int)(entry.second*100/shots), entry.second, shots, entry.first.c_str());
			}
		}
	}
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
	shots = 1;	/* default number of shot */
	is_log = 1;
	verbose = 0;

	while ((c = getopt_long(argc, argv, "f:o:s:vh", NULL, NULL)) != -1) {
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
			verbose = 1;
			break;
		case 'h':
			usage();
			break;
		}
	}

	if(strlen(f_in) == 0) {
		usage();
	}

	if(strlen(f_out) == 0) {
		verbose = 1;
		is_log = 0;
	}

	convertQASM();
	runQASM();
}
