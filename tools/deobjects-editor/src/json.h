// A small JSON value with ordered object keys, a parser and a pretty printer.
// Only what the editor needs: no external dependency, so the tool builds anywhere.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Error : std::runtime_error {
	using std::runtime_error::runtime_error;
};

class Value {
public:
	enum Type { Null, Bool, Int, Real, String, Array, Object };

	Value() = default;
	Value(std::nullptr_t) {}
	Value(bool b) : type_(Bool), int_(b) {}
	Value(int v) : type_(Int), int_(v) {}
	Value(int64_t v) : type_(Int), int_(v) {}
	Value(uint32_t v) : type_(Int), int_(v) {}
	Value(double v) : type_(Real), real_(v) {}
	Value(const char *s) : type_(String), str_(s) {}
	Value(std::string s) : type_(String), str_(std::move(s)) {}

	static Value array() {
		Value v;
		v.type_ = Array;
		return v;
	}
	static Value object() {
		Value v;
		v.type_ = Object;
		return v;
	}

	Type type() const { return type_; }
	bool is_null() const { return type_ == Null; }
	bool is_bool() const { return type_ == Bool; }
	bool is_number() const { return type_ == Int || type_ == Real; }
	bool is_int() const { return type_ == Int; }
	bool is_string() const { return type_ == String; }
	bool is_array() const { return type_ == Array; }
	bool is_object() const { return type_ == Object; }

	bool as_bool() const;
	int64_t as_int() const;
	double as_real() const;
	const std::string &as_string() const;

	// Arrays.
	std::vector<Value> &items();
	const std::vector<Value> &items() const;
	void push(Value v) { items().push_back(std::move(v)); }
	size_t size() const { return type_ == Array ? arr_.size() : type_ == Object ? obj_.size() : 0; }

	// Objects keep their insertion order.
	std::vector<std::pair<std::string, Value>> &members();
	const std::vector<std::pair<std::string, Value>> &members() const;
	Value &operator[](const std::string &key);  // inserts null when missing
	const Value *find(const std::string &key) const;
	bool has(const std::string &key) const { return find(key) != nullptr; }
	// Throws json::Error naming the key when missing.
	const Value &at(const std::string &key) const;

	std::string dump() const;  // pretty-printed, short arrays on one line
	static Value parse(const std::string &text);

private:
	void dump_to(std::string &out, int indent) const;

	Type type_ = Null;
	int64_t int_ = 0;
	double real_ = 0;
	std::string str_;
	std::vector<Value> arr_;
	std::vector<std::pair<std::string, Value>> obj_;
};

}  // namespace json
