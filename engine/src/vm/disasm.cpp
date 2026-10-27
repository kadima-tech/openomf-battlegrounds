#include "vm/disasm.h"

#include <cstring>
#include <map>

#include "de/contents.h"

namespace vm {

namespace {

std::map<int32_t, OpInfo> build_table() {
	std::map<int32_t, OpInfo> t;
#define OP(code, is_jump, ...) t[code] = OpInfo{true, is_jump, {__VA_ARGS__}};
#include "vm/opcodes.inc"
#undef OP
	return t;
}

size_t kind_size(OperandKind k) {
	switch (k) {
	case kU8:
	case kI8:
		return 1;
	case kU16:
	case kI16:
	case kU16IfClass:
		return 2;
	default:
		return 4;
	}
}

}  // namespace

const OpInfo &op_info(int32_t opcode) {
	static const auto table = build_table();
	static const OpInfo unknown;
	auto it = table.find(opcode);
	return it == table.end() ? unknown : it->second;
}

Instruction decode_at(std::span<const uint8_t> code, size_t &p) {
	auto read = [&](size_t n) {
		if (p + n > code.size()) {
			throw de::FormatError("VM: operand past end of code at " + std::to_string(p));
		}
		uint32_t v = 0;
		memcpy(&v, code.data() + p, n);
		p += n;
		return v;
	};
	{
		Instruction ins;
		ins.offset = uint32_t(p);
		ins.opcode = int32_t(read(4));
		const OpInfo &info = op_info(ins.opcode);
		if (!info.known) {
			throw de::FormatError("VM: unknown opcode " + std::to_string(ins.opcode) + " at " +
			                      std::to_string(ins.offset));
		}
		for (OperandKind k : info.operands) {
			if (k == kU16IfClass) {
				if (ins.args.empty() || ins.args.back() != 10) {  // only for class-typed params
					continue;
				}
			}
			if (k == kBlob) {
				size_t n = size_t(ins.args.back());
				if (p + n > code.size()) {
					throw de::FormatError("VM: inline data past end of code");
				}
				ins.blob = code.subspan(p, n);
				p += n;
				continue;
			}
			uint32_t v = read(kind_size(k));
			switch (k) {
			case kI8:
				ins.args.push_back(int8_t(v));
				break;
			case kI16:
				ins.args.push_back(int16_t(v));
				break;
			case kI32:
				ins.args.push_back(int32_t(v));
				break;
			default:
				ins.args.push_back(v);
			}
		}
		return ins;
	}
}

std::vector<Instruction> decode(std::span<const uint8_t> code) {
	std::vector<Instruction> out;
	size_t p = 0;
	while (p < code.size()) {
		out.push_back(decode_at(code, p));
	}
	return out;
}

std::string format(const Instruction &ins) {
	std::string s = std::to_string(ins.offset) + ": op" + std::to_string(ins.opcode);
	for (auto a : ins.args) {
		s += " " + std::to_string(a);
	}
	if (!ins.blob.empty()) {
		s += " \"";
		for (auto b : ins.blob) {
			if (b >= 32 && b < 127) {
				s += char(b);
			} else {
				char buf[8];
				snprintf(buf, sizeof buf, "\\x%02x", b);
				s += buf;
			}
		}
		s += "\"";
	}
	return s;
}

}  // namespace vm
