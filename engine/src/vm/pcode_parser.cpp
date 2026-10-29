#include "vm/pcode_parser.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace vm::pc {

namespace {

struct Token {
	enum Kind { End, Ident, Int, Float, String, Punct } kind = End;
	std::string text;
	int64_t ival = 0;
	double fval = 0;
	int line = 1;
};

std::vector<Token> lex(const std::string &src, const std::string &where) {
	std::vector<Token> out;
	size_t i = 0, n = src.find('\0');
	if (n == std::string::npos) n = src.size();
	int line = 1;
	auto fail = [&](const std::string &m) { throw CompileError(where + ":" + std::to_string(line) + ": " + m); };
	while (i < n) {
		char c = src[i];
		if (c == '\n') {
			++line, ++i;
			continue;
		}
		if (isspace(uint8_t(c)) || uint8_t(c) >= 0x80) {
			++i;
			continue;
		}
		if (c == '/' && i + 1 < n && src[i + 1] == '/') {
			while (i < n && src[i] != '\n') ++i;
			continue;
		}
		if (c == '/' && i + 1 < n && src[i + 1] == '*') {
			i += 2;
			while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
				if (src[i] == '\n') ++line;
				++i;
			}
			i += 2;
			continue;
		}
		Token t;
		t.line = line;
		if (isalpha(uint8_t(c)) || c == '_') {
			size_t s = i;
			while (i < n && (isalnum(uint8_t(src[i])) || src[i] == '_')) ++i;
			t.kind = Token::Ident;
			t.text = src.substr(s, i - s);
		} else if (isdigit(uint8_t(c)) || (c == '.' && i + 1 < n && isdigit(uint8_t(src[i + 1])))) {
			size_t s = i;
			bool is_float = false;
			if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
				i += 2;
				while (i < n && isxdigit(uint8_t(src[i]))) ++i;
				t.kind = Token::Int;
				t.ival = int64_t(strtoull(src.substr(s, i - s).c_str(), nullptr, 16));
			} else {
				while (i < n && isdigit(uint8_t(src[i]))) ++i;
				if (i < n && src[i] == '.') {
					is_float = true;
					++i;
					while (i < n && isdigit(uint8_t(src[i]))) ++i;
				}
				if (i < n && (src[i] == 'e' || src[i] == 'E') && i + 1 < n &&
				    (isdigit(uint8_t(src[i + 1])) || ((src[i + 1] == '-' || src[i + 1] == '+') && i + 2 < n && isdigit(uint8_t(src[i + 2]))))) {
					is_float = true;
					i += 2;
					while (i < n && isdigit(uint8_t(src[i]))) ++i;
				}
				std::string num = src.substr(s, i - s);
				if (i < n && (src[i] == 'f' || src[i] == 'F')) {
					is_float = true;
					++i;
				}
				if (is_float) {
					t.kind = Token::Float;
					t.fval = strtod(num.c_str(), nullptr);
				} else {
					t.kind = Token::Int;
					t.ival = int64_t(strtoull(num.c_str(), nullptr, 10));
				}
			}
		} else if (c == '"') {
			++i;
			t.kind = Token::String;
			while (i < n && src[i] != '"') {
				if (src[i] == '\\' && i + 1 < n) {
					char e = src[i + 1];
					t.text += e == 'n' ? '\n' : e == 't' ? '\t' : e;
					i += 2;
					continue;
				}
				if (src[i] == '\n') ++line;
				t.text += src[i++];
			}
			if (i >= n) fail("unterminated string");
			++i;
		} else if (c == '\'') {
			++i;
			t.kind = Token::Int;
			if (i < n) t.ival = uint8_t(src[i++]);
			if (i < n && src[i] == '\'') ++i;
		} else {
			static const char *ops[] = {"::", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "==", "!=", "<=", ">=",
			                            "&&", "||", "<<", ">>", nullptr};
			t.kind = Token::Punct;
			for (const char **o = ops; *o; ++o) {
				if (src.compare(i, 2, *o) == 0) {
					t.text = *o;
					break;
				}
			}
			if (t.text.empty()) t.text = std::string(1, c);
			i += t.text.size();
		}
		out.push_back(std::move(t));
	}
	Token end;
	end.line = line;
	out.push_back(end);
	return out;
}

bool builtin_type(const std::string &s) {
	static const char *names[] = {"int", "float", "dword", "word", "byte", "bool", "string", "char", "short", "void", nullptr};
	for (const char **p = names; *p; ++p) {
		if (s == *p) return true;
	}
	return false;
}

// Native classes (CActor, COMF2Fighter) and behaviors (BCar) are capitalised C/B + capital.
bool looks_like_class(const std::string &s) {
	return s.size() > 1 && (s[0] == 'C' || s[0] == 'B' || s[0] == 'E') && isupper(uint8_t(s[1]));
}

class Parser {
public:
	Parser(std::vector<Token> t, std::string where) : t_(std::move(t)), where_(std::move(where)) {}

	std::vector<StmtPtr> statements() {
		std::vector<StmtPtr> out;
		while (!at_end()) {
			if (accept("}")) continue;  // a stray closing brace (some stored sources have one)
			out.push_back(statement());
		}
		return out;
	}

private:
	std::vector<Token> t_;
	std::string where_;
	size_t p_ = 0;

	const Token &peek(size_t k = 0) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
	bool at_end() const { return peek().kind == Token::End; }
	bool is(const char *punct, size_t k = 0) const { return peek(k).kind == Token::Punct && peek(k).text == punct; }
	bool is_word(const char *w, size_t k = 0) const { return peek(k).kind == Token::Ident && peek(k).text == w; }
	[[noreturn]] void fail(const std::string &m) const {
		throw CompileError(where_ + ":" + std::to_string(peek().line) + ": " + m + " near '" + peek().text + "'");
	}
	void expect(const char *punct) {
		if (!is(punct)) fail(std::string("expected '") + punct + "'");
		++p_;
	}
	bool accept(const char *punct) {
		if (!is(punct)) return false;
		++p_;
		return true;
	}
	std::string ident() {
		if (peek().kind != Token::Ident) fail("expected a name");
		return t_[p_++].text;
	}

	bool starts_decl() const {
		static const char *mods[] = {"final", "instance", "replicate", "param", "rparam", "interface", nullptr};
		for (const char **m = mods; *m; ++m) {
			if (is_word(*m)) return true;
		}
		if (peek().kind != Token::Ident || is_word("return") || is_word("else")) return false;
		// Type name: a word (CPrimitiveGeneratorMesh::CTextPrimitive) followed by another word.
		size_t k = 1;
		while (is("::", k) && peek(k + 1).kind == Token::Ident) k += 2;
		return peek(k).kind == Token::Ident;
	}

	std::unique_ptr<Decl> declaration() {
		auto d = std::make_unique<Decl>();
		d->line = peek().line;
		for (;;) {
			if (is_word("final")) d->final_ = true;
			else if (is_word("instance")) d->instance = true;
			else if (is_word("replicate")) d->replicate = true;
			else if (is_word("param")) d->param = true;
			else if (is_word("rparam")) d->rparam = true;
			else if (is_word("interface")) d->interface = true;
			else break;
			++p_;
		}
		d->type = ident();
		while (accept("::")) d->type += "::" + ident();
		do {
			Declarator dc;
			dc.name = ident();
			if (accept("[")) {
				expect("]");
				dc.array = true;
			}
			if (accept("=")) dc.init = assignment();
			d->names.push_back(std::move(dc));
		} while (accept(","));
		expect(";");
		return d;
	}

	StmtPtr make(Stmt::Kind k) {
		auto s = std::make_unique<Stmt>();
		s->kind = k;
		s->line = peek().line;
		return s;
	}

	StmtPtr statement() {
		if (accept(";")) return make(Stmt::Empty);
		if (is("{")) {
			auto s = make(Stmt::Block);
			++p_;
			// Some stored sources end inside a block (they compiled): the end closes it.
			while (!is("}") && !at_end()) s->list.push_back(statement());
			accept("}");
			return s;
		}
		if (is_word("if")) {
			auto s = make(Stmt::If);
			++p_;
			expect("(");
			s->cond = expression();
			expect(")");
			s->body = statement();
			if (is_word("else")) {
				++p_;
				s->other = statement();
			}
			return s;
		}
		if (is_word("while")) {
			auto s = make(Stmt::While);
			++p_;
			expect("(");
			s->cond = expression();
			expect(")");
			s->body = statement();
			return s;
		}
		if (is_word("do")) {
			auto s = make(Stmt::DoWhile);
			++p_;
			s->body = statement();
			if (!is_word("while")) fail("expected while");
			++p_;
			expect("(");
			s->cond = expression();
			expect(")");
			expect(";");
			return s;
		}
		if (is_word("for")) {
			auto s = make(Stmt::For);
			++p_;
			expect("(");
			if (starts_decl()) {
				auto d = make(Stmt::DeclS);
				d->decl = declaration();
				s->init = std::move(d);
			} else {
				if (!is(";")) {
					auto e = make(Stmt::ExprS);
					e->e = expression();
					s->init = std::move(e);
				}
				expect(";");
			}
			if (!is(";")) s->cond = expression();
			expect(";");
			if (!is(")")) s->step = expression();
			expect(")");
			s->body = statement();
			return s;
		}
		if (is_word("switch")) {
			auto s = make(Stmt::Switch);
			++p_;
			expect("(");
			s->cond = expression();
			expect(")");
			expect("{");
			while (!is("}")) {
				if (at_end()) fail("unterminated switch");
				if (is_word("case")) {
					++p_;
					s->cases.emplace_back(s->list.size(), expression());
					expect(":");
				} else if (is_word("default")) {
					++p_;
					expect(":");
					s->default_at = int64_t(s->list.size());
				} else {
					s->list.push_back(statement());
				}
			}
			++p_;
			return s;
		}
		if (is_word("return")) {
			auto s = make(Stmt::Return);
			++p_;
			if (!is(";")) s->e = expression();
			expect(";");
			return s;
		}
		if (is_word("break") || is_word("continue")) {
			auto s = make(is_word("break") ? Stmt::Break : Stmt::Continue);
			++p_;
			expect(";");
			return s;
		}
		if (is_word("inherit")) {
			auto s = make(Stmt::Inherit);
			++p_;
			s->name = ident();
			while (accept(".")) s->name += "." + ident();
			expect(";");
			return s;
		}
		if (is_word("enum")) {
			auto s = make(Stmt::Enum);
			++p_;
			s->enm = std::make_unique<EnumDecl>();
			s->enm->name = ident();
			expect("{");
			while (!is("}")) {
				std::string n = ident();
				ExprPtr v;
				if (accept("=")) v = assignment();
				s->enm->values.emplace_back(n, std::move(v));
				if (!accept(",")) break;
			}
			expect("}");
			accept(";");
			return s;
		}
		if (starts_decl()) {
			auto s = make(Stmt::DeclS);
			s->decl = declaration();
			return s;
		}
		auto s = make(Stmt::ExprS);
		s->e = expression();
		expect(";");
		return s;
	}

	ExprPtr node(Expr::Kind k) {
		auto e = std::make_unique<Expr>();
		e->kind = k;
		e->line = peek().line;
		return e;
	}

	ExprPtr expression() {
		auto e = assignment();
		while (is(",")) {  // comma expressions: keep the last (rare)
			++p_;
			e = assignment();
		}
		return e;
	}

	ExprPtr assignment() {
		auto lhs = ternary();
		static const char *ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", nullptr};
		for (const char **o = ops; *o; ++o) {
			if (is(*o)) {
				auto e = node(Expr::Assign);
				e->text = *o;
				++p_;
				e->a = std::move(lhs);
				e->b = assignment();
				return e;
			}
		}
		return lhs;
	}

	ExprPtr ternary() {
		auto c = binary(0);
		if (!is("?")) return c;
		auto e = node(Expr::Ternary);
		++p_;
		e->a = std::move(c);
		e->b = assignment();
		expect(":");
		e->c = assignment();
		return e;
	}

	static int precedence(const std::string &op) {
		static const std::pair<const char *, int> table[] = {
		    {"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5}, {"==", 6}, {"!=", 6}, {"<", 7}, {">", 7}, {"<=", 7},
		    {">=", 7}, {"<<", 8}, {">>", 8}, {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10}};
		for (auto &[o, p] : table) {
			if (op == o) return p;
		}
		return -1;
	}

	ExprPtr binary(int min) {
		auto lhs = unary();
		for (;;) {
			if (peek().kind != Token::Punct) return lhs;
			int p = precedence(peek().text);
			if (p < 0 || p <= min) return lhs;
			auto e = node(Expr::Binary);
			e->text = peek().text;
			++p_;
			e->a = std::move(lhs);
			e->b = binary(p);
			lhs = std::move(e);
		}
	}

	bool cast_ahead() const {
		if (!is("(") || peek(1).kind != Token::Ident) return false;
		size_t k = 2;
		while (is("::", k) && peek(k + 1).kind == Token::Ident) k += 2;
		if (!is(")", k)) return false;
		const std::string &n = peek(1).text;
		if (!builtin_type(n) && !looks_like_class(n)) return false;
		const Token &next = peek(k + 1);
		return next.kind == Token::Ident || next.kind == Token::Int || next.kind == Token::Float || next.kind == Token::String ||
		       (next.kind == Token::Punct && (next.text == "(" || next.text == "!" || next.text == "-" || next.text == "~"));
	}

	ExprPtr unary() {
		if (is("!") || is("-") || is("~") || is("+")) {
			auto e = node(Expr::Unary);
			e->text = peek().text;
			++p_;
			e->a = unary();
			if (e->text == "+") return std::move(e->a);
			return e;
		}
		if (is("++") || is("--")) {
			auto e = node(is("++") ? Expr::PreInc : Expr::PreDec);
			++p_;
			e->a = unary();
			return e;
		}
		if (cast_ahead()) {
			auto e = node(Expr::Cast);
			++p_;
			e->text = ident();
			while (accept("::")) e->text += "::" + ident();
			expect(")");
			e->a = unary();
			return e;
		}
		return postfix();
	}

	ExprPtr postfix() {
		auto e = primary();
		for (;;) {
			if (is("(")) {
				auto c = node(Expr::Call);
				++p_;
				c->a = std::move(e);
				if (!is(")")) {
					do c->args.push_back(assignment());
					while (accept(","));
				}
				expect(")");
				e = std::move(c);
			} else if (is(".")) {
				auto m = node(Expr::Member);
				++p_;
				m->text = ident();
				m->a = std::move(e);
				e = std::move(m);
			} else if (is("[")) {
				auto ix = node(Expr::Index);
				++p_;
				ix->a = std::move(e);
				ix->b = expression();
				expect("]");
				e = std::move(ix);
			} else if (is("++") || is("--")) {
				auto pe = node(is("++") ? Expr::PostInc : Expr::PostDec);
				++p_;
				pe->a = std::move(e);
				e = std::move(pe);
			} else {
				return e;
			}
		}
	}

	ExprPtr primary() {
		const Token &t = peek();
		if (t.kind == Token::Int) {
			auto e = node(Expr::Int);
			e->ival = t.ival;
			++p_;
			return e;
		}
		if (t.kind == Token::Float) {
			auto e = node(Expr::Float);
			e->fval = t.fval;
			++p_;
			return e;
		}
		if (t.kind == Token::String) {
			auto e = node(Expr::String);
			e->text = t.text;
			++p_;
			while (peek().kind == Token::String) e->text += t_[p_++].text;  // "a" "b"
			return e;
		}
		if (accept("(")) {
			auto e = expression();
			expect(")");
			return e;
		}
		if (is(".")) {  // .Bounce, .PlayerWon.PlayerDestroyed1: a method path from the running state
			++p_;
			auto e = node(Expr::Ident);
			e->text = "." + ident();
			return e;
		}
		if (t.kind == Token::Ident) {
			if (t.text == "null") {
				++p_;
				return node(Expr::Null);
			}
			auto e = node(Expr::Ident);
			e->text = ident();
			if (is("::")) {
				auto s = node(Expr::Scoped);
				s->text = e->text;
				while (accept("::")) s->text += "::" + ident();
				return s;
			}
			return e;
		}
		fail("expected an expression");
	}
};

}  // namespace

std::vector<StmtPtr> parse(const std::string &source, const std::string &where) {
	Parser p(lex(source, where), where);
	return p.statements();
}

}  // namespace vm::pc
