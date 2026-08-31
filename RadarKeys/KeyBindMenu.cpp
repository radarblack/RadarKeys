#include "KeyBindMenu.h"
#include "RawInput.h"
#include "LuaBridge.h"
#include "DebuggerMenu.h"
#include "Util.h"
#include "HookUtils.h"
#include "spdlog/spdlog.h"
#include "imgui/imgui.h"

#include <fstream>
#include <filesystem>
#include <map>
#include <chrono>
#include <cstdio>

namespace RadarKeys {
	namespace KeyBindMenu {

		std::vector<KeyBind> bindings;
		bool showCapturePrompt = false; 
		static bool isAssigningMenuToggleKey = false; 

		const std::string& GetBindsFileName() {
			static std::string cached;
			if (cached.empty()) {
				cached = (std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys" / "radar_keybinds.conf").string();
			}
			return cached;
		}

		// Modifiers as checkboxes; dropdown shows persistable base keys only.
		struct VkNameEntry { const char* name; USHORT vKey; };
		const VkNameEntry vkNameTable[] = {
			{"A", 'A'}, {"B", 'B'}, {"C", 'C'}, {"D", 'D'}, {"E", 'E'}, {"F", 'F'},
			{"G", 'G'}, {"H", 'H'}, {"I", 'I'}, {"J", 'J'}, {"K", 'K'}, {"L", 'L'},
			{"M", 'M'}, {"N", 'N'}, {"O", 'O'}, {"P", 'P'}, {"Q", 'Q'}, {"R", 'R'},
			{"S", 'S'}, {"T", 'T'}, {"U", 'U'}, {"V", 'V'}, {"W", 'W'}, {"X", 'X'},
			{"Y", 'Y'}, {"Z", 'Z'},
			{"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'},
			{"5", '5'}, {"6", '6'}, {"7", '7'}, {"8", '8'}, {"9", '9'},
			{"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
			{"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
			{"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
			{"Space", VK_SPACE}, {"Tab", VK_TAB}, {"Enter", VK_RETURN},
			{"Insert", VK_INSERT}, {"Delete", VK_DELETE},
			{"Home", VK_HOME}, {"End", VK_END},
			{"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT},
			{"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
			{"Numpad0", VK_NUMPAD0}, {"Numpad1", VK_NUMPAD1}, {"Numpad2", VK_NUMPAD2},
			{"Numpad3", VK_NUMPAD3}, {"Numpad4", VK_NUMPAD4}, {"Numpad5", VK_NUMPAD5},
			{"Numpad6", VK_NUMPAD6}, {"Numpad7", VK_NUMPAD7}, {"Numpad8", VK_NUMPAD8},
			{"Numpad9", VK_NUMPAD9},
			{",", VK_OEM_COMMA}, {".", VK_OEM_PERIOD},
		};
		const int vkNameTableCount = sizeof(vkNameTable) / sizeof(vkNameTable[0]);

		std::string NameForVKey(USHORT vKey) {
			for (int i = 0; i < vkNameTableCount; i++) {
				if (vkNameTable[i].vKey == vKey) {
					return vkNameTable[i].name;
				}
			}
			return "Unknown(" + std::to_string(vKey) + ")";
		}

		// returns -1 if not found in the table
		int VKeyForName(const std::string& name) {
			for (int i = 0; i < vkNameTableCount; i++) {
				if (name == vkNameTable[i].name) {
					return vkNameTable[i].vKey;
				}
			}
			return -1;
		}

		std::string CombinedDisplayName(const KeyBind& bind) {
			std::string result;
			if (bind.needCtrl) result += "Ctrl+";
			if (bind.needShift) result += "Shift+";
			if (bind.needAlt) result += "Alt+";
			result += bind.keyName;
			
			if (bind.mode == BindMode::TOGGLE) {
				result += " [Toggle Switch]";
			} else if (bind.holdSeconds > 0.0f) {
				char buf[32];
				snprintf(buf, sizeof(buf), " (hold %.1fs)", bind.holdSeconds);
				result += buf;
			}
			return result;
		}

		std::string ResolveScriptPath(const std::string& typedPath) {
			std::filesystem::path typed(typedPath);
			if (typed.is_absolute()) {
				return typedPath;
			}

			std::filesystem::path relativePart;
			if (typedPath.find('/') != std::string::npos || typedPath.find('\\') != std::string::npos) {
				relativePart = typedPath;
			}
			else {
				relativePart = std::filesystem::path("mod") / "modules" / typedPath;
			}

			std::filesystem::path fullPath = std::filesystem::path(GetGameDirectory()) / relativePart;
			return fullPath.string();
		}

		// Menu-toggle key: plain key only (no modifiers) for  access; persisted via LoadBindings/SaveBindings. default F7.
		USHORT menuToggleVKey = VK_F7;
		RawInput::ActionHandle menuToggleHandle = 0;
		bool menuOpen = false;

		// share one RawInput action per vKey to let OnBoundKeyPressed see all bindings and fall back to plain key when modifiers don't match.
		std::map<USHORT, RawInput::ActionHandle> vKeyDispatchers;

		// RawInput action wired to whatever menuToggleVKey currently is - see RegisterMenuToggleKey
		void OnMenuToggleKeyPressed(RawInput::BUTTONEVENT buttonEvent) {
			if (buttonEvent != RawInput::BUTTONEVENT::ONDOWN) {
				return;
			}
			
			// safety gate. so that you can't close the menu when the prompt is up.
			if (showCapturePrompt) {
				return;
			}

			menuOpen = !menuOpen;
		}

		void RegisterMenuToggleKey(USHORT vKey) {
			menuToggleVKey = vKey;
			menuToggleHandle = RawInput::RegisterAction(vKey, OnMenuToggleKeyPressed);
		}

		// reserved regardless of modifiers for IH commands and other default keys
		bool IsReservedVKey(USHORT vKey) {
			return vKey == VK_F2 || vKey == VK_F3 || vKey == VK_ESCAPE || vKey == menuToggleVKey;
		}

		// binding free if exact (vKey,Ctrl,Shift,Alt) tuple unused; modifiers create separate bindings.
		// updated to allow a Tap and a Hold configuration on the same key combo
		bool IsComboAvailable(USHORT vKey, bool needCtrl, bool needShift, bool needAlt, float holdSeconds) {
			if (IsReservedVKey(vKey)) {
				return false;
			}
			for (const KeyBind& bind : bindings) {
				if (bind.vKey == vKey && 
            		bind.needCtrl == needCtrl && 
            		bind.needShift == needShift && 
            		bind.needAlt == needAlt) {
            
            		if (bind.holdSeconds == holdSeconds) {
                		return false;
            		}
            
            		if ((holdSeconds <= 0.0f && bind.holdSeconds <= 0.0f) || 
                		(holdSeconds > 0.0f && bind.holdSeconds > 0.0f)) {
                			return false;
            		}
		        }
			}
			return true;
		}

		struct HoldTrack {
			std::chrono::steady_clock::time_point startTime;
			bool fired = false;
		};
		std::map<USHORT, HoldTrack> holdTracks;

		const KeyBind* FindMatchingBinding(USHORT vKey, bool ctrlHeld, bool shiftHeld, bool altHeld) {
			const KeyBind* exactMatch = nullptr;
			const KeyBind* fallbackMatch = nullptr; // fallback if there is no assigned script to the key combo

			for (const KeyBind& bind : bindings) {
				if (bind.vKey != vKey) {
					continue;
				}

				if (bind.needCtrl == ctrlHeld && bind.needShift == shiftHeld && bind.needAlt == altHeld) {
					exactMatch = &bind;
					break;
				}

				if (!bind.needCtrl && !bind.needShift && !bind.needAlt) {
					fallbackMatch = &bind;
				}
			}

			// return the precise modifier profile if found; otherwise drop down to your plain key bind seamlessly
			return exactMatch != nullptr ? exactMatch : fallbackMatch;
		}

		void FireBinding(const KeyBind& bind) {
			if (bind.mode == BindMode::TOGGLE) {
				std::string targetScript = bind.currentToggleState ? bind.toggleOffScriptPath : bind.toggleOnScriptPath;
				std::string stateString = bind.currentToggleState ? "SWITCHED OFF" : "SWITCHED ON";
				std::string filename = std::filesystem::path(targetScript).filename().string();

				std::string logMsg = "[Toggle] " + CombinedDisplayName(bind) + " -> " + stateString + " (Running: " + filename + ")";
				DebuggerMenu::LogButtonPress(logMsg);

				if (DebuggerMenu::LogScriptAttempt(targetScript)) {
					LuaBridge::QueueMessageIn("DoScript|dofile([[" + targetScript + "]])");
				}
				
				for (auto& b : bindings) {
					if (b.vKey == bind.vKey && b.needCtrl == bind.needCtrl && b.needShift == bind.needShift && b.needAlt == bind.needAlt) {
						b.currentToggleState = !b.currentToggleState;
						break;
					}
				}
			} else {
				if (DebuggerMenu::LogScriptAttempt(bind.scriptPath)) {
					LuaBridge::QueueMessageIn("DoScript|dofile([[" + bind.scriptPath + "]])");
				}
			}
		}

		void OnBoundKeyPressed(USHORT vKey, RawInput::BUTTONEVENT buttonEvent) {
			// prevents 'tap' scripts from running when the input detection prompt is up
			if (showCapturePrompt) {
				return;
			}

			bool ctrlHeld = RawInput::IsKeyHeldReal(VK_CONTROL);
			bool shiftHeld = RawInput::IsKeyHeldReal(VK_SHIFT);
			bool altHeld = RawInput::IsKeyHeldReal(VK_MENU);

			const KeyBind* toRun = FindMatchingBinding(vKey, ctrlHeld, shiftHeld, altHeld);

			if (buttonEvent == RawInput::BUTTONEVENT::ONUP) {
				// the key was released. Check if we were tracking a hold for this key.
				auto it = holdTracks.find(vKey);
				if (it != holdTracks.end()) {
					// if it hasn't fired the hold script yet, it means it's a short tap!
					if (!it->second.fired) {
						// look for a structural 'instant' (holdSeconds == 0) fallback binding on the same key combo
						const KeyBind* tapBind = FindMatchingBinding(vKey, ctrlHeld, shiftHeld, altHeld);
						if (tapBind && tapBind->holdSeconds <= 0.0f) {
							DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " tapped cleanly (Hold bypassed)");
							FireBinding(*tapBind);
						}
					}
					holdTracks.erase(it);
				}
				return;
			}

			if (buttonEvent != RawInput::BUTTONEVENT::ONDOWN) {
				return;
			}

			// log raw down event
			std::string pressedName;
			if (ctrlHeld) pressedName += "Ctrl+";
			if (shiftHeld) pressedName += "Shift+";
			if (altHeld) pressedName += "Alt+";
			pressedName += NameForVKey(vKey);
			DebuggerMenu::LogButtonPress(pressedName + " pressed");

			if (toRun == nullptr) {
				return;
			}

			// look to see if there is ANY binding on this virtual key setup that uses a hold delay
			bool hasHoldOptionOnKey = false;
			for (const KeyBind& bind : bindings) {
				if (bind.vKey == vKey && bind.holdSeconds > 0.0f) {
					hasHoldOptionOnKey = true;
					break;
				}
			}

			if (toRun->holdSeconds <= 0.0f) {
				// if there's no hold profile registered anywhere on this key, fire instantly like normal
				if (!hasHoldOptionOnKey) {
					FireBinding(*toRun);
				}
				else {
					// if the user lets go early, OnBoundKeyPressed's ONUP section above catches it as a tap.
					HoldTrack track;
					track.startTime = std::chrono::steady_clock::now();
					track.fired = false;
					holdTracks[vKey] = track;
				}
			}
			else {
				// this is explicitly a hold binding. Start the clock timer ticker.
				HoldTrack track;
				track.startTime = std::chrono::steady_clock::now();
				track.fired = false;
				holdTracks[vKey] = track;
			}
		}

		void Update() {
			if (showCapturePrompt) {
				return;
			}

			for (auto it = holdTracks.begin(); it != holdTracks.end(); ) {
				USHORT vKey = it->first;
				HoldTrack& track = it->second;

				if (track.fired || !RawInput::IsKeyHeldReal(vKey)) {
					it = holdTracks.erase(it);
					continue;
				}

				bool ctrlHeld = RawInput::IsKeyHeldReal(VK_CONTROL);
				bool shiftHeld = RawInput::IsKeyHeldReal(VK_SHIFT);
				bool altHeld = RawInput::IsKeyHeldReal(VK_MENU);
				
				// look specifically for the binding configured with a hold delay
				const KeyBind* holdBind = nullptr;
				for (const KeyBind& bind : bindings) {
					if (bind.vKey == vKey && bind.needCtrl == ctrlHeld && bind.needShift == shiftHeld && bind.needAlt == altHeld && bind.holdSeconds > 0.0f) {
						holdBind = &bind;
						break;
					}
				}

				if (holdBind != nullptr) {
					float elapsedSeconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - track.startTime).count();
					if (elapsedSeconds >= holdBind->holdSeconds) {
						DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " held past threshold " + std::to_string(holdBind->holdSeconds) + "s");
						FireBinding(*holdBind);
						track.fired = true;
					}
				}
				++it;
			}
		}

		// registers shared vKey dispatcher; safe to call repeatedly per binding.
		void EnsureDispatcherRegistered(USHORT vKey) {
			if (vKeyDispatchers.find(vKey) != vKeyDispatchers.end()) {
				return;
			}
			RawInput::ActionHandle handle = RawInput::RegisterAction(vKey, [vKey](RawInput::BUTTONEVENT buttonEvent) {
				OnBoundKeyPressed(vKey, buttonEvent);
			});
			vKeyDispatchers[vKey] = handle;
		}

		// unregisters vKey dispatcher only when no bindings remain; other modifier combos may still need it.
		void RemoveDispatcherIfUnused(USHORT vKey) {
			for (const KeyBind& bind : bindings) {
				if (bind.vKey == vKey) {
					return; // still in use by another binding
				}
			}
			auto it = vKeyDispatchers.find(vKey);
			if (it != vKeyDispatchers.end()) {
				RawInput::UnRegisterAction(vKey, it->second);
				vKeyDispatchers.erase(it);
			}
		}

		void SaveBindings() {
			std::error_code ec;
			std::filesystem::path bindsDir = std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys";
			std::filesystem::create_directories(bindsDir, ec);

			std::ofstream outFile(GetBindsFileName());
			if (!outFile) return;

			outFile << "MENUKEY|" << NameForVKey(menuToggleVKey) << "\n";
			for (const KeyBind& bind : bindings) {
				int modeInt = static_cast<int>(bind.mode);
				outFile << "BIND_V2|" << bind.keyName << "|" 
						<< (bind.needCtrl ? "1" : "0") << "|" 
						<< (bind.needShift ? "1" : "0") << "|" 
						<< (bind.needAlt ? "1" : "0") << "|" 
						<< modeInt << "|" 
						<< bind.holdSeconds << "|" 
						<< bind.scriptPath << "|" 
						<< bind.toggleOnScriptPath << "|" 
						<< bind.toggleOffScriptPath << "\n";
			}
			outFile.close();
		}

		void LoadBindings() {
			std::ifstream inFile(GetBindsFileName());
			if (!inFile) return;

			std::string line;
			while (std::getline(inFile, line)) {
				line = trim(line);
				if (line.empty()) continue;
				std::vector<std::string> parts = split(line, "|");
				if (parts.size() < 2) continue;

				if (parts[0] == "MENUKEY") {
					int vKey = VKeyForName(trim(parts[1]));
					if (vKey != -1) menuToggleVKey = (USHORT)vKey;
				}
				else if (parts[0] == "BIND_V2" && parts.size() >= 10) {
					std::string keyName = trim(parts[1]);
					bool needCtrl = trim(parts[2]) == "1";
					bool needShift = trim(parts[3]) == "1";
					bool needAlt = trim(parts[4]) == "1";
					
					BindMode mode = BindMode::STANDARD;
					try { mode = static_cast<BindMode>(std::stoi(trim(parts[5]))); } catch(...) {}
					
					float holdSeconds = 0.0f;
					try { holdSeconds = std::stof(trim(parts[6])); } catch(...) {}
					
					std::string scriptPath = trim(parts[7]);
					std::string toggleOn = trim(parts[8]);
					std::string toggleOff = trim(parts[9]);

					int vKey = VKeyForName(keyName);
					if (vKey == -1) continue;

					KeyBind bind{ (USHORT)vKey, needCtrl, needShift, needAlt, keyName, mode, scriptPath, holdSeconds, toggleOn, toggleOff, false };
					bindings.push_back(bind);
				}
				// backwards compatibility safety catch for original layout profiles
				else if (parts[0] == "BIND" && parts.size() >= 7) {
					std::string keyName = trim(parts[1]);
					bool needCtrl = trim(parts[2]) == "1";
					bool needShift = trim(parts[3]) == "1";
					bool needAlt = trim(parts[4]) == "1";
					float holdSeconds = 0.0f;
					try { holdSeconds = std::stof(trim(parts[5])); } catch (...) {}
					std::string scriptPath = trim(parts[6]);

					int vKey = VKeyForName(keyName);
					if (vKey == -1) continue;

					KeyBind bind{ (USHORT)vKey, needCtrl, needShift, needAlt, keyName, BindMode::STANDARD, scriptPath, holdSeconds, "", "", false };
					bindings.push_back(bind);
				}
			}
		}

		void AddBinding(USHORT vKey, const std::string& keyName, bool needCtrl, bool needShift, bool needAlt, float holdSeconds, const std::string& scriptPath) {
			KeyBind bind{ vKey, needCtrl, needShift, needAlt, keyName, scriptPath, holdSeconds };
			bindings.push_back(bind);
			EnsureDispatcherRegistered(vKey);
			SaveBindings();
			DebuggerMenu::LogBindEvent("bound " + CombinedDisplayName(bind) + " -> " + scriptPath);
		}

		void RemoveBinding(int index) {
			if (index < 0 || index >= (int)bindings.size()) {
				return;
			}
			std::string removedDesc = CombinedDisplayName(bindings[index]) + " -> " + bindings[index].scriptPath;
			USHORT vKey = bindings[index].vKey;
			bindings.erase(bindings.begin() + index);
			RemoveDispatcherIfUnused(vKey);
			SaveBindings();
			DebuggerMenu::LogBindEvent("unbound " + removedDesc);
		}

		void RemoveAllBindings() {
			size_t count = bindings.size();
			for (const auto& entry : vKeyDispatchers) {
				RawInput::UnRegisterAction(entry.first, entry.second);
			}
			vKeyDispatchers.clear();
			bindings.clear();
			SaveBindings();
			DebuggerMenu::LogBindEvent("unbound all (" + std::to_string(count) + " binding(s))");
		}

		void Init(const std::string& defaultMenuKeyName) {
			int defaultVKey = VKeyForName(defaultMenuKeyName);
			if (defaultVKey != -1) {
				menuToggleVKey = (USHORT)defaultVKey;
			}
			else if (!defaultMenuKeyName.empty()) {
				spdlog::warn("KeyBindMenu::Init: unknown keyBindMenuToggleKey '{}' in ihhook_config.lua, using F4", defaultMenuKeyName);
			}

			LoadBindings(); // may override menuToggleVKey again if radar_keybinds.conf has a persisted MENUKEY
			for (const KeyBind& bind : bindings) {
				EnsureDispatcherRegistered(bind.vKey);
			}
			RegisterMenuToggleKey(menuToggleVKey);
		}

		static USHORT capturedVKey = 0;
		static bool capturedCtrl = false;
		static bool capturedShift = false;
		static bool capturedAlt = false;
		static float capturedHoldSeconds = 0.0f;
		static char capturedScriptPathBuffer[512] = "";
		static bool uiToggleModeChecked = false;
		static bool uiLongPressModeChecked = false;
		static char capturedToggleOnPathBuffer[512] = "";
		static char capturedToggleOffPathBuffer[512] = "";

		bool IsValidLuaScript(const char* pathBuffer) {
			if (pathBuffer == nullptr || pathBuffer[0] == '\0') return false;
			std::string text(pathBuffer);
			if (text.size() < 4) return false;
			std::string ext = text.substr(text.size() - 4);
			for (char &c : ext) c = std::tolower(c);
			return (ext == ".lua" && std::filesystem::exists(ResolveScriptPath(text)));
		}

		void DrawKeyCapturePrompt() {
			ImGui::SetNextWindowSize(ImVec2(400, 320), ImGuiCond_FirstUseEver);
			if (!ImGui::Begin("Assigning key bind...", nullptr, ImGuiWindowFlags_NoCollapse)) {
				ImGui::End();
				return;
			}

			// key capture
			if (capturedVKey == 0) {
				capturedCtrl  = ImGui::GetIO().KeyCtrl;
				capturedShift = ImGui::GetIO().KeyShift;
				capturedAlt   = ImGui::GetIO().KeyAlt;

				for (int i = 1; i < 256; i++) {
					if (i == VK_CONTROL || i == VK_SHIFT || i == VK_MENU || i == VK_LWIN || i == VK_RWIN ||
						i == VK_LCONTROL || i == VK_RCONTROL || i == VK_LSHIFT || i == VK_RSHIFT || i == VK_LMENU || i == VK_RMENU) continue;
					if (i == VK_LBUTTON && ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) continue;

					if (ImGui::IsKeyPressed((ImGuiKey)i)) {
						capturedVKey = (USHORT)i;
						capturedCtrl  = ImGui::GetIO().KeyCtrl;
						capturedShift = ImGui::GetIO().KeyShift;
						capturedAlt   = ImGui::GetIO().KeyAlt;
						break;
					}
				}
			}

			// key box
			ImGui::BeginChild("KeyDisplayFrame", ImVec2(110, 95), true);
			ImGui::Text(" Key Target");
			ImGui::Separator();
			if (capturedVKey == 0) {
				ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), " PRESS\n  KEY..");
			} else {
				ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "\n  %s", NameForVKey(capturedVKey).c_str());
			}
			ImGui::EndChild();

			ImGui::SameLine();

			// modifiers
			ImGui::BeginGroup();
			if (!isAssigningMenuToggleKey) {
				ImGui::Checkbox("Ctrl", &capturedCtrl);
				ImGui::Checkbox("Shift", &capturedShift);
				ImGui::Checkbox("Alt", &capturedAlt);
			} else {
				ImGui::Text("\n [ Menu Key Mode ]");
			}
			ImGui::EndGroup();

			ImGui::SameLine(230);

			// checks what type of key assignment is triggered.
			ImGui::BeginGroup();
			if (!isAssigningMenuToggleKey) {
				ImGui::Text("Input Sub-Mode:");
				
				// disables either of the bind mode
				if (ImGui::Checkbox("Toggle Switch", &uiToggleModeChecked)) {
					if (uiToggleModeChecked) uiLongPressModeChecked = false;
				}
				if (ImGui::Checkbox("Long Press", &uiLongPressModeChecked)) {
					if (uiLongPressModeChecked) {
						uiToggleModeChecked = false;
						ImGui::OpenPopup("Long Press Duration Setup");
					} else {
						capturedHoldSeconds = 0.0f;
					}
				}
			}
			ImGui::EndGroup();

			// long press
			if (ImGui::BeginPopupModal("Long Press Duration Setup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::Text("Configure Hold Ticker Requirements:");
				ImGui::Separator();
				ImGui::Spacing();
				
				ImGui::SetNextItemWidth(120);
				ImGui::InputFloat("Hold Seconds##popInput", &capturedHoldSeconds, 0.1f, 0.5f, "%.1fs");
				if (capturedHoldSeconds < 0.5f) capturedHoldSeconds = 0.5f;

				ImGui::Spacing();
				if (ImGui::Button("Confirm and Apply", ImVec2(180, 26))) {
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}

			ImGui::Separator();

			// script path
			if (!isAssigningMenuToggleKey) {
				if (uiToggleModeChecked) {
					// toggle mode script path 1
					ImGui::Text("ON State Script Destination:");
					ImGui::SetNextItemWidth(-1);
					ImGui::InputText("##toggleOnInput", capturedToggleOnPathBuffer, IM_ARRAYSIZE(capturedToggleOnPathBuffer));
					if (capturedToggleOnPathBuffer[0] != '\0') {
						if (!IsValidLuaScript(capturedToggleOnPathBuffer)) ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "* Script target invalid or not found!");
						else ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "  Target script recognized.");
					}

					// toggle mode script path 2
					ImGui::Text("OFF State Script Destination:");
					ImGui::SetNextItemWidth(-1);
					ImGui::InputText("##toggleOffInput", capturedToggleOffPathBuffer, IM_ARRAYSIZE(capturedToggleOffPathBuffer));
					if (capturedToggleOffPathBuffer[0] != '\0') {
						if (!IsValidLuaScript(capturedToggleOffPathBuffer)) ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "* Script target invalid or not found!");
						else ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "  Target script recognized.");
					}
				} else {
					// long press
					std::string scriptLabel = uiLongPressModeChecked ? "Script Path (Fires after hold delay):" : "Script Path:";
					ImGui::Text("%s", scriptLabel.c_str());
					ImGui::SetNextItemWidth(-1);
					ImGui::InputText("##captureScriptInput", capturedScriptPathBuffer, IM_ARRAYSIZE(capturedScriptPathBuffer));
					if (capturedScriptPathBuffer[0] != '\0') {
						if (!IsValidLuaScript(capturedScriptPathBuffer)) ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "* Script target invalid or not found!");
						else ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "  Target script recognized.");
					}
				}
			}

			ImGui::Separator();

			// finalize
			bool comboAvailable = (capturedVKey == 0) ? true : IsComboAvailable(capturedVKey, capturedCtrl, capturedShift, capturedAlt, capturedHoldSeconds);
			bool pathsValidated = false;

			if (isAssigningMenuToggleKey) {
				pathsValidated = true;
			} else if (uiToggleModeChecked) {
				pathsValidated = IsValidLuaScript(capturedToggleOnPathBuffer) && IsValidLuaScript(capturedToggleOffPathBuffer);
			} else {
				pathsValidated = IsValidLuaScript(capturedScriptPathBuffer);
			}

			bool canFinalize = capturedVKey != 0 && comboAvailable && pathsValidated;
			if (!canFinalize) ImGui::BeginDisabled();

			if (ImGui::Button("Finalize Profile", ImVec2(145, 30))) {
				if (isAssigningMenuToggleKey) {
					if (menuToggleHandle != 0) RawInput::UnRegisterAction(menuToggleVKey, menuToggleHandle);
					menuToggleVKey = capturedVKey;
					menuToggleHandle = RawInput::RegisterAction(menuToggleVKey, OnMenuToggleKeyPressed);
					SaveBindings();
				} else {
					KeyBind bind;
					bind.vKey = capturedVKey;
					bind.needCtrl = capturedCtrl;
					bind.needShift = capturedShift;
					bind.needAlt = capturedAlt;
					bind.keyName = NameForVKey(capturedVKey);
					
					if (uiToggleModeChecked) {
						bind.mode = BindMode::TOGGLE;
						bind.toggleOnScriptPath = ResolveScriptPath(capturedToggleOnPathBuffer);
						bind.toggleOffScriptPath = ResolveScriptPath(capturedToggleOffPathBuffer);
					} else {
						bind.mode = BindMode::STANDARD;
						bind.holdSeconds = capturedHoldSeconds;
						bind.scriptPath = ResolveScriptPath(capturedScriptPathBuffer);
					}
					
					bindings.push_back(bind);
					EnsureDispatcherRegistered(bind.vKey);
					SaveBindings();
				}

				// Global cleanup resets
				capturedVKey = 0; capturedHoldSeconds = 0.0f;
				capturedScriptPathBuffer[0] = '\0'; capturedToggleOnPathBuffer[0] = '\0'; capturedToggleOffPathBuffer[0] = '\0';
				uiToggleModeChecked = false; uiLongPressModeChecked = false;
				showCapturePrompt = false; isAssigningMenuToggleKey = false;
			}
			if (!canFinalize) ImGui::EndDisabled();

			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(145, 30))) {
				capturedVKey = 0; capturedHoldSeconds = 0.0f;
				capturedScriptPathBuffer[0] = '\0'; capturedToggleOnPathBuffer[0] = '\0'; capturedToggleOffPathBuffer[0] = '\0';
				uiToggleModeChecked = false; uiLongPressModeChecked = false;
				showCapturePrompt = false; isAssigningMenuToggleKey = false;
			}

			ImGui::End();
		}

		// draws the ui
		void Draw(bool* p_open) {
			ImGui::SetNextWindowSize(ImVec2(480, 360), ImGuiCond_::ImGuiCond_FirstUseEver);
			if (!ImGui::Begin("RadarKeys - Key Bindings", p_open)) {
				ImGui::End();
				return;
			}

			if (ImGui::Button("Debugger Overlay")) {
				DebuggerMenu::menuOpen = !DebuggerMenu::menuOpen;
			}
			ImGui::Separator();

			std::string buttonLabel = "Menu Hotkey: [" + NameForVKey(menuToggleVKey) + "]";
			if (ImGui::Button(buttonLabel.c_str(), ImVec2(-1, 28))) {
				isAssigningMenuToggleKey = true;
				showCapturePrompt = true;
			}
			ImGui::Separator();

			int removeIndex = -1;
			ImGui::BeginChild("ActiveBindingsOverviewList", ImVec2(0, 200), true);
			for (int i = 0; i < (int)bindings.size(); i++) {
				ImGui::PushID(i);
				if (ImGui::Button("Remove")) {
					removeIndex = i;
				}
				ImGui::SameLine();
				ImGui::Text("%s", CombinedDisplayName(bindings[i]).c_str());
				
				// a little designs lol
				if (bindings[i].mode == BindMode::TOGGLE) {
					ImGui::SameLine();
					if (bindings[i].currentToggleState) {
						ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "[STATE: ON]");
					} else {
						ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[STATE: OFF]");
					}
				}

				ImGui::SameLine(220);
				
				// renders the filenames
				std::string displayScript;
				if (bindings[i].mode == BindMode::TOGGLE) {
					std::string onFile = std::filesystem::path(bindings[i].toggleOnScriptPath).filename().string();
					std::string offFile = std::filesystem::path(bindings[i].toggleOffScriptPath).filename().string();
					displayScript = "On: " + onFile + " | Off: " + offFile;
				} else {
					displayScript = "-> " + std::filesystem::path(bindings[i].scriptPath).filename().string();
				}
				
				ImGui::TextWrapped("%s", displayScript.c_str());
				ImGui::PopID();
				ImGui::Separator();
			}
			ImGui::EndChild();

			if (removeIndex != -1) {
				RemoveBinding(removeIndex);
			}

			if (bindings.empty()) ImGui::BeginDisabled();
			if (ImGui::Button("Clear All Hotkeys")) {
				RemoveAllBindings();
			}
			if (bindings.empty()) ImGui::EndDisabled();

			ImGui::Separator();
			ImGui::Spacing();

			// launch the custom prompt
			if (ImGui::Button("Add New Binding...", ImVec2(-1, 35))) {
				showCapturePrompt = true;
			}

			ImGui::End();

			// draw if hte user interacts
			if (showCapturePrompt) {
				DrawKeyCapturePrompt();
			}
		}
	}
}
