#include "Render.h"
#include "D3D11Hook.hpp"
#include "WindowsMessageHook.hpp"
#include "RawInput.h"
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
#include <chrono>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace RadarKeys {
	namespace Render {

		std::unique_ptr<D3D11Hook> d3d11Hook;
		std::unique_ptr<WindowsMessageHook> windowsMessageHook;
		bool d3dHooked = false;

		HWND hwnd = nullptr;
		ID3D11RenderTargetView* mainRenderTargetView = nullptr;
		bool ImGuiInitialized = false;
		bool frameInitialized = false;
		bool firstFrame = true;

		bool IsUnlockCursor() {
			// cursor visibility tied to open menus; computed fresh each frame (no cache).
			return KeyBindMenu::menuOpen || DebuggerMenu::menuOpen;
		}

		void CleanupRenderTarget() {
			spdlog::trace("CleanupRenderTarget");
			auto log = spdlog::get("radarkeys");
			if (log != NULL) {
				log->flush();
			}

			if (mainRenderTargetView != nullptr) {
				mainRenderTargetView->Release();
				mainRenderTargetView = nullptr;
			}
		}

		void CreateRenderTarget() {
			CleanupRenderTarget();

			ID3D11Texture2D* backBuffer{ nullptr };
			if (d3d11Hook->get_swap_chain()->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&backBuffer) == S_OK) {
				d3d11Hook->get_device()->CreateRenderTargetView(backBuffer, NULL, &mainRenderTargetView);
				backBuffer->Release();
			}
		}

		// called by IHHook message hook on game's window-message thread (same as RawInput WM_INPUT)
		bool OnMessage(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {

			if (!frameInitialized) {
				return true;
			}

			// only swallows input if its from the mouse
			if (message == WM_INPUT && (IsUnlockCursor() || showCapturePrompt)) {
				RAWINPUT raw{};
				UINT size = sizeof(RAWINPUT);
				
				// Windows Input API...
				if (GetRawInputData((HRAWINPUT)l_param, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
					if (raw.header.dwType == RIM_TYPEMOUSE) {
						return false; // should only swallow mouse inputs????
					}
				}
			}

			bool handledMessage = !RawInput::OnMessage(wnd, message, w_param, l_param);

			if (IsUnlockCursor() && ImGui_ImplWin32_WndProcHandler(wnd, message, w_param, l_param) != 0) {
				// Block game input when UI active (RE2FW pattern)
				auto& io = ImGui::GetIO();
				if (io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput) {
					handledMessage = true;
				}
			}

			if (handledMessage) {
				// Menu eating input may swallow keyup events for keys held when menu opened.
				if (w_param == WM_KEYUP) {
					return true;
				}
				return false;// eat the message
			}
			return true;
		}

		// called on initialize and on device reset (see OnReset)
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

			// RE2FW: explicitly call the destructor first
			windowsMessageHook.reset();
			windowsMessageHook = std::make_unique<WindowsMessageHook>(hwnd);
			windowsMessageHook->on_message = [](auto wnd, auto msg, auto wParam, auto lParam) {
				return OnMessage(wnd, msg, wParam, lParam);
			};

			spdlog::info("Creating render target");
			CreateRenderTarget();

			spdlog::info("Window Handle: {0:x}", (uintptr_t)hwnd);

			if (!ImGuiInitialized) {
				spdlog::info("Initializing ImGui");
				IMGUI_CHECKVERSION();
				ImGui::CreateContext();
				ImGuiIO& io = ImGui::GetIO(); (void)io;

				spdlog::info("Initializing ImGui Win32");
				if (!ImGui_ImplWin32_Init(hwnd)) {
					spdlog::error("Failed to initialize ImGui.");
					return false;
				}

				spdlog::info("Initializing ImGui D3D11");
				if (!ImGui_ImplDX11_Init(device, context)) {
					spdlog::error("Failed to initialize ImGui.");
					return false;
				}
				ImGuiInitialized = true;
			}

			ImGui::StyleColorsDark();

			if (firstFrame) {
				firstFrame = false;

				RawInput::InitializeInput();

				DebuggerMenu::Init();// must come before KeyBindMenu can log anything useful through it
				KeyBindMenu::Init("F7");// no config file in this build - starting key is just hardcoded here
			}

			return true;
		}

		void DrawUI() {
			LuaBridge::ProcessMessages();

			auto& io = ImGui::GetIO();
			bool unlock = IsUnlockCursor();

			if (io.WantCaptureMouse) {
				RawInput::BlockMouseClick();
			}
			else {
				RawInput::UnBlockMouseClick();
			}

			if (io.WantCaptureKeyboard) {
				RawInput::BlockKeyboard();
			}
			else {
				RawInput::UnBlockKeyboard();
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
			KeyBindMenu::Update();
			auto frameTimeStart = std::chrono::high_resolution_clock::now();

			// frameInitialized resets on device reset; place session‑once init behind firstFrame in FrameInitialize.
			if (!frameInitialized) {
				if (!FrameInitialize()) {
					spdlog::error("Failed to frame initialize RadarKeys");
					return;
				}
				spdlog::info("RadarKeys frame initialized");
				frameInitialized = true;
				return; // give it an extra frame to settle
			}

			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();

			DrawUI();

			ImGui::EndFrame();
			ImGui::Render();

			ID3D11DeviceContext* context = nullptr;
			d3d11Hook->get_device()->GetImmediateContext(&context);
			context->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

			auto frameTimeEnd = std::chrono::high_resolution_clock::now();
			auto frameDuration = std::chrono::duration_cast<std::chrono::microseconds>(frameTimeEnd - frameTimeStart).count();
			(void)frameDuration;
		}

		void OnReset() {
			spdlog::info("OnReset");
			auto log = spdlog::get("radarkeys");
			if (log != NULL) {
				log->flush();
			}

			//RE2FW: crashes if we don't release it at this point
			CleanupRenderTarget();
			frameInitialized = false;

			spdlog::info("OnReset done");
			if (log != NULL) {
				log->flush();
			}
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
