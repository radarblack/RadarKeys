//D3D11Hook.cpp - from RE2Framework
#include <algorithm>
#include <spdlog/spdlog.h>
#include "D3D11Hook.hpp"

using namespace std;

static const char* LOG_HOOKING_D3D11 = "Hooking D3D11";
static const char* LOG_FAILED_CREATE_DUMMY_WINDOW_D3D11_HOOK = "Failed to create dummy window for D3D11 hook. GetLastError={0:x}";
static const char* LOG_CREATING_DUMMY_D3D11_DEVICE = "Creating dummy D3D11 device.";
static const char* LOG_FAILED_CREATE_DUMMY_D3D11_DEVICE_HRESULT = "Failed to create dummy D3D11 device. HRESULT={0:x} max_feature={1:x}";
static const char* LOG_CREATED_DUMMY_D3D11_DEVICE_HRESULT_0 = "Created dummy D3D11 device. HRESULT={0:x} max_feature={1:x}";
static const char* LOG_RELEASED_DUMMY_D3D11_DEVICE = "Released dummy D3D11 device";

static D3D11Hook* g_d3d11_hook = nullptr;

D3D11Hook::~D3D11Hook() {
    unhook();
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
    m_swap_chain = nullptr;
    g_d3d11_hook = nullptr;
}

bool D3D11Hook::hook() {
    spdlog::info(LOG_HOOKING_D3D11);

    g_d3d11_hook = this;

    HWND h_wnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (h_wnd == nullptr) {
        spdlog::error(LOG_FAILED_CREATE_DUMMY_WINDOW_D3D11_HOOK, GetLastError());
        return false;
    }

    IDXGISwapChain* swap_chain = nullptr;
    ID3D11Device* device = nullptr;
    D3D_FEATURE_LEVEL device_max_feature_level = D3D_FEATURE_LEVEL_9_1;
    ID3D11DeviceContext* context = nullptr;

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    DXGI_SWAP_CHAIN_DESC swap_chain_desc;

    ZeroMemory(&swap_chain_desc, sizeof(swap_chain_desc));

    swap_chain_desc.BufferCount = 1;
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.OutputWindow = h_wnd;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    swap_chain_desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    spdlog::info(LOG_CREATING_DUMMY_D3D11_DEVICE);
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_NULL, nullptr, 0, &feature_level, 1, D3D11_SDK_VERSION, &swap_chain_desc, &swap_chain, &device, &device_max_feature_level, &context);
    if (FAILED(hr)) {  
        spdlog::error(LOG_FAILED_CREATE_DUMMY_D3D11_DEVICE_HRESULT, hr, device_max_feature_level);
        DestroyWindow(h_wnd);
        return false;
    }
    spdlog::info(LOG_CREATED_DUMMY_D3D11_DEVICE_HRESULT_0, hr, device_max_feature_level);

    auto present_fn = (*(uintptr_t**)swap_chain)[8];
    auto resize_buffers_fn = (*(uintptr_t**)swap_chain)[13];
    m_present_hook = std::make_unique<FunctionHook>(present_fn, (uintptr_t)&D3D11Hook::present);
    m_resize_buffers_hook = std::make_unique<FunctionHook>(resize_buffers_fn, (uintptr_t)&D3D11Hook::resize_buffers);

    device->Release();
    context->Release();
    swap_chain->Release();
    DestroyWindow(h_wnd);
    spdlog::info(LOG_RELEASED_DUMMY_D3D11_DEVICE);

    m_hooked = m_present_hook->create() && m_resize_buffers_hook->create();

    return m_hooked;
}

bool D3D11Hook::unhook() {
    bool ok = true;

    if (m_present_hook) {
        ok = m_present_hook->remove() && ok;
    }
    if (m_resize_buffers_hook) {
        ok = m_resize_buffers_hook->remove() && ok;
    }

    m_hooked = false;
    if (g_d3d11_hook == this) {
        g_d3d11_hook = nullptr;
    }
    return ok;
}

HRESULT WINAPI D3D11Hook::present(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    auto d3d11 = g_d3d11_hook;

    if (!d3d11 || !d3d11->m_present_hook) {
        return swap_chain->Present(sync_interval, flags);
    }

    d3d11->m_swap_chain = swap_chain;

    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
        if (d3d11->m_device != device) {
            if (d3d11->m_device) {
                d3d11->m_device->Release();
            }
            d3d11->m_device = device;
        }
        else {
            device->Release();
        }
    }

    if (d3d11->m_on_present) {
        d3d11->m_on_present(*d3d11);
    }

    auto present_fn = d3d11->m_present_hook->get_original<decltype(D3D11Hook::present)>();

    return present_fn(swap_chain, sync_interval, flags);
}

HRESULT WINAPI D3D11Hook::resize_buffers(IDXGISwapChain* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags) {
    auto d3d11 = g_d3d11_hook;

    if (!d3d11 || !d3d11->m_resize_buffers_hook) {
        return swap_chain->ResizeBuffers(buffer_count, width, height, new_format, swap_chain_flags);
    }

    if (d3d11->m_on_resize_buffers) {
        d3d11->m_on_resize_buffers(*d3d11);
    }

    auto resize_buffers_fn = d3d11->m_resize_buffers_hook->get_original<decltype(D3D11Hook::resize_buffers)>();

    return resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
}
