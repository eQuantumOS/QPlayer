#include "Scanner.h"

void Scanner::readName(Token &t) 
{
	std::stringstream ss;
	while(isdigit(ch) || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '.') {
		ss << ch;
		nextCh();
	}

	t.kind = Kind::str;
	t.str = ss.str();
}

void Scanner::readNumber(Token &t) 
{
	std::stringstream ss;
	while(isdigit(ch)) {
		ss << ch;
		nextCh();
	}
	if(ch != '.') {
		t.kind = Kind::number;
		t.val = std::stoi(ss.str());
		return;
	}
	ss << ch;
	nextCh();
	while(isdigit(ch)) {
		ss << ch;
		nextCh();
	}

	t.kind = Kind::rnumber;
	t.rval = std::stod(ss.str());

	return;
}

void Scanner::skipComment() 
{
	while(ch != '\n' && ch != (char) -1) {
		nextCh();
	}
}

void Scanner::nextCh(void) 
{
	in->get(ch);
}

void Scanner::scan(void) 
{
	bool need_next = true;

	while(!in->eof()) {
		Token t(Kind::none, line);

		if(need_next == true) {
			nextCh();
		} else {
			need_next = true;
		}

		if((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
			readName(t);
			need_next = false;
		} else if(isdigit(ch)) {
			readNumber(t);
			need_next = false;
		} else if(ch == ';') {
			t.kind = Kind::semicolon;
			t.str = ";";
		} else if(ch == '\n') {
			line++;
		} else if(ch == '(') {	
			t.kind = Kind::lpar; 
			t.str = "(";
		} else if(ch == ')') {	
			t.kind = Kind::rpar; 
			t.str = ")";
		} else if(ch == '[') {	
			t.kind = Kind::lbrack; 
			t.str = "[";
		} else if(ch == ']') {	
			t.kind = Kind::rbrack; 
			t.str = "]";
		} else if(ch == '{') {	
			t.kind = Kind::lbrace; 
			t.str = "{";
		} else if(ch == '}') {	
			t.kind = Kind::rbrace; 
			t.str = "}";
		} else if(ch == ',') {	
			t.kind = Kind::comma; 
			t.str = ",";
		} else if(ch == '+') {	
			t.kind = Kind::plus; 
			t.str = ";";
		} else if(ch == '-') {
			nextCh(); 
			if(ch == '>') {
				t.kind = Kind::arrow; 
				t.str = "->";
			} else {
				t.kind = Kind::minus; 
				t.str = "-";
				need_next = false;
			}
		} else if(ch == '*') {	
			t.kind = Kind::multiply; 
			t.str = "*";
		} else if(ch == '/') {
			nextCh(); 
			if(ch == '/') {
				skipComment();
			} else {
				t.kind = Kind::divide; 
				t.str = "/";
			}
			need_next = false;
		} else if(ch == '>') {	
			nextCh(); 
			if(ch == '=') {
				t.kind = Kind::gte; 
				t.str = ">=";
			} else {
				t.kind = Kind::gt; 
				t.str = ">";
				need_next = false;
			}
		} else if(ch == '<') {	
			nextCh(); 
			if(ch == '=') {
				t.kind = Kind::lte; 
				t.str = "<=";
			} else {
				t.kind = Kind::lt; 
				t.str = "<";
				need_next = false;
			}
		} else if(ch == '=') {	
			nextCh(); 
			if(ch == '=') {
				t.kind = Kind::eq; 
				t.str = "==";
			} else {
				need_next = false;
				t.str = "=";
			}
		}

		if(t.kind != Kind::none) {
			tokens.push_back(t);
		}
	}

	check_args();
}

void Scanner::check_args(void) 
{
	Kind curKind = Kind::none;
	int brack_cnt = 0;
	int par_cnt = 0;
	int brace_cnt = 0;
	for(auto k : tokens) {
		if(k.kind == Kind::lbrack) {
			if(brack_cnt != 0) {
				logExit("[%s:%d] '[]' pair is invalid in line %d", _F_, _L_, k.line);
			}
			brack_cnt++;
		} else if(k.kind == Kind::rbrack) {
			if(brack_cnt == 0) {
				logExit("[%s:%d] '[]' pair is invalid in line %d", _F_, _L_, k.line);
			}
			brack_cnt--;
		} else if(k.kind == Kind::lpar) {
			par_cnt++;
		} else if(k.kind == Kind::rpar) {
			if(par_cnt == 0) {
				logExit("[%s:%d] '()' pair is invalid in line %d", _F_, _L_, k.line);
			}
			par_cnt--;
		} else if(k.kind == Kind::lbrace) {
			brace_cnt++;
		} else if(k.kind == Kind::rbrace) {
			if(brace_cnt == 0) {
				logExit("[%s:%d] '{}' pair is invalid in line %d", _F_, _L_, k.line);
			}
			brace_cnt--;
		}
	}

	if(par_cnt != 0 || brack_cnt != 0 || brace_cnt != 0) {
		logExit("[%s:%d] symbolic pair('[]', '()', '{}') is invalid.", _F_, _L_);
	}
}

void Scanner::build_ugates(std::map<std::string, Ugate> &ugates) 
{
	for(int i=0; i<tokens.size(); i++) {
		if(tokens[i].str != "gate") {
			continue;
		}

		class Ugate ug;
		ug.name = tokens[++i].str;

		if(tokens[i+1].kind == Kind::lpar) {
			int par_cnt = 1;
			while(par_cnt != 0) {
				i++;
				if(tokens[i].kind == Kind::comma) {
					continue;
				} else if(tokens[i].kind == Kind::lpar) {
					continue;
				} else if(tokens[i].kind == Kind::rpar) {
					break;
				} 
				ug.args.push_back(tokens[i].str);
			}
		}

		while(tokens[++i].kind != Kind::lbrace) {
			if(tokens[i].kind != Kind::comma) {
				ug.params.push_back(tokens[i].str);
			}
		}

		while(tokens[++i].kind != Kind::rbrace) {
			ug.tokens.push_back(tokens[i]);
		}

		ugates.insert(make_pair(ug.name, ug));
	}
}

void Scanner::build_qregs(std::map<std::string, Reg_args> &qregs) 
{
	Reg_args regs;
	int qsize = 0;
	int qpos = 0;

	for(int i=0; i<tokens.size(); i++) {
		if(tokens[i].str != "qreg") {
			continue;
		}

		std::string name = tokens[++i].str;

		if(tokens[++i].kind != Kind::lbrack) {	
			logExit("[%s:%d] qreg definition is invalid in line %d", _F_, _L_, tokens[i].line);
		}

		qsize = tokens[++i].val;
		regs.pos = qpos;
		regs.size = qsize;

		qregs.insert(make_pair(name, regs));
		qpos += qsize;

		if(tokens[++i].kind != Kind::rbrack) {	
			logExit("[%s:%d] qreg definition is invalid in line %d", _F_, _L_, tokens[i].line);
		}
	}
}

void Scanner::build_cregs(std::map<std::string, Reg_args> &cregs) 
{
	Reg_args regs;
	int csize = 0;
	int cpos = 0;

	for(int i=0; i<tokens.size(); i++) {
		if(tokens[i].str != "creg") {
			continue;
		}

		std::string name = tokens[++i].str;

		if(tokens[++i].kind != Kind::lbrack) {	
			logExit("[%s:%d] creg definition is invalid in line %d", _F_, _L_, tokens[i].line);
		}

		csize = tokens[++i].val;
		regs.pos = cpos;
		regs.size = csize;

		cregs.insert(make_pair(name, regs));
		cpos += csize;

		if(tokens[++i].kind != Kind::rbrack) {	
			logExit("[%s:%d] creg definition is invalid in line %d", _F_, _L_, tokens[i].line);
		}
	}
}

void Scanner::get_tokens(std::vector<Token> &stmt, int &pos) 
{
	stmt.clear();

	for(; pos<tokens.size(); pos++) {
		if(tokens[pos].str == "OPENQASM" || tokens[pos].str == "include") {
			while(tokens[++pos].kind != Kind::semicolon) {}
		} else if(tokens[pos].str == "qreg" || tokens[pos].str == "creg") {
			while(tokens[++pos].kind != Kind::semicolon) {}
		} else if(tokens[pos].str == "gate") {
			while(tokens[++pos].kind != Kind::rbrace) {}
		} else {
			while(1) {
				stmt.push_back(tokens[pos]);
				if(tokens[pos].kind == Kind::semicolon) {
					break;
				}
				pos++;
			}
			pos++;
			break;
		} 
	}
}

void Scanner::get_utokens(std::vector<Token> source, std::vector<Token> &stmt, int &pos) 
{
	stmt.clear();

	for(; pos<source.size(); pos++) {
		if(source[pos].str == "OPENQASM" || source[pos].str == "include") {
			while(source[++pos].kind != Kind::semicolon) {}
		} else if(source[pos].str == "qreg" || source[pos].str == "creg") {
			while(source[++pos].kind != Kind::semicolon) {}
		} else if(source[pos].str == "gate") {
			while(source[++pos].kind != Kind::rbrace) {}
		} else {
			while(1) {
				stmt.push_back(source[pos]);
				if(source[pos].kind == Kind::semicolon) {
					break;
				}
				pos++;
			}
			pos++;
			break;
		} 
	}
}

void Scanner::show(void) {
	for(auto t : tokens) {
		t.show();
	}
	printf("\n");
}
