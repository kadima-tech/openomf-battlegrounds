// Instruction decoding for DE VM bytecode: i32 opcode followed by typed operands.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vm {

enum OperandKind : uint8_t { kI32, kU32, kF32, kU8, kI8, kU16, kI16, kBlob, kU16IfClass };

struct OpInfo {
	bool known = false;
	bool jump = false;
	std::vector<OperandKind> operands;
};

const OpInfo &op_info(int32_t opcode);

struct Instruction {
	uint32_t offset;   // byte offset within the method's code
	int32_t opcode;
	std::vector<int64_t> args;  // integer operands (floats bit-cast to their u32 pattern)
	std::span<const uint8_t> blob;  // inline data for opcodes 2/3
};

// Decode one instruction at byte offset p, advancing p past it.
// Throws de::FormatError on an unknown opcode or truncated operand.
Instruction decode_at(std::span<const uint8_t> code, size_t &p);
std::vector<Instruction> decode(std::span<const uint8_t> code);
std::string format(const Instruction &ins);

}  // namespace vm
