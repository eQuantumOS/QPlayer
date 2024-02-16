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
};

/***************************************************
  STMT class contains indivisual instructions to be
  executed in the simulator.
   - type : command type { GATE, IF, MEASURE }
   - line : line in the QASM code
   - gate : gate
   - args : angle arguments (if angle exists)
   - qubits : qubit arguments 
   - cubits : creg arguments (in case of MEASURE)
   - compare : compare condition (in case of IF)
 ***************************************************/
class STMT {
public:
	int type;
	int line;
    std::string gate;
    std::vector<double> args;
    std::vector<int> qubits;
    std::vector<int> cubits;
    std::vector<Token> compare;
};

/***************************************************
  Ugate class is used to manage user-defined gates
   - name : user gate name
   - args : user gate angle arguments
   - params : user gate qubit arguments
   - tokens : internal gate statements in this ugate

   < code example >
   gate mygate (r1, r2) a b {
        x a;
        u2 (r1, r2) b;
   }

   < data structures >
   name : mygate
   args : { r1, r2 }
   params : { a, b }
   tokens : { x a } - { u2 r1 r2 b }
   
 ***************************************************/
class Ugate {
public:
    std::string name;
    std::vector<std::string> args;
    std::vector<std::string> params;
    std::vector<Token> tokens;
};

/***************************************************
  It is used to handle qreg, creg lists. 'pos', 'size'
  means the start number and size, respectively.
  Assuming that we declared qreg, creg as in the below
  example, the values are as follows...

   < code example >
   qreg q1[3];   pos=0, size=3
   qreg q2[2];   pos=3, size=2
   creg c1[3];   pos=0, size=3
   creg c2[2];   pos=3, size=2
 ***************************************************/
class Reg_args {
public:
	int pos;
	int size;
};
