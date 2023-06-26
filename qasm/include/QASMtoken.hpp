/*
DD-based simulator by JKU Linz, Austria

Developer: Alwin Zulehner, Robert Wille

With code from the QMDD implementation provided by Michael Miller (University of Victoria, Canada)
and Philipp Niemann (University of Bremen, Germany).

For more information, please visit http://iic.jku.at/eda/research/quantum_simulation

If you have any questions feel free to contact us using
alwin.zulehner@jku.at or robert.wille@jku.at

If you use the quantum simulator for your research, we would be thankful if you referred to it
by citing the following publication:

@article{zulehner2018simulation,
    title={Advanced Simulation of Quantum Computations},
    author={Zulehner, Alwin and Wille, Robert},
    journal={IEEE Transactions on Computer Aided Design of Integrated Circuits and Systems (TCAD)},
    year={2018},
    eprint = {arXiv:1707.00865}
}
*/

#ifndef TOKEN_H_
#define TOKEN_H_

#include <string>
#include <map>

class Token {
 public:

	enum class Kind {include, none, identifier, number, plus, semicolon, eof, lpar, rpar, lbrack, rbrack, lbrace, rbrace, comma, minus, times, nninteger, real, qreg, creg, ugate, cxgate, gate, pi, measure, openqasm, probabilities, sin, cos, tan, exp, ln, sqrt, div, power, string, gt, barrier, opaque, _if, eq, reset, snapshot};

	Token(Kind kind, int line, int col) {
		this->kind = kind;
		this->line = line;
		this->col = col;
		this->val = 0;
		this->valReal = 0.0;
	}

	Token() : Token(Kind::none, 0, 0) {
	}

	static std::map<Kind, std::string> KindNames;
	Kind kind;
	int line;
	int col;
	int val;
	double valReal;
	std::string str;

	// added by ksjin
	char *kindStr() { return __kindStr(this->kind); }
	char *kindStr(Kind k) { return __kindStr(k); }

	char *__kindStr(Kind k) {
		if(k == Kind::include) 
			return "include";
		else if(k == Kind::none) 
			return "none";
		else if(k == Kind::identifier) 
			return "identifier";
		else if(k == Kind::number) 
			return "number";
		else if(k == Kind::plus) 
			return "plus";
		else if(k == Kind::semicolon) 
			return "semicolon";
		else if(k == Kind::eof) 
			return "eof";
		else if(k == Kind::lpar) 
			return "lpar";
		else if(k == Kind::rpar) 
			return "rpar";
		else if(k == Kind::lbrack) 
			return "lbrack";
		else if(k == Kind::rbrack) 
			return "rbrack";
		else if(k == Kind::lbrace) 
			return "lbrace";
		else if(k == Kind::rbrace) 
			return "rbrace";
		else if(k == Kind::comma) 
			return "comma";
		else if(k == Kind::minus) 
			return "minus";
		else if(k == Kind::times) 
			return "times";
		else if(k == Kind::nninteger) 
			return "nninteger";
		else if(k == Kind::real) 
			return "real";
		else if(k == Kind::qreg) 
			return "qreg";
		else if(k == Kind::creg) 
			return "creg";
		else if(k == Kind::ugate) 
			return "ugate";
		else if(k == Kind::cxgate) 
			return "cxgate";
		else if(k == Kind::gate) 
			return "gate";
		else if(k == Kind::pi) 
			return "pi";
		else if(k == Kind::measure) 
			return "measure";
		else if(k == Kind::openqasm) 
			return "openqasm";
		else if(k == Kind::probabilities) 
			return "probabilities";
		else if(k == Kind::sin) 
			return "sin";
		else if(k == Kind::cos) 
			return "cos";
		else if(k == Kind::tan) 
			return "tan";
		else if(k == Kind::exp) 
			return "exp";
		else if(k == Kind::ln) 
			return "ln";
		else if(k == Kind::sqrt) 
			return "sqrt";
		else if(k == Kind::div) 
			return "div";
		else if(k == Kind::power) 
			return "power";
		else if(k == Kind::string) 
			return "string";
		else if(k == Kind::gt) 
			return "gt";
		else if(k == Kind::barrier) 
			return "barrier";
		else if(k == Kind::opaque) 
			return "opaque";
		else if(k == Kind::_if) 
			return "_if";
		else if(k == Kind::eq) 
			return "eq";
		else if(k == Kind::reset) 
			return "reset";
		else if(k == Kind::snapshot) 
			return "snapshot";
		return "unknown";
	}
 };

#endif /* TOKEN_H_ */
