#pragma once

#include <Windows.h>
#include <cstdint>

namespace RadarKeys {

	inline uintptr_t GetExeBase() {
		return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	}

	constexpr uintptr_t EXE_PREFERRED_BASE = 0x140000000ull;

	inline constexpr uintptr_t ToRva(uintptr_t absAddr) {
		return absAddr - EXE_PREFERRED_BASE;
	}

	inline void* ResolveGameAddress(uintptr_t absAddr) {
		if (absAddr == 0) {
			return nullptr;
		}
		const uintptr_t base = GetExeBase();
		if (!base) {
			return nullptr;
		}
		return reinterpret_cast<void*>(base + ToRva(absAddr));
	}

}
