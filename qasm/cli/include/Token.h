#pragma once

#include <iostream>
#include <climits>
#include <string>
#include <vector>
#include <map>

using namespace std;

enum StmtType {
	GATE,
	IF,
	MEASURE
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

class Token {
public:
	Kind kind;
	int line;
	int val;
	double rval;
	std::string str;

public:
	Token(Kind k, int l) { 	
		kind = k;
		line = l;
		val = INT_MAX;
		rval = LONG_MAX; 
		str = "";
	}
	~Token() {}

	char *token2str(Kind kind);
	void show(void);
	void show_human(void);
};

class STMT {
public:
	int type;
    std::vector<Token> compare;
    std::string gate;
    std::vector<double> args;
    std::vector<int> qubits;
    std::vector<int> cubits;
};

class Ugate {
public:
    std::string name;
    std::vector<std::string> args;
    std::vector<std::string> params;
    std::vector<Token> tokens;
};

class Reg_args {
public:
	int min;
	int max;
};
