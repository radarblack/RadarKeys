#include "Render.h"
#include "D3D11Hook.hpp"
#include "WindowsMessageHook.hpp"
#include "RawInput.h"
#include "DirectInputHook.h"
#include "LuaKeyState.h"
#include "LuaBridge.h"
#include "KeyBindMenu.h"
#include "DebuggerMenu.h"
#include "spdlog/spdlog.h"

#include <imgui/imgui.h>
#include "imguiimpl/imgui_impl_win32.h"
#include "imguiimpl/imgui_impl_dx11.h"

#include <memory>
#include <filesystem>
#include <string>
#include <vector>
#include <dxgi1_4.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
namespace RadarKeys {
	namespace Render {

		std::unique_ptr<D3D11Hook> d3d11Hook;
		std::unique_ptr<WindowsMessageHook> windowsMessageHook;
		bool d3dHooked = false;

		HWND hwnd = nullptr;
		std::vector<ID3D11RenderTargetView*> backBufferRTVs;
		IDXGISwapChain* rtvSwapChain = nullptr;
		bool ImGuiInitialized = false;
		bool frameInitialized = false;
		bool firstFrame = true;

		bool IsUnlockCursor() {
			return KeyBindMenu::menuOpen || DebuggerMenu::menuOpen;
		}

		void CleanupRenderTarget() {
			spdlog::trace("CleanupRenderTarget");

			for (ID3D11RenderTargetView* rtv : backBufferRTVs) {
				if (rtv != nullptr) {
					rtv->Release();
				}
			}
			backBufferRTVs.clear();
			rtvSwapChain = nullptr;
		}

		ID3D11RenderTargetView* EnsureRenderTarget() {
			if (!d3d11Hook || !d3d11Hook->get_swap_chain() || !d3d11Hook->get_device()) {
				return nullptr;
			}

			IDXGISwapChain* swapChain = d3d11Hook->get_swap_chain();
			if (swapChain != rtvSwapChain) {
				CleanupRenderTarget();
				rtvSwapChain = swapChain;
			}

			UINT bufferCount = 1;
			DXGI_SWAP_CHAIN_DESC swapDesc{};
			if (SUCCEEDED(swapChain->GetDesc(&swapDesc)) && swapDesc.BufferCount > 0) {
				bufferCount = swapDesc.BufferCount;
				if (bufferCount > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) {
					bufferCount = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
				}
			}

			UINT bufferIndex = 0;
			IDXGISwapChain3* swapChain3 = nullptr;
			if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapChain3))) && swapChain3 != nullptr) {
				bufferIndex = swapChain3->GetCurrentBackBufferIndex();
				swapChain3->Release();
				if (bufferIndex >= bufferCount) {
					bufferIndex = 0;
				}
			}

			if (backBufferRTVs.size() != bufferCount) {
				CleanupRenderTarget();
				rtvSwapChain = swapChain;
				backBufferRTVs.assign(bufferCount, nullptr);
			}

			if (backBufferRTVs[bufferIndex] == nullptr) {
				ID3D11Texture2D* backBuffer{ nullptr };
				HRESULT hr = swapChain->GetBuffer(bufferIndex, __uuidof(ID3D11Texture2D), reinterpret_cast<LPVOID*>(&backBuffer));
				if (FAILED(hr) || backBuffer == nullptr) {
					spdlog::warn("EnsureRenderTarget: GetBuffer({}) failed: 0x{:08X}", bufferIndex, static_cast<unsigned>(hr));
					return nullptr;
				}

				hr = d3d11Hook->get_device()->CreateRenderTargetView(backBuffer, nullptr, &backBufferRTVs[bufferIndex]);
				backBuffer->Release();
				if (FAILED(hr)) {
					backBufferRTVs[bufferIndex] = nullptr;
					spdlog::warn("EnsureRenderTarget: CreateRenderTargetView failed: 0x{:08X}", static_cast<unsigned>(hr));
					return nullptr;
				}
			}

			return backBufferRTVs[bufferIndex];
		}

		bool OnMessage(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
			if (message == WM_KILLFOCUS ||
				(message == WM_ACTIVATE && LOWORD(w_param) == WA_INACTIVE) ||
				(message == WM_ACTIVATEAPP && w_param == 0)) {
				RawInput::OnFocusLost();
				LuaKeyState::OnFocusLost();
				return true;
			}

			if (!frameInitialized) {
				return true;
			}

			bool mouseRawProcessedHere = false;
			if (message == WM_INPUT) {
				RAWINPUT raw{};
				UINT size = sizeof(RAWINPUT);

				// Windows Input API...
				if (GetRawInputData((HRAWINPUT)l_param, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
					if (raw.header.dwType == RIM_TYPEMOUSE) {
						RawInput::ProcessMouseButtons(&raw);
						mouseRawProcessedHere = true;
						if (RawInput::IsMouseBlockedToGame()) {
							return false;
						}
					}
				}
			}

			bool handledMessage = false;
			if (!mouseRawProcessedHere) {
				handledMessage = !RawInput::OnMessage(wnd, message, w_param, l_param);
			}

			if (IsUnlockCursor() && ImGui_ImplWin32_WndProcHandler(wnd, message, w_param, l_param) != 0) {
				auto& io = ImGui::GetIO();
				if (io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput) {
					handledMessage = true;
				}
			}

			if (RawInput::IsKeyboardBlockedToGame() &&
				message >= WM_KEYFIRST && message <= WM_KEYLAST &&
				w_param != VK_ESCAPE) {
				handledMessage = true;
			}
			if (RawInput::IsMouseBlockedToGame() &&
				message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
				handledMessage = true;
			}

			if (handledMessage) {
				if (message == WM_KEYUP || message == WM_SYSKEYUP ||
					message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
					message == WM_MBUTTONUP || message == WM_XBUTTONUP) {
					return true;
				}
				return false;
			}
			return true;
		}

		bool FrameInitialize() {
			if (frameInitialized) {
				return true;
			}

			spdlog::info("Attempting to frame initialize");

			auto device = d3d11Hook->get_device();
			auto swapChain = d3d11Hook->get_swap_chain();

			if (device == nullptr || swapChain == nullptr) {
				spdlog::info("Device or SwapChain null. DirectX 12 may be in use. A crash may occur.");
				return false;
			}

			ID3D11DeviceContext* context = nullptr;
			device->GetImmediateContext(&context);

			DXGI_SWAP_CHAIN_DESC swapDesc{};
			swapChain->GetDesc(&swapDesc);
			hwnd = swapDesc.OutputWindow;
			if (hwnd == nullptr) {
				spdlog::warn("FrameInitialize: swap chain has no output window - input hook skipped");
			}
			windowsMessageHook.reset();
			windowsMessageHook = std::make_unique<WindowsMessageHook>(hwnd);
			windowsMessageHook->on_message = [](auto wnd, auto msg, auto wParam, auto lParam) {
				return OnMessage(wnd, msg, wParam, lParam);
			};

			spdlog::info("Creating render target");
			EnsureRenderTarget();

			spdlog::info("Window Handle: {0:x}", (uintptr_t)hwnd);

			if (!ImGuiInitialized) {
				spdlog::info("Initializing ImGui");
				IMGUI_CHECKVERSION();
				ImGui::CreateContext();
				ImGuiIO& io = ImGui::GetIO(); (void)io;

				spdlog::info("Initializing ImGui Win32");
				if (!ImGui_ImplWin32_Init(hwnd)) {
					spdlog::error("Failed to initialize ImGui.");
					context->Release();
					return false;
				}

				spdlog::info("Initializing ImGui D3D11");
				if (!ImGui_ImplDX11_Init(device, context)) {
					spdlog::error("Failed to initialize ImGui.");
					context->Release();
					return false;
				}
				ImGuiInitialized = true;
			}
			context->Release();

			ImGui::StyleColorsDark();

			if (firstFrame) {
				firstFrame = false;

				RawInput::InitializeInput();

				DebuggerMenu::Init();
				KeyBindMenu::Init("F7");
			}

			return true;
		}

		void DrawUI() {
			LuaBridge::ProcessMessages();

			auto& io = ImGui::GetIO();
			bool unlock = IsUnlockCursor();
			const bool blockMouse = unlock && KeyBindMenu::captureSuppressMouse;
			const bool blockKeyboard = unlock && KeyBindMenu::captureSuppressKeyboard;
			const bool blockGamepad = unlock && KeyBindMenu::captureSuppressGamepad;

			if (blockMouse) {
				RawInput::BlockMouseClick();
			}
			else {
				RawInput::UnBlockMouseClick();
			}

			if (blockKeyboard) {
				RawInput::BlockKeyboard();
			}
			else {
				RawInput::UnBlockKeyboard();
			}

			RawInput::SetGamepadBlockedToGame(blockGamepad);

			static bool lastBlockKeyboard = false;
			static bool lastBlockMouse = false;
			static bool lastBlockGamepad = false;
			static bool lastUnlock = false;
			if (unlock != lastUnlock) {
				spdlog::info("Input unlock: {} (mainMenu={}, debugger={})",
					unlock ? "ON" : "OFF", KeyBindMenu::menuOpen, DebuggerMenu::menuOpen);
				lastUnlock = unlock;
			}
			if (blockKeyboard != lastBlockKeyboard) {
				spdlog::info("Keyboard block-to-game: {} (unlock={}, captureSuppressKeyboard={}, mainMenu={}, debugger={})",
					blockKeyboard ? "ON" : "OFF", unlock, KeyBindMenu::captureSuppressKeyboard,
					KeyBindMenu::menuOpen, DebuggerMenu::menuOpen);
				lastBlockKeyboard = blockKeyboard;
			}
			if (blockMouse != lastBlockMouse) {
				spdlog::info("Mouse block-to-game: {} (unlock={}, captureSuppressMouse={}, mainMenu={}, debugger={})",
					blockMouse ? "ON" : "OFF", unlock, KeyBindMenu::captureSuppressMouse,
					KeyBindMenu::menuOpen, DebuggerMenu::menuOpen);
				lastBlockMouse = blockMouse;
			}
			if (blockGamepad != lastBlockGamepad) {
				spdlog::info("Gamepad block-to-game: {} (unlock={}, captureSuppressGamepad={}, mainMenu={}, debugger={})",
					blockGamepad ? "ON" : "OFF", unlock, KeyBindMenu::captureSuppressGamepad,
					KeyBindMenu::menuOpen, DebuggerMenu::menuOpen);
				lastBlockGamepad = blockGamepad;
			}

			io.MouseDrawCursor = unlock;

			if (KeyBindMenu::menuOpen) {
				KeyBindMenu::Draw(&KeyBindMenu::menuOpen);
			}

			if (DebuggerMenu::menuOpen) {
				DebuggerMenu::Draw(&DebuggerMenu::menuOpen);
			}
		}

		void OnFrame() {
			if (!frameInitialized) {
				if (!FrameInitialize()) {
					spdlog::error("Failed to frame initialize RadarKeys");
					return;
				}
				spdlog::info("RadarKeys frame initialized");
				frameInitialized = true;
				return;
			}

			DirectInputHook::Poll(hwnd);
			RawInput::PollGamepad();
			RawInput::PollPlaystation();
			KeyBindMenu::Update();

			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();

			DrawUI();

			ImGui::EndFrame();
			ImGui::Render();

			ID3D11RenderTargetView* frameRTV = EnsureRenderTarget();
			if (frameRTV == nullptr) {
				return;
			}

			ID3D11DeviceContext* context = nullptr;
			d3d11Hook->get_device()->GetImmediateContext(&context);
			ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			ID3D11DepthStencilView* savedDSV = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);

			context->OMSetRenderTargets(1, &frameRTV, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
			for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
				if (savedRTVs[i] != nullptr) {
					savedRTVs[i]->Release();
				}
			}
			if (savedDSV != nullptr) {
				savedDSV->Release();
			}
			context->Release();
		}

		void OnReset() {
			spdlog::info("OnReset");

			CleanupRenderTarget();
			frameInitialized = false;

			spdlog::info("OnReset done");
		}

		void CreateD3DHook() {
			d3d11Hook = std::make_unique<D3D11Hook>();
			d3d11Hook->on_present([](D3D11Hook& hook) { OnFrame(); });
			d3d11Hook->on_resize_buffers([](D3D11Hook& hook) { OnReset(); });

			d3dHooked = d3d11Hook->hook();
			if (d3dHooked) {
				spdlog::info("Hooked D3D11");
			}
			else {
				std::wstring title = L"MGSTPP - RadarKeys";
				std::wstring message =
					L"ERROR: Could not hook D3D11\n"
					L"See radarkeys_log.txt in MGS_TPP folder for details.\n";
				MessageBox(NULL, message.c_str(), title.c_str(), NULL);
			}
		}
	}
}
