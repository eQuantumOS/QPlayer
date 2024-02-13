#pragma once

#include <istream>
#include <sstream>
#include <string>

#include "Token.h"

class Scanner {
public:
	std::istream *in;
	std::vector<Token> tokens;
	int line;
	char ch;
	
public:
	Scanner(std::istream *in_stream) : in(in_stream) {
		line = 1;
	}

	~Scanner() {}

public:
	void readName(Token &t); 
	void readNumber(Token &t);
	void skipComment();
	void nextCh(void);
	void scan(void);
	void check_args(void);
	void build_ugates(std::map<std::string, Ugate> &ugates);
	void build_qregs(std::map<std::string, Reg_args> &qregs);
	void build_cregs(std::map<std::string, Reg_args> &cregs);
	void get_tokens(std::vector<Token> &tokens, int &pos);
	void get_utokens(std::vector<Token> source, std::vector<Token> &tokens, int &pos);
	void show(void);
};
