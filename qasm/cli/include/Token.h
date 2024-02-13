#pragma once

#include <iostream>
#include <climits>
#include <string>
#include <vector>
#include <map>

#include "qplayer.h"
#include "ds.h"

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
	int line;
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
