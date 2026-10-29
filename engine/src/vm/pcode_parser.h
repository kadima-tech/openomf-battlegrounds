// The script language of Behavior sources (PCParser::yyparse / PCScanner::GetToken in Core):
// a C-like language of declarations, statements and expressions. This is the front end of the
// compiler for the classes the game ships without VMC2 (compiler.h).
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace vm::pc {

struct CompileError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
	enum Kind {
		Int, Float, String, Null, Ident, Scoped,  // literals; a name; Base::Name
		Call,      // callee(args): callee is Ident, Scoped or Member
		Member,    // a.name
		Index,     // a[i]
		Unary,     // op a (! - ~)
		PreInc, PreDec, PostInc, PostDec,
		Binary,    // a op b
		Assign,    // a op= b (op "=" or "+=" ...)
		Cast,      // (Type)a
		Ternary,   // a ? b : c
	} kind;
	int line = 0;
	int64_t ival = 0;
	double fval = 0;
	std::string text;  // identifier, string literal, operator, cast type
	ExprPtr a, b, c;
	std::vector<ExprPtr> args;
};

struct Declarator {
	std::string name;
	bool array = false;
	ExprPtr init;
};

struct Decl {
	bool final_ = false, instance = false, replicate = false, param = false, rparam = false, interface = false;
	std::string type;  // e.g. "int", "CActor", "BCar" (with interface)
	std::vector<Declarator> names;
	int line = 0;
};

struct EnumDecl {
	std::string name;
	std::vector<std::pair<std::string, ExprPtr>> values;
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Stmt {
	enum Kind { Empty, ExprS, DeclS, Block, If, While, DoWhile, For, Return, Break, Continue, Switch, Inherit, Enum } kind;
	int line = 0;
	ExprPtr e, cond, step;
	StmtPtr init, body, other;  // For: init/cond/step/body; If: body/other
	std::vector<StmtPtr> list;  // Block; Switch: the body statements
	std::unique_ptr<Decl> decl;
	std::unique_ptr<EnumDecl> enm;
	std::string name;           // Inherit
	// Switch: case labels by statement index (default: case_default).
	std::vector<std::pair<size_t, ExprPtr>> cases;
	int64_t default_at = -1;
};

// A method's or members section's statements. The text ends at its first NUL (the stored sources
// carry editor bytes after it).
std::vector<StmtPtr> parse(const std::string &source, const std::string &where);

}  // namespace vm::pc
