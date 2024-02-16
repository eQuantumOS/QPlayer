#include "Token.h"

char *Token::token2str(Kind kind) 
{
	switch(kind) {
	case Kind::none: return "none";
	case Kind::openqasm: return "OPENQASM";
	case Kind::include: return "include";
	case Kind::qreg: return "qreg";
	case Kind::creg: return "creg";
	case Kind::gate: return "gate";
	case Kind::number: return "number";
	case Kind::rnumber: return "rnumber";
	case Kind::str: return "string";
	case Kind::semicolon: return "semicolon";
	case Kind::eol: return "eol";
	case Kind::comma: return "comma";
	case Kind::plus: return "plus";
	case Kind::minus: return "minus";
	case Kind::multiply: return "multiply";
	case Kind::divide: return "divide";
	case Kind::lpar: return "lpar";
	case Kind::rpar: return "rpar";
	case Kind::lbrack: return "lbrack";
	case Kind::rbrack: return "rbrack";
	case Kind::lbrace: return "lbrace";
	case Kind::rbrace: return "rbrace";
	case Kind::PI: return "pi";
	case Kind::SIN: return "sin";
	case Kind::COS: return "cos";
	case Kind::TAN: return "tan";
	case Kind::EXP: return "exp";
	case Kind::LOG: return "log";
	case Kind::SQRT: return "sqrt";
	case Kind::POWER: return "pow";
	case Kind::lt: return "lt";
	case Kind::lte: return "lte";
	case Kind::eq: return "eq";
	case Kind::gt: return "gt";
	case Kind::gte: return "gte";
	case Kind::arrow: return "arrow";
	case Kind::barrier: return "barrier";
	case Kind::ifstmt: return "if";
	}

	return "none";
}

void Token::show(void) 
{
	if(kind == Kind::none) {
		return;
	} else if(kind == Kind::eol) {
		printf("\n");
	} else if(kind == Kind::lbrace) {
		printf("{\n");
	} else if(kind == Kind::rbrace) {
		printf("}\n");
	} else if(kind == Kind::semicolon) {
		printf(";\n");
	} else {
		if(val != INT_MAX) {
			printf("%d ", val);
		} else if(rval != LONG_MAX) {
			printf("%.2f ", rval);
		} else {
			printf("%s ", str.c_str());
		}
	}
}
