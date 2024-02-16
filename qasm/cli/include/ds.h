#pragma once

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>

#define LOG_SIZE	256

enum StmtType {
	GATE,
	IF,
	MEASURE
};

enum GateType {
	G1,			// no angle, 1 qubit
	G2,			// no angle, 2 qubit
	G3,			// no angle, 3 qubit
	G4,			// 1 angle, 1 qubit
	G5,			// 2 angle, 1 qubit
	G6, 		// 3 angle, 1 qubit
	G7,			// 1 angle, 2 qubit
	G8,			// 2 angle, 2 qubit
	G9			// 3 angle, 2 qubit
};

enum Kind {
	none, 
	openqasm, 
	include, 
	qreg, 
	creg, 
	gate, 
	number, 
	rnumber, 
	str, 
	semicolon, 
	eol,
	comma, 
	plus, 
	minus, 
	multiply,
	divide, 
	lpar, 
	rpar, 
	lbrack, 
	rbrack, 
	lbrace, 
	rbrace, 
	PI, 
	SIN, 
	COS, 
	TAN, 
	EXP, 
	LOG, 
	SQRT, 
	POWER, 
	lt,
	lte,
	eq,
	gt, 
	gte, 
	arrow,			
	barrier, 
	ifstmt
};

inline static void logExit(const char *fmt, ...)
{
	char log[LOG_SIZE];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(log, LOG_SIZE, fmt, ap);
	va_end(ap);

	printf("%s\n", log);
	exit(0);
}

