#include <spdlog/spdlog.h>
#include <MinHook.h>
#include "FunctionHook.hpp"

using namespace std;

bool g_isMinHookInitialized{ false };

FunctionHook::FunctionHook(Address target, Address destination)
    : m_target{ 0 },
    m_destination{ 0 },
    m_original{ 0 }
{
    spdlog::info("Attempting to hook {:p}->{:p}", target.ptr(), destination.ptr());

    if (!g_isMinHookInitialized && MH_Initialize() == MH_OK) {
        g_isMinHookInitialized = true;
    }
    
    if (MH_CreateHook(target.as<LPVOID>(), destination.as<LPVOID>(), (LPVOID*)&m_original) == MH_OK) {
        m_target = target;
        m_destination = destination;

        spdlog::info("Hook init successful {:p}->{:p}", target.ptr(), destination.ptr());
    }
    else {
        spdlog::error("Failed to hook {:p}", target.ptr());
    }
}

FunctionHook::~FunctionHook() {
    remove();
}

bool FunctionHook::create() {
    if (m_target == 0 || m_destination == 0 || m_original == 0) {
        spdlog::error("FunctionHook not initialized");
        return false;
    }

    const auto target = m_target;
    const auto destination = m_destination;

    if (MH_EnableHook((LPVOID)target) != MH_OK) {
        m_original = 0;
        m_destination = 0;
        m_target = 0;

        spdlog::error("Failed to enable hook {:x}->{:x}", target, destination);
        return false;
    }

    spdlog::info("Hooked {:x}->{:x}", m_target, m_destination);
    return true;
}

bool FunctionHook::remove() {
    if (m_original == 0) {
        return true;
    }

    if (MH_DisableHook((LPVOID)m_target) != MH_OK ||
        MH_RemoveHook((LPVOID)m_target) != MH_OK) {
        return false;
    }
    m_target = 0;
    m_destination = 0;
    m_original = 0;

    return true;
}
