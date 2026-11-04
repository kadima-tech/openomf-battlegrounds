#include "vm/memory.h"

#include "de/platform.h"

#include <algorithm>

namespace vm {

Memory::Memory(uint32_t reserve) : reserved_(reserve) {
	void *p = de::reserve_pages(reserve);
	if (!p) {
		throw MemoryError("vm: cannot reserve script address space");
	}
	base_ = static_cast<uint8_t *>(p);
}

Memory::~Memory() { de::release_pages(base_, reserved_); }

uint32_t Memory::alloc(uint32_t size, uint32_t align) {
	uint64_t addr = (uint64_t(top_) + align - 1) & ~uint64_t(align - 1);
	if (addr + size > reserved_) {
		throw MemoryError("vm: script address space exhausted");
	}
	// Only addresses below top_ are ever touched (ptr() checks), so memory is committed as the
	// arena grows. Windows has no overcommit: committing the whole reserve up front would charge
	// 1 GB against the page file.
	if (addr + size > committed_) {
		const uint64_t step = 1u << 20;
		uint64_t end = std::min<uint64_t>((addr + size + step - 1) / step * step, reserved_);
		if (!de::commit_pages(base_ + committed_, size_t(end - committed_))) {
			throw MemoryError("vm: cannot commit script memory");
		}
		committed_ = uint32_t(end);
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
