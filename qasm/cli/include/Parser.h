#pragma once

#include <iostream>
#include <fstream>
#include <istream>
#include <sstream>
#include <string>
#include <map>

#include "Token.h"
#include "Scanner.h"

class Parser {
public:
	std::istream *in;	
	std::map<std::string, Reg_args> qregs;
	std::map<std::string, Reg_args> cregs;
	std::map<std::string, int> gates;
	std::map<std::string, Ugate> ugates;
	std::vector<STMT> stmts;
	std::vector<int> creg_measure;
	QRegister *QReg;
	int numQubits;
	int numCubits;

public:
	Scanner *scanner;

public:
	Parser(std::string fname) {
		in = new std::ifstream (fname, std::ifstream::in);
		if(!in->good()) {
			std::cerr << "ERROR opening file " << fname << std::endl;
			std::exit(1);
		}
		scanner = new Scanner(in);

		init_gates();
	}

	~Parser() {
		delete in;
		delete scanner;
	}

private:
	void init_gates(void);
	void check_brack(std::vector<Token> tokens, bool &is_brack);
	void get_qubit(Token tk, std::string qname, int val, std::vector<int> &qubits);
	void get_cubit(Token tk, std::string cname, int val, std::vector<int> &cubits);
	void get_qubits(Token tk, std::string qname, std::vector<int> &qubits);
	void get_cubits(Token tk, std::string cname, std::vector<int> &cubits);
	void get_cregvalue(Token tk, std::string cname, int &cuval);
	void check_gate_stmt(STMT stmt);

	void build_gate_stmt(std::vector<Token> tokens);
	void build_ugate_stmt(std::vector<Token> tokens);
	void build_if_stmt(std::vector<Token> tokens);
	void build_measure_stmt(std::vector<Token> tokens);

	void exec_gate(STMT stmt);

public:
	void parse(void);
	void run(void);
	void get_measure(std::string &str);
	void show_tokens(void);
	void show_stmt(void);
	void reset(void);
	struct qregister_stat getQRegStat(void);

public:
	/* for debugging */
	void dumpQReg(void);
}; 
