#include "vm/memory.h"

#include <sys/mman.h>

namespace vm {

Memory::Memory(uint32_t reserve) : reserved_(reserve) {
	void *p = mmap(nullptr, reserve, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED) {
		throw MemoryError("vm: cannot reserve script address space");
	}
	base_ = static_cast<uint8_t *>(p);
}

Memory::~Memory() { munmap(base_, reserved_); }

uint32_t Memory::alloc(uint32_t size, uint32_t align) {
	uint64_t addr = (uint64_t(top_) + align - 1) & ~uint64_t(align - 1);
	if (addr + size > reserved_) {
		throw MemoryError("vm: script address space exhausted");
	}
	top_ = uint32_t(addr + size);
	memset(base_ + addr, 0, size);
	return uint32_t(addr);
}

uint32_t Memory::alloc_copy(const void *data, uint32_t size) {
	uint32_t a = alloc(size ? size : 1);
	memcpy(base_ + a, data, size);
	return a;
}

}  // namespace vm
