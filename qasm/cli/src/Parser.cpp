#include "Parser.h"
#include "Cal.h"

/***************************************************************
  A. execution flow

     1) Parser::parse()
        - tokinzing qasm codes
        - build qreg, creg list
        - build user gates map
        - build gate statements for calling simulator

     2) Parser::run()  
        - call gate command in the STMT list in order

  B. QASM code & Memory example

     1) sample code

        OPENQASM 2.0;
        include "qelib1.inc";

        qreg q[4];
        creg c[4];

        gate mygate (r1) a b {
            h a;
            u1 (r1) b;
        }

        x q[0];
        cx q[0], q[1];
        mygate (pi) q[0], q[2];

        measure q -> c;

        if (c == 0) x q[1];

     2) decomposed tokens (Scanner::vector<Token> Tokens)

        OPENQASM 2.00 ;
        include qelib1.inc ;
        qreg q [ 4 ] ;
        creg c [ 4 ] ;
        gate mygate ( r1 ) a b {
        h a ;
        u1 ( r1 ) b ;
        }
        x q [ 0 ] ;
        cx q [ 0 ] , q [ 1 ] ;
        mygate ( pi ) q [ 0 ] , q [ 2 ] ;
        measure q -> c ;
        if ( c == 0 ) x q [ 1 ] ;

     3) statement lists (Parser::vector<STMT> stmts)

        stmt[0] : x 0 
        stmt[1] : cx 0 1 
        stmt[2] : h 0 
        stmt[3] : u1 3.14 2 
        stmt[4] : measure 0 1 2 3  -> 0 1 2 3 
        stmt[5] : if c == 0 x 1 
 ***************************************************************/

void Parser::parse(void) 
{
	std::vector<Token> tokens;
	int pos = 0;

	/*******************************************/
	/* step1: toknizing all QASM codes         */
	/*******************************************/
	scanner->scan();
	
	/*******************************************/
	/* step2: build qreg, creg list            */
	/*******************************************/
	numQubits = 0;
	numCubits = 0;
	scanner->build_qregs(qregs);
	scanner->build_cregs(cregs);
	for(auto q : qregs) numQubits += q.second.size;
	for(auto c : cregs) numCubits += c.second.size;
	for(int i=0; i<numCubits; i++) creg_measure.push_back(0);

	if(numQubits == 0) {
		logExit("[%s:%d] qreg is not defined", _F_, _L_);
	}

#if 0
	if(numCubits == 0) {
		logExit("[%s:%d] creg is not defined", _F_, _L_);
	}
#endif

	/*******************************************/
	/* step3: build user defined gates         */
	/*******************************************/
	scanner->build_ugates(ugates);

	/*******************************************/
	/* step4: build stmts                      */
	/*******************************************/
	while(1) {
		scanner->get_tokens(tokens, pos);
		if(tokens.size() == 0) {
			break;
		}

		if(tokens[0].kind != Kind::str) {
			logExit("[%s:%d] does not start with invalid command in line %d", _F_, _L_, tokens[0].line);
		}

		if(gates.find(tokens[0].str) != gates.end()) {
			build_gate_stmt(tokens);
		} else if(ugates.find(tokens[0].str) != ugates.end()) {
			build_ugate_stmt(tokens);
		} else if(tokens[0].str == "if") {
			build_if_stmt(tokens);
		} else if(tokens[0].str == "measure") {
			build_measure_stmt(tokens);
		} else if(tokens[0].str == "barrier") {
			continue;
		} else {
			logExit("[%s:%d] does not start with invalid command in line %d", _F_, _L_, tokens[0].line);
		}
	}

	/*******************************************/
	/* step5: allocate qregister               */
	/*******************************************/
	QReg = new QRegister(numQubits);
}

void Parser::run(void) 
{
	for(auto stmt : stmts) {
		if(stmt.type == StmtType::MEASURE) {
			for(int i=0; i<stmt.qubits.size(); i++) {
				creg_measure[stmt.cubits[i]] = M(QReg, stmt.qubits[i]);
			}
		} else if(stmt.type == StmtType::IF) {
			std::string lparam = stmt.compare[0].str;
			Kind kind = stmt.compare[1].kind;
			int rparam = stmt.compare[2].val;
			int cuval = 0;
			bool pass = false;

			get_cregvalue(stmt.compare[0], lparam, cuval);

			if(kind == Kind::eq) {
				if(cuval == rparam) pass = true;
			} else if(kind == Kind::lt) {
				if(cuval < rparam) pass = true;
			} else if(kind == Kind::lte) {
				if(cuval <= rparam) pass = true;
			} else if(kind == Kind::gt) {
				if(cuval > rparam) pass = true;
			} else if(kind == Kind::gte) {
				if(cuval >= rparam) pass = true;
			}

			if(pass == false) {
				continue;
			}

			/* convert stmt type & run */
			stmt.type = StmtType::GATE;
			exec_gate(stmt);
		} else {
			exec_gate(stmt);
		}
	}
}

void Parser::build_gate_stmt(std::vector<Token> tokens) 
{
	STMT stmt;
	bool is_brack = false;
	int pos = 0;

	/***********************************/
	/* step1: parsing gate name        */
	/***********************************/
	stmt.gate = tokens[pos++].str;
	stmt.type = StmtType::GATE;
	stmt.line = tokens[0].line;
	check_brack(tokens, is_brack);

	/***********************************/
	/* step2: parsing angle argument   */
	/***********************************/
	if(tokens[pos].kind == Kind::lpar) {
		std::string arg;
		int par_cnt = 1;
		double rval = 0;

		while(par_cnt != 0) {
			pos++;
			if(tokens[pos].kind == Kind::lpar) {
				par_cnt++;
				arg += tokens[pos].str;
			} else if(tokens[pos].kind == Kind::rpar) {
				par_cnt--;
				if(par_cnt == 0) {
					rval = calArg(arg);
					stmt.args.push_back(rval);
					arg.clear();
				} else {
					arg += tokens[pos].str;
				}
			} else if(tokens[pos].kind == Kind::number) {	
				arg += to_string(tokens[pos].val);
			} else if(tokens[pos].kind == Kind::rnumber) {	
				arg += to_string(tokens[pos].rval);
			} else if(tokens[pos].kind == Kind::comma) {	
				rval = calArg(arg);
				stmt.args.push_back(rval);
				arg.clear();
			} else {
				arg += tokens[pos].str;
			}
		}
	}

	/***********************************/
	/* step3: parsing qubit argument   */
	/***********************************/
	if(is_brack == false) {
		get_qubits(tokens[pos], tokens[pos].str, stmt.qubits);
	} else {
		while(tokens[pos].kind != Kind::semicolon) {
			if(tokens[pos].kind == Kind::lbrack) {	
				if(tokens[pos+1].kind == Kind::rbrack) {	
					logExit("[%s:%d] qubit is empty in line %d", _F_, _L_, tokens[0].line);
				} else if(tokens[pos+1].kind != Kind::number) {	
					logExit("[%s:%d] qubit is not number type in line %d", _F_, _L_, tokens[0].line);
				} 
				get_qubit(tokens[pos], tokens[pos-1].str, tokens[pos+1].val, stmt.qubits);
			}
			pos++;
		}
	} 

	stmts.push_back(stmt);
}

void Parser::build_ugate_stmt(std::vector<Token> tokens) 
{
	std::vector<double> arg_params;
	std::vector<int> qubit_params;
	std::string uname;
	std::string gname;
	int pos = 0;
	Ugate ugate;

	/***********************************/
	/* step1: check & get user gate    */
	/***********************************/
	uname = tokens[pos++].str;

	auto it = ugates.find(uname);
	if(it == ugates.end()) {
		logExit("[%s:%d] invalid user gate(%s) in line %d", _F_, _L_, uname.c_str(), tokens[0].line);
	} else { 
		ugate = it->second;
	}

	/***********************************/
	/* step2: parsing angle argument   */
	/***********************************/
	if(tokens[pos].kind == Kind::lpar) {
		std::string arg;
		int par_cnt = 1;
		double rval = 0;

		while(par_cnt != 0) {
			pos++;
			if(tokens[pos].kind == Kind::lpar) {
				par_cnt++;
				arg += tokens[pos].str;
			} else if(tokens[pos].kind == Kind::rpar) {
				par_cnt--;
				if(par_cnt == 0) {
					rval = calArg(arg);
					arg_params.push_back(rval);
					arg.clear();
				} else {
					arg += tokens[pos].str;
				}
			} else if(tokens[pos].kind == Kind::number) {	
				arg += to_string(tokens[pos].val);
			} else if(tokens[pos].kind == Kind::rnumber) {	
				arg += to_string(tokens[pos].rval);
			} else if(tokens[pos].kind == Kind::comma) {	
				rval = calArg(arg);
				arg_params.push_back(rval);
				arg.clear();
			} else {
				arg += tokens[pos].str;
			}
		}
	}

	/***********************************/
	/* step3: get parameters           */
	/***********************************/
	while(tokens[pos].kind != Kind::semicolon) {
		if(tokens[pos].kind == Kind::lbrack) {	
			get_qubit(tokens[pos], tokens[pos-1].str, tokens[pos+1].val, qubit_params);
		}
		pos++;
	}

	/***********************************/
	/* step4: build stmts in user gate */
	/***********************************/
	pos = 0;
	while(true) {
		std::vector<Token> utokens;
		STMT stmt;
		int upos = 0;

		scanner->get_utokens(ugate.tokens, utokens, pos);
		if(utokens.size() == 0) {
			break;
		}

		/***********************************/
		/* step1: parsing gate name        */
		/***********************************/
		stmt.gate = utokens[upos++].str;
		stmt.line = utokens[0].line;
		stmt.type = StmtType::GATE;

		/***********************************/
		/* step2: parsing angle argument   */
		/***********************************/
		if(utokens[upos].kind == Kind::lpar) {
			upos++;
			while(true) {
				bool arg_found = false;
				int aidx = 0;

				if(utokens[upos].kind == Kind::comma) {
					upos++;
					continue;
				}

				for(int i=0; i<ugate.args.size(); i++) {
					if(utokens[upos].str == ugate.args[i]) {
						arg_found = true;
						aidx = i;
						break;
					}
				}
	
				if(arg_found == false) {
					logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), tokens[0].line);
				} 

				stmt.args.push_back(arg_params[aidx]);

				if(utokens[++upos].kind == Kind::rpar) {
					upos++;
					break;
				}
			}
		}

		/***********************************/
		/* step3: parsing qubit argument   */
		/***********************************/
		while(utokens[upos].kind != Kind::semicolon) {
			bool param_found = false;
			int qidx = 0;

			if(utokens[upos].kind == Kind::comma) {
				upos++;
				continue;
			}

			for(int i=0; i<ugate.params.size(); i++) {
				if(utokens[upos].str == ugate.params[i]) {
					param_found = true;
					qidx = i;
					break;
				}
			}

			if(param_found == false) {
				logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), tokens[0].line);
			}

			stmt.qubits.push_back(qubit_params[qidx]);

			upos++;
		}
	
		stmts.push_back(stmt);
	}
}

void Parser::build_measure_stmt(std::vector<Token> tokens) 
{
	STMT stmt;
	bool is_brack = false;
	int pos = 0;

	/***********************************/
	/* step1: parsing gate name        */
	/***********************************/
	stmt.gate = tokens[pos++].str;
	stmt.line = tokens[0].line;
	stmt.type = StmtType::MEASURE;
	check_brack(tokens, is_brack);

	/***********************************/
	/* step2: parsing left qregister   */
	/***********************************/
	if(is_brack == false) {
		get_qubits(tokens[pos], tokens[pos].str, stmt.qubits);
		while(tokens[pos++].kind != Kind::arrow) {}
	} else {
		while(tokens[pos].kind != Kind::arrow) {
			if(tokens[pos].kind == Kind::lbrack) {	
				get_qubit(tokens[pos], tokens[pos-1].str, tokens[pos+1].val, stmt.qubits);
			}
			pos++;
		}
	}

	/***********************************/
	/* step3: parsing right cregister  */
	/***********************************/
	if(is_brack == false) {
		get_cubits(tokens[pos], tokens[pos].str, stmt.cubits);
	} else {
		while(tokens[pos].kind != Kind::semicolon) {
			if(tokens[pos].kind == Kind::lbrack) {	
				get_cubit(tokens[pos], tokens[pos-1].str, tokens[pos+1].val, stmt.cubits);
			}
			pos++;
		}
	}

	if(stmt.qubits.size() != stmt.cubits.size()) {
		logExit("[%s:%d] qubit size mismatch in line %d", _F_, _L_, tokens[0].line);
	}

	stmts.push_back(stmt);
}

void Parser::build_if_stmt(std::vector<Token> tokens) 
{
	STMT stmt;
	bool is_brack = false;
	int pos = 0;

	/***********************************/
	/* step1: parsing gate name        */
	/***********************************/
	stmt.type = StmtType::IF;
	stmt.line = tokens[0].line;
	check_brack(tokens, is_brack);

	/***********************************/
	/* step2: parsing compare argument */
	/***********************************/
	while(tokens[pos++].kind != Kind::lpar) {}
	while(tokens[pos].kind != Kind::rpar) {
		stmt.compare.push_back(tokens[pos]);
		pos++;
	}

	stmt.gate = tokens[++pos].str;

	/***********************************/
	/* step3: parsing angle argument   */
	/***********************************/
	if(tokens[pos+1].kind == Kind::lpar) {
		std::string arg;
		int par_cnt = 1;
		double rval = 0;

		pos++;
		while(par_cnt != 0) {
			pos++;
			if(tokens[pos].kind == Kind::lpar) {
				par_cnt++;
				arg += tokens[pos].str;
			} else if(tokens[pos].kind == Kind::rpar) {
				par_cnt--;
				if(par_cnt == 0) {
					rval = calArg(arg);
					stmt.args.push_back(rval);
					arg.clear();
				} else {
					arg += tokens[pos].str;
				}
			} else if(tokens[pos].kind == Kind::number) {	
				arg += to_string(tokens[pos].val);
			} else if(tokens[pos].kind == Kind::rnumber) {	
				arg += to_string(tokens[pos].rval);
			} else if(tokens[pos].kind == Kind::comma) {	
				rval = calArg(arg);
				stmt.args.push_back(rval);
				arg.clear();
			} else {
				arg += tokens[pos].str;
			}
		}
	}

	/***********************************/
	/* step4: parsing qubit argument   */
	/***********************************/
	if(is_brack == false) {
		pos++;
		get_qubits(tokens[pos], tokens[pos].str, stmt.qubits);
	} else {
		while(tokens[pos].kind != Kind::semicolon) {
			if(tokens[pos].kind == Kind::lbrack) {	
				get_qubit(tokens[pos], tokens[pos-1].str, tokens[pos+1].val, stmt.qubits);
			}
			pos++;
		}
	} 

	stmts.push_back(stmt);
}

void Parser::exec_gate(STMT stmt)
{
	check_gate_stmt(stmt);

	if(stmt.gate == "reset") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			initZ(QReg, stmt.qubits[i]);
		}
	} else if(stmt.gate == "id") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			continue;
		}
	} else if(stmt.gate == "x") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			X(QReg, stmt.qubits[i]);
		}
	} else if(stmt.gate == "y") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			Y(QReg, stmt.qubits[i]);
		}
	} else if(stmt.gate == "z") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			Z(QReg, stmt.qubits[i]);
		}
	} else if(stmt.gate == "h") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			H(QReg, stmt.qubits[i]);
		}
	} else if(stmt.gate == "t") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			T(QReg, stmt.qubits[i]);
		}
	} else if(stmt.gate == "tdg") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			TDG(QReg, stmt.qubits[i]);
		}
	} else if(stmt.gate == "s") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			S(QReg, stmt.qubits[i]);
		}
	} else if(stmt.gate == "sdg") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			SDG(QReg, stmt.qubits[i]);
		}
	} else if(stmt.gate == "sx") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			SX(QReg, stmt.qubits[i]);
		}
	} else if(stmt.gate == "sxdg") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			SXDG(QReg, stmt.qubits[i]);
		}
	} else if(stmt.gate == "u1") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			U1(QReg, stmt.qubits[i], stmt.args[0]);
		}
	} else if(stmt.gate == "u2") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			U2(QReg, stmt.qubits[i], stmt.args[0], stmt.args[1]);
		}
	} else if(stmt.gate == "u3") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			U3(QReg, stmt.qubits[i], stmt.args[0], stmt.args[1], stmt.args[3]);
		}
	} else if(stmt.gate == "rx") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			RX(QReg, stmt.qubits[i], stmt.args[0]);
		}
	} else if(stmt.gate == "ry") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			RY(QReg, stmt.qubits[i], stmt.args[0]);
		}
	} else if(stmt.gate == "rz") {
		for(int i=0; i<stmt.qubits.size(); i++) {
			RZ(QReg, stmt.qubits[i], stmt.args[0]);
		}
	} else if(stmt.gate == "cx") {
		CX(QReg, stmt.qubits[0], stmt.qubits[1]);
	} else if(stmt.gate == "ch") {
		CH(QReg, stmt.qubits[0], stmt.qubits[1]);
	} else if(stmt.gate == "cy") {
		CY(QReg, stmt.qubits[0], stmt.qubits[1]);
	} else if(stmt.gate == "cz") {
		CZ(QReg, stmt.qubits[0], stmt.qubits[1]);
	} else if(stmt.gate == "crz") {
		CRZ(QReg, stmt.qubits[0], stmt.qubits[1], stmt.args[0]);
	} else if(stmt.gate == "cu1") {
		CU1(QReg, stmt.qubits[0], stmt.qubits[1], stmt.args[0]);
	} else if(stmt.gate == "cu2") {
		CU2(QReg, stmt.qubits[0], stmt.qubits[1], stmt.args[0], stmt.args[1]);
	} else if(stmt.gate == "cu3") {
		CU3(QReg, stmt.qubits[0], stmt.qubits[1], stmt.args[0], stmt.args[1], stmt.args[2]);
	} else if(stmt.gate == "swap") {
		SWAP(QReg, stmt.qubits[0], stmt.qubits[1]);
	} else if(stmt.gate == "ccx") {
		CCX(QReg, stmt.qubits[0], stmt.qubits[1], stmt.qubits[2]);
	} else if(stmt.gate == "cwap") {
		CSWAP(QReg, stmt.qubits[0], stmt.qubits[1], stmt.qubits[2]);
	} else if(stmt.gate == "iswap") {
		iSWAP(QReg, stmt.qubits[0], stmt.qubits[1]);
	}
}

void Parser::check_brack(std::vector<Token> tokens, bool &is_brack)
{
	is_brack = false;

	for(int i=0; i<tokens.size(); i++) {
		if(tokens[i].kind == Kind::rbrack) {
			is_brack = true;
			break;
		}
	}
}

void Parser::get_qubit(Token tk, std::string qname, int val, std::vector<int> &qubits) 
{
	auto it = qregs.find(qname);
	if(it == qregs.end()) {
		logExit("[%s:%d] invalid qreg in line %d", _F_, _L_, tk.line);
	} else {
		int qubit = it->second.pos + val;
		if(qubit <= numQubits) {
			qubits.push_back(it->second.pos + val);
		} else {
			logExit("[%s:%d] qubit is out-of-range in line %d", _F_, _L_, tk.line);
		}
	}
}

void Parser::get_qubits(Token tk, std::string qname, std::vector<int> &qubits) 
{
	auto it = qregs.find(qname);
	if(it == qregs.end()) {
		logExit("[%s:%d] invalid qreg in line %d", _F_, _L_, tk.line);
	} else {
		for(int i=0; i<it->second.size; i++) {
			qubits.push_back(it->second.pos + i);
		}
	}
}

void Parser::get_cubit(Token tk, std::string cname, int val, std::vector<int> &cubits) 
{
	auto it = cregs.find(cname);
	if(it == cregs.end()) {
		logExit("[%s:%d] invalid creg line %d", _F_, _L_, tk.line);
	} else {
		int cubit = it->second.pos + val;
		if(cubit <= numCubits) {
			cubits.push_back(it->second.pos + val);
		} else {
			logExit("[%s:%d] creg is out-of-range in line %d", _F_, _L_, tk.line);
		}
	}
}

void Parser::get_cubits(Token tk, std::string cname, std::vector<int> &cubits) 
{
	auto it = cregs.find(cname);
	if(it == cregs.end()) {
		logExit("[%s:%d] invalid creg line %d", _F_, _L_, tk.line);
	} else {
		for(int i=0; i<it->second.size; i++) {
			cubits.push_back(it->second.pos + i);
		}
	}
}

void Parser::get_cregvalue(Token tk, std::string cname, int &cuval)
{
	Reg_args creg;

	auto it = cregs.find(cname);
	if(it == cregs.end()) {
		logExit("[%s:%d] invalid creg line %d", _F_, _L_, tk.line);
	} else { 
		creg = it->second;
	}

	cuval = 0;
	for(int i=creg.pos; i<(creg.pos+creg.size); i++) {
		if(creg_measure[i] != 0) {
			cuval |= (1 << i);
		}
	}
}

void Parser::get_measure(std::string &str)
{
	char buf[1024] = "";
	int val = 0;

	str.clear();
	for(int i=0; i<creg_measure.size(); i++) {
		if(creg_measure[i] != 0) {
			val |= (1 << i);
		}
	}

	to_binary(val, numQubits, buf);
	str = std::string(buf);
}

void Parser::show_tokens(void) 
{
	printf("============== show Tokens ==============\n");
	scanner->show();
}

void Parser::show_stmt(void)
{
	printf("============== show STMT ==============\n");
	for(auto stmt : stmts) {
		if(stmt.type == StmtType::IF) {
			printf("if ");
			for(auto token : stmt.compare) {
				token.show();
			}
		}
		printf("%s ", stmt.gate.c_str());
		for(auto arg : stmt.args) {
			printf("%.2f ", arg);
		}
		for(auto qubit : stmt.qubits) {
			printf("%d ", qubit);
		}
		if(stmt.type == StmtType::MEASURE) {
			printf(" -> ");
			for(auto cbit : stmt.cubits) {
				printf("%d ", cbit);
			}
		}

		printf("\n");
	}
}

void Parser::check_gate_stmt(STMT stmt)
{
	int type = 0;

	auto it = gates.find(stmt.gate);
	if(it == gates.end()) {
		logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), stmt.line);
	} else { 
		type = it->second;
	}

	if(type == G1) {
		if(stmt.args.size() != 0 || stmt.qubits.size() < 1) {
			logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), stmt.line);
		}
	} else if(type == G2) {
		if(stmt.args.size() != 0 || stmt.qubits.size() != 2) {
			logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), stmt.line);
		}
	} else if(type == G3) {
		if(stmt.args.size() != 0 || stmt.qubits.size() != 3) {
			logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), stmt.line);
		}
	} else if(type == G4) {
		if(stmt.args.size() != 1 || stmt.qubits.size() < 1) {
			logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), stmt.line);
		}
	} else if(type == G5) {
		if(stmt.args.size() != 2 || stmt.qubits.size() < 1) {
			logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), stmt.line);
		}
	} else if(type == G6) {
		if(stmt.args.size() != 3 || stmt.qubits.size() < 1) {
			logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), stmt.line);
		}
	} else if(type == G7) {
		if(stmt.args.size() != 1 || stmt.qubits.size() != 2) {
			logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), stmt.line);
		}
	} else if(type == G8) {
		if(stmt.args.size() != 2 || stmt.qubits.size() != 2) {
			logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), stmt.line);
		}
	} else if(type == G9) {
		if(stmt.args.size() != 3 || stmt.qubits.size() != 2) {
			logExit("[%s:%d] invalid gate(%s) in line %d", _F_, _L_, stmt.gate.c_str(), stmt.line);
		}
	}
}

void Parser::init_gates(void) 
{
	gates.insert(make_pair("reset", GateType::G1));
	gates.insert(make_pair("id", GateType::G1));
	gates.insert(make_pair("u1", GateType::G4));
	gates.insert(make_pair("u2", GateType::G5));
	gates.insert(make_pair("u3", GateType::G6));
	gates.insert(make_pair("x", GateType::G1));
	gates.insert(make_pair("y", GateType::G1));
	gates.insert(make_pair("z", GateType::G1));
	gates.insert(make_pair("h", GateType::G1));
	gates.insert(make_pair("p", GateType::G1));
	gates.insert(make_pair("s", GateType::G1));
	gates.insert(make_pair("t", GateType::G1));
	gates.insert(make_pair("sdg", GateType::G1));
	gates.insert(make_pair("tdg", GateType::G1));
	gates.insert(make_pair("rx", GateType::G4));
	gates.insert(make_pair("ry", GateType::G4));
	gates.insert(make_pair("rz", GateType::G4));
	gates.insert(make_pair("cx", GateType::G2));
	gates.insert(make_pair("cz", GateType::G2));
	gates.insert(make_pair("cy", GateType::G2));
	gates.insert(make_pair("ch", GateType::G2));
	gates.insert(make_pair("ccx", GateType::G3));
	gates.insert(make_pair("crz", GateType::G7));
	gates.insert(make_pair("cu1", GateType::G7));
	gates.insert(make_pair("cu2", GateType::G8));
	gates.insert(make_pair("cu3", GateType::G9));
	gates.insert(make_pair("swap", GateType::G2));
	gates.insert(make_pair("cswap", GateType::G3));
	gates.insert(make_pair("sx", GateType::G1));
	gates.insert(make_pair("sxdg", GateType::G1));
	gates.insert(make_pair("iswap", GateType::G2));
} 

void Parser::reset(void)
{
	/* reset quantum states to |00...00> */
	QReg->reset();

	/* reset measure values */
	for(int i=0; i<creg_measure.size(); i++) {
		creg_measure[i] = 0;
	}
}

struct qregister_stat Parser::getQRegStat(void)
{
	return QReg->getQRegStat();
}

void Parser::dumpQReg(void)
{
	dump(QReg);
}
