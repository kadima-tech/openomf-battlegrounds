#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace json {

namespace {

const char *type_name(Value::Type t) {
	switch (t) {
	case Value::Null: return "null";
	case Value::Bool: return "a boolean";
	case Value::Int:
	case Value::Real: return "a number";
	case Value::String: return "a string";
	case Value::Array: return "an array";
	case Value::Object: return "an object";
	}
	return "?";
}

[[noreturn]] void wrong(Value::Type have, const char *want) {
	throw Error(std::string("expected ") + want + ", found " + type_name(have));
}

void append_utf8(std::string &out, uint32_t cp) {
	if (cp < 0x80) {
		out += char(cp);
	} else if (cp < 0x800) {
		out += char(0xC0 | (cp >> 6));
		out += char(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += char(0xE0 | (cp >> 12));
		out += char(0x80 | ((cp >> 6) & 0x3F));
		out += char(0x80 | (cp & 0x3F));
	} else {
		out += char(0xF0 | (cp >> 18));
		out += char(0x80 | ((cp >> 12) & 0x3F));
		out += char(0x80 | ((cp >> 6) & 0x3F));
		out += char(0x80 | (cp & 0x3F));
	}
}

void quote(std::string &out, const std::string &s) {
	out += '"';
	for (unsigned char ch : s) {
		switch (ch) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (ch < 0x20) {
				char buf[8];
				snprintf(buf, sizeof buf, "\\u%04x", ch);
				out += buf;
			} else {
				out += char(ch);
			}
		}
	}
	out += '"';
}

class Parser {
public:
	explicit Parser(const std::string &s) : s_(s) {}

	Value document() {
		Value v = value();
		ws();
		if (p_ != s_.size()) fail("unexpected text after the document");
		return v;
	}

private:
	[[noreturn]] void fail(const std::string &what) {
		size_t line = 1, col = 1;
		for (size_t i = 0; i < p_ && i < s_.size(); ++i) {
			if (s_[i] == '\n') {
				++line;
				col = 1;
			} else {
				++col;
			}
		}
		throw Error("JSON line " + std::to_string(line) + ", column " + std::to_string(col) + ": " + what);
	}

	void ws() {
		while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_;
	}

	bool take(char c) {
		ws();
		if (p_ < s_.size() && s_[p_] == c) {
			++p_;
			return true;
		}
		return false;
	}

	void expect(char c) {
		if (!take(c)) fail(std::string("expected '") + c + "'");
	}

	bool word(const char *w) {
		size_t n = std::char_traits<char>::length(w);
		if (s_.compare(p_, n, w) == 0) {
			p_ += n;
			return true;
		}
		return false;
	}

	Value value() {
		ws();
		if (p_ >= s_.size()) fail("unexpected end of text");
		char c = s_[p_];
		if (c == '{') {
			++p_;
			Value v = Value::object();
			if (take('}')) return v;
			do {
				ws();
				if (p_ >= s_.size() || s_[p_] != '"') fail("expected a key string");
				std::string key = string();
				expect(':');
				if (v.has(key)) fail("duplicate key \"" + key + "\"");
				v.members().emplace_back(std::move(key), value());
			} while (take(','));
			expect('}');
			return v;
		}
		if (c == '[') {
			++p_;
			Value v = Value::array();
			if (take(']')) return v;
			do {
				v.push(value());
			} while (take(','));
			expect(']');
			return v;
		}
		if (c == '"') return Value(string());
		if (word("true")) return Value(true);
		if (word("false")) return Value(false);
		if (word("null")) return Value();
		return number();
	}

	uint32_t hex4() {
		if (p_ + 4 > s_.size()) fail("truncated \\u escape");
		uint32_t v = 0;
		for (int i = 0; i < 4; ++i) {
			char h = s_[p_++];
			v <<= 4;
			if (h >= '0' && h <= '9') v |= uint32_t(h - '0');
			else if (h >= 'a' && h <= 'f') v |= uint32_t(h - 'a' + 10);
			else if (h >= 'A' && h <= 'F') v |= uint32_t(h - 'A' + 10);
			else fail("bad \\u escape");
		}
		return v;
	}

	std::string string() {
		++p_;  // opening quote
		std::string out;
		while (true) {
			if (p_ >= s_.size()) fail("unterminated string");
			char c = s_[p_++];
			if (c == '"') return out;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (p_ >= s_.size()) fail("unterminated string");
			char e = s_[p_++];
			switch (e) {
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case '/': out += '/'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u': {
				uint32_t cp = hex4();
				if (cp >= 0xD800 && cp < 0xDC00 && p_ + 1 < s_.size() && s_[p_] == '\\' && s_[p_ + 1] == 'u') {
					p_ += 2;
					uint32_t lo = hex4();
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				append_utf8(out, cp);
				break;
			}
			default: fail("bad escape");
			}
		}
	}

	Value number() {
		size_t start = p_;
		bool real = false;
		if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+')) ++p_;
		while (p_ < s_.size()) {
			char c = s_[p_];
			if (c >= '0' && c <= '9') {
				++p_;
			} else if (c == '.' || c == 'e' || c == 'E' || ((c == '-' || c == '+') && real)) {
				real = true;
				++p_;
			} else {
				break;
			}
		}
		std::string text = s_.substr(start, p_ - start);
		if (text.empty() || text == "-" || text == "+") fail("expected a value");
		char *end = nullptr;
		if (!real) {
			long long v = strtoll(text.c_str(), &end, 10);
			if (*end == 0) return Value(int64_t(v));
		}
		double d = strtod(text.c_str(), &end);
		if (*end != 0) fail("bad number \"" + text + "\"");
		return Value(d);
	}

	const std::string &s_;
	size_t p_ = 0;
};

// True when the value prints on one short line.
bool is_flat(const Value &v) {
	if (v.is_array()) {
		if (v.size() > 16) return false;
		for (auto &e : v.items()) {
			if (e.is_object() && e.size() > 0) return false;
			if (e.is_array() && !is_flat(e)) return false;
		}
		return v.dump().size() < 100;
	}
	return !v.is_object() || v.size() == 0;
}

}  // namespace

bool Value::as_bool() const {
	if (type_ != Bool) wrong(type_, "true or false");
	return int_ != 0;
}

int64_t Value::as_int() const {
	if (type_ == Int) return int_;
	if (type_ == Real && std::floor(real_) == real_) return int64_t(real_);
	wrong(type_, "an integer");
}

double Value::as_real() const {
	if (type_ == Int) return double(int_);
	if (type_ == Real) return real_;
	wrong(type_, "a number");
}

const std::string &Value::as_string() const {
	if (type_ != String) wrong(type_, "a string");
	return str_;
}

std::vector<Value> &Value::items() {
	if (type_ != Array) wrong(type_, "an array");
	return arr_;
}

const std::vector<Value> &Value::items() const {
	if (type_ != Array) wrong(type_, "an array");
	return arr_;
}

std::vector<std::pair<std::string, Value>> &Value::members() {
	if (type_ != Object) wrong(type_, "an object");
	return obj_;
}

const std::vector<std::pair<std::string, Value>> &Value::members() const {
	if (type_ != Object) wrong(type_, "an object");
	return obj_;
}

Value &Value::operator[](const std::string &key) {
	for (auto &m : members()) {
		if (m.first == key) return m.second;
	}
	obj_.emplace_back(key, Value());
	return obj_.back().second;
}

const Value *Value::find(const std::string &key) const {
	if (type_ != Object) return nullptr;
	for (auto &m : obj_) {
		if (m.first == key) return &m.second;
	}
	return nullptr;
}

const Value &Value::at(const std::string &key) const {
	const Value *v = find(key);
	if (!v) throw Error("missing \"" + key + "\"");
	return *v;
}

void Value::dump_to(std::string &out, int indent) const {
	auto newline = [&](int n) {
		out += '\n';
		out.append(size_t(n) * 2, ' ');
	};
	switch (type_) {
	case Null: out += "null"; break;
	case Bool: out += int_ ? "true" : "false"; break;
	case Int: out += std::to_string(int_); break;
	case Real: {
		char buf[40];
		// Enough digits to read back the same double (and so the same float).
		snprintf(buf, sizeof buf, "%.17g", real_);
		std::string s = buf;
		// Prefer the shortest text that reads back the same value.
		for (int prec = 6; prec < 17; ++prec) {
			snprintf(buf, sizeof buf, "%.*g", prec, real_);
			if (strtod(buf, nullptr) == real_) {
				s = buf;
				break;
			}
		}
		if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
		out += s;
		break;
	}
	case String: quote(out, str_); break;
	case Array: {
		if (arr_.empty()) {
			out += "[]";
			break;
		}
		bool flat = true;
		for (auto &e : arr_) flat = flat && is_flat(e);
		std::string one;
		if (flat) {
			one += '[';
			for (size_t i = 0; i < arr_.size(); ++i) {
				if (i) one += ", ";
				arr_[i].dump_to(one, 0);
			}
			one += ']';
		}
		if (flat && one.size() < 100) {
			out += one;
			break;
		}
		out += '[';
		for (size_t i = 0; i < arr_.size(); ++i) {
			if (i) out += ',';
			newline(indent + 1);
			arr_[i].dump_to(out, indent + 1);
		}
		newline(indent);
		out += ']';
		break;
	}
	case Object: {
		if (obj_.empty()) {
			out += "{}";
			break;
		}
		out += '{';
		for (size_t i = 0; i < obj_.size(); ++i) {
			if (i) out += ',';
			newline(indent + 1);
			quote(out, obj_[i].first);
			out += ": ";
			obj_[i].second.dump_to(out, indent + 1);
		}
		newline(indent);
		out += '}';
		break;
	}
	}
}

std::string Value::dump() const {
	std::string out;
	dump_to(out, 0);
	return out;
}

Value Value::parse(const std::string &text) { return Parser(text).document(); }

}  // namespace json
