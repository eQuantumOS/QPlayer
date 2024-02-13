#pragma once

#define MAX_LENG 512

#include <iostream>
#include <string>
#include <stack>
#include <sstream>
#include <cmath>

/* operation parameter for calculator */
struct opparam {
	int p;      		// priority
	std::string o;   	// op
};
 
inline static void calc(stack<double> &opval, stack<opparam> &op) {
	double p1 = 0;
	double p2 = 0;
	double result = 0;

	p2 = opval.top();
	opval.pop();
	p1 = opval.top();
	opval.pop();
	std::string opparam = op.top().o;
	op.pop();

	if (opparam == "*")
		result =  p1 * p2;
	else if (opparam == "/")
		result = p1 / p2;
	else if (opparam == "+")
		result = p1 + p2;
	else if (opparam == "-")
		result = p1 - p2;

	opval.push(result);
}

inline static void calTokenizer(std::string &str) {
	std::string netStr;
	std::string valString;
	int leftPos = 0;
	int rightPos = 0;
	int pos = 0;
	int type;

	/**********************************************/
	/* STEP1: replace 'pi' string to double value */
	/**********************************************/
	netStr.clear();
	do { 
		if((pos = str.find("pi")) == string::npos) {
			break;
		}

		leftPos = str.find("p", pos);
		rightPos = str.find("i", leftPos);

		valString = str.substr(leftPos+1, rightPos-leftPos-1);
		netStr = str.substr(0, pos);
		netStr += "3.141592653589793238462643383279502884";
		if(rightPos < str.size() - 1) {
			netStr += str.substr(rightPos+1, str.size()-1);
		}
		str = netStr;
	} while(1);

	/**************************************************/
	/* STEP2: replace cos(val) string to double value */
	/**************************************************/
	netStr.clear();
	do { 
		if((pos = str.find("cos")) != string::npos) {
			type = COS;
		} else if((pos = str.find("sin")) != string::npos) {
			type = SIN;
		} else if((pos = str.find("tan")) != string::npos) {
			type = TAN;
		} else if((pos = str.find("exp")) != string::npos) {
			type = EXP;
		} else if((pos = str.find("ln")) != string::npos) {
			type = LOG;
		} else if((pos = str.find("sqrt")) != string::npos) {
			type = SQRT;
		} else {
			break;
		}

		leftPos = str.find("(", pos);
		rightPos = str.find(")", leftPos);

		valString = str.substr(leftPos+1, rightPos-leftPos-1);
		netStr = str.substr(0, pos);

		switch(type) {
		case COS: netStr += to_string(cos(stod(valString))); break;
		case SIN: netStr += to_string(sin(stod(valString))); break;
		case TAN: netStr += to_string(tan(stod(valString))); break;
		case EXP: netStr += to_string(exp(stod(valString))); break;
		case LOG: netStr += to_string(log(stod(valString))); break;
		case SQRT: netStr += to_string(sqrt(stod(valString))); break;
		}

		if(rightPos < str.size() - 1) {
			netStr += str.substr(rightPos+1, str.size()-1);
		}
		str = netStr;
	} while(1);

	/*******************************************/
	/* STEP3: split tokens for stack operation */
	/*******************************************/
	netStr.clear();
	for(int i=0; i<str.size(); i++) {
		bool seperator = false;

		if(str[i]=='(' || str[i]==')') {
			seperator = true;
		} else if(str[i]=='-' && i==0) {
			netStr.push_back(str[i]);
			continue;
		} else if(str[i]=='-' && (str[i-1]!='*' && str[i-1]!='/' && str[i-1]!='+')) {
			seperator = true;
		} else if(str[i]=='*' || str[i]=='/' || str[i]=='+') {
			seperator = true;
		}

		if(seperator == true) {
			netStr.push_back(' ');
			netStr.push_back(str[i]);
			netStr.push_back(' ');
		} else {
			netStr.push_back(str[i]);
		}
	}

	str = netStr;
}

inline static double calArg(std::string str) 
{
	calTokenizer(str);
	stringstream ss(str);
	stack<double> opval; 
	stack<opparam> op; 
	double rval = 0;

	std::string tok;
	while (ss >> tok) {
		if (tok == "(") {
			op.push({0, tok});
		} else if (tok == ")") {
			while (op.top().o != "(")
				calc(opval, op);
			op.pop();
		} else if (tok == "*" || tok == "/" || tok == "+" || tok == "-") {
			int prior = 0; 
			if(tok == "*")
				prior = 2;
			else if(tok == "/")
				prior = 2;
			else if(tok == "+")
				prior = 1;
			else if(tok == "-")
				prior = 1;
		 
			while (!op.empty() && prior <= op.top().p)
				calc(opval, op);

			op.push({prior, tok});
		} else {
			opval.push(stod(tok));
		}
	}

	while (!op.empty())
		calc(opval, op);
 
	return opval.top();
}
