// Script-visible 32-bit address space.
//
// Compiled DE bytecode assumes 4-byte pointers: frames, members and native structs it peeks
// into (op 47/50 read through pointer + byte offset) are laid out for a 32-bit build. On a
// 64-bit host every script-visible byte therefore lives in this arena and "pointers" stored
// in slots are arena addresses. Address 0 is never handed out, so it stays the null pointer.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace vm {

struct MemoryError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

class Memory {
public:
	explicit Memory(uint32_t reserve = 0xf0000000u);  // reserved lazily; committed on touch
	~Memory();
	Memory(const Memory &) = delete;
	Memory &operator=(const Memory &) = delete;

	uint32_t alloc(uint32_t size, uint32_t align = 8);
	uint32_t alloc_copy(const void *data, uint32_t size);
	// A block nothing refers to any more; the next alloc of that size reuses it.
	void release(uint32_t addr, uint32_t size);
	uint32_t used() const { return top_; }

	uint8_t *ptr(uint32_t addr, uint32_t size = 1) {
		if (addr == 0 || uint64_t(addr) + size > top_) {
			throw MemoryError("vm: bad address " + std::to_string(addr));
		}
		return base_ + addr;
	}
	template <typename T>
	T get(uint32_t addr) {
		T v;
		memcpy(&v, ptr(addr, sizeof(T)), sizeof(T));
		return v;
	}
	template <typename T>
	void set(uint32_t addr, T v) {
		memcpy(ptr(addr, sizeof(T)), &v, sizeof(T));
	}

private:
	uint8_t *base_ = nullptr;
	uint32_t reserved_;
	uint32_t committed_ = 0;
	std::map<uint32_t, std::vector<uint32_t>> released_;  // by size
	uint32_t top_ = 16;  // keep low addresses unused so small offsets never look valid
};

}  // namespace vm
