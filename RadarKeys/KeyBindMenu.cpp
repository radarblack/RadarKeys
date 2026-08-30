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
			if (bind.holdSeconds > 0.0f) {
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
				if (bind.vKey == vKey && bind.needCtrl == needCtrl && bind.needShift == needShift && bind.needAlt == needAlt) {
					// If the hold seconds match, they are conflicting duplicates.
					if (bind.holdSeconds == holdSeconds) {
						return false;
					}
					
					// chceks if the hold second remains at 0.0
					if ((holdSeconds <= 0.0f && bind.holdSeconds <= 0.0f) || (holdSeconds > 0.0f && bind.holdSeconds > 0.0f)) {
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
			const KeyBind* fallbackMatch = nullptr; // the plain/no-modifier binding on this vKey, if any
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
			return exactMatch != nullptr ? exactMatch : fallbackMatch;
		}

		void FireBinding(const KeyBind& bind) {
			// Checks if file exists
			if (DebuggerMenu::LogScriptAttempt(bind.scriptPath)) {
				LuaBridge::QueueMessageIn("DoScript|dofile([[" + bind.scriptPath + "]])");
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

			const KeyBind* toRun = FindMatchingBinding(vKey, ctrlHeld, shiftHeld, altHeld);
			if (toRun == nullptr) {
				return;
			}

			// look to see if there is ANY binding on this virtual key setup that uses a hold delay
			bool hasHoldOptionOnKey = false;
			for (const KeyBind& bind : bindings) {
				if (bind.vKey == vKey && bind.needCtrl == ctrlHeld && bind.needShift == shiftHeld && bind.needAlt == altHeld && bind.holdSeconds > 0.0f) {
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
			// ensure mod/radarKeys/ directory exists before writing bindings file (ofstream won't create it).
			std::error_code ec;
			std::filesystem::path bindsDir = std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys";
			std::filesystem::create_directories(bindsDir, ec);
			if (ec) {
				spdlog::warn("KeyBindMenu::SaveBindings: couldn't create {} directory: {}", bindsDir.string(), ec.message());
			}

			std::ofstream outFile(GetBindsFileName());
			if (!outFile) {
				spdlog::warn("KeyBindMenu::SaveBindings: couldn't open {} for writing", GetBindsFileName());
				return;
			}
			outFile << "MENUKEY|" << NameForVKey(menuToggleVKey) << "\n";
			for (const KeyBind& bind : bindings) {
				outFile << "BIND|" << bind.keyName << "|" << (bind.needCtrl ? "1" : "0") << "|" << (bind.needShift ? "1" : "0") << "|" << (bind.needAlt ? "1" : "0") << "|" << bind.holdSeconds << "|" << bind.scriptPath << "\n";
			}
			outFile.close();
			spdlog::debug("KeyBindMenu::SaveBindings: wrote {} binding(s) to {}", bindings.size(), GetBindsFileName());
		}

		void LoadBindings() {
			std::ifstream inFile(GetBindsFileName());
			if (!inFile) {
				spdlog::debug("KeyBindMenu::LoadBindings: no {} yet (fine on first run)", GetBindsFileName());
				return;
			}

			std::string line;
			while (std::getline(inFile, line)) {
				line = trim(line);
				if (line.size() == 0) {
					continue;
				}
				std::vector<std::string> parts = split(line, "|");
				if (parts.size() < 2) {
					spdlog::warn("KeyBindMenu::LoadBindings: skipping malformed line: {}", line);
					continue;
				}

				if (parts[0] == "MENUKEY") {
					int vKey = VKeyForName(trim(parts[1]));
					if (vKey != -1) {
						menuToggleVKey = (USHORT)vKey;
					}
					else {
						spdlog::warn("KeyBindMenu::LoadBindings: unknown MENUKEY name '{}', keeping default", parts[1]);
					}
				}
				else if (parts[0] == "BIND" && parts.size() >= 7) {
					std::string keyName = trim(parts[1]);
					bool needCtrl = trim(parts[2]) == "1";
					bool needShift = trim(parts[3]) == "1";
					bool needAlt = trim(parts[4]) == "1";
					float holdSeconds = 0.0f;
					try {
						holdSeconds = std::stof(trim(parts[5]));
					}
					catch (...) {
						spdlog::warn("KeyBindMenu::LoadBindings: bad holdSeconds value '{}', defaulting to 0", parts[5]);
					}
					std::string scriptPath = trim(parts[6]);
					int vKey = VKeyForName(keyName);
					if (vKey == -1) {
						spdlog::warn("KeyBindMenu::LoadBindings: unknown key name '{}', skipping binding", keyName);
						continue;
					}
					bindings.push_back(KeyBind{ (USHORT)vKey, needCtrl, needShift, needAlt, keyName, scriptPath, holdSeconds });
				}
				else if (parts[0] == "BIND") {
					// handles old config formats safely by skipping invalid entries.
					spdlog::warn("KeyBindMenu::LoadBindings: skipping old-format/malformed BIND line: {}", line);
				}
			}
			spdlog::debug("KeyBindMenu::LoadBindings: loaded {} binding(s) from {}", bindings.size(), GetBindsFileName());
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

		static bool showCapturePrompt = false;
		static USHORT capturedVKey = 0;
		static bool capturedCtrl = false;
		static bool capturedShift = false;
		static bool capturedAlt = false;
		static float capturedHoldSeconds = 0.0f;
		static char capturedScriptPathBuffer[512] = "";

		void DrawKeyCapturePrompt() {
			// center the  overlay nicely on the viewport screen
			ImGui::SetNextWindowSize(ImVec2(500, 250), ImGuiCond_Always);
			ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f - 250, ImGui::GetIO().DisplaySize.y * 0.5f - 125), ImGuiCond_Always);

			if (!ImGui::Begin("Assign Hotkey Connection", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
				ImGui::End();
				return;
			}

			// key capture
			if (capturedVKey == 0) {
				for (int i = 1; i < 256; i++) {
					// skip processing modifier keys independently so they don't break the listener
					if (i == VK_CONTROL || i == VK_SHIFT || i == VK_MENU || i == VK_LWIN || i == VK_RWIN) {
						continue;
					}
					// ignore left mouse clicks inside the menu bounds so checking checkboxes works safely
					if (i == VK_LBUTTON && ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
						continue;
					}

					if (ImGui::IsKeyPressed((ImGuiKey)i)) {
						capturedVKey = (USHORT)i;
						
						// lock in active modifier states the instant the base key is registered
						capturedCtrl = ImGui::GetIO().KeyCtrl;
						capturedShift = ImGui::GetIO().KeyShift;
						capturedAlt = ImGui::GetIO().KeyAlt;
						break;
					}
				}
			}

			ImGui::BeginChild("KeyDisplayFrame", ImVec2(160, 100), true);
			if (capturedVKey == 0) {
				ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "PRESS ANY KEY...");
			} else {
				ImGui::Text("Captured Base:");
				ImGui::Separator();
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "  %s", NameForVKey(capturedVKey).c_str());
				ImGui::Spacing();
				ImGui::TextDisabled("(Click here to reset)");
				if (ImGui::IsItemClicked()) {
					capturedVKey = 0;
				}
			}
			ImGui::EndChild();

			ImGui::SameLine();

			// column stack layout for modifiers matching your wireframe center boxes alignment
			ImGui::BeginGroup();
			ImGui::Checkbox("Ctrl", &capturedCtrl);
			ImGui::Checkbox("Shift", &capturedShift);
			ImGui::Checkbox("Alt", &capturedAlt);
			ImGui::EndGroup();

			ImGui::SameLine(260);

			// right side panel for Hold seconds numerical display controls
			ImGui::BeginGroup();
			ImGui::Text("Hold Configuration:");
			
			// upper Horizontal Rectangle display container matching your graphic design layout
			ImGui::BeginChild("HoldValueBox", ImVec2(180, 28), true, ImGuiWindowFlags_NoScrollbar);
			if (capturedHoldSeconds <= 0.0f) {
				ImGui::Text("Instant Trigger (0.0s)");
			} else {
				ImGui::Text("Duration: %.1fs", capturedHoldSeconds);
			}
			ImGui::EndChild();

			// math calculation adjustments via [+] and [-] control buttons
			if (ImGui::Button(" - ##SubHold", ImVec2(40, 25))) {
				capturedHoldSeconds -= 0.5f;
				if (capturedHoldSeconds < 0.0f) capturedHoldSeconds = 0.0f;
			}
			ImGui::SameLine();
			if (ImGui::Button(" + ##AddHold", ImVec2(40, 25))) {
				capturedHoldSeconds += 0.5f;
			}
			
			ImGui::SameLine(115);
			
			// tooltip
			bool comboAvailable = (capturedVKey == 0) ? true : IsComboAvailable(capturedVKey, capturedCtrl, capturedShift, capturedAlt, capturedHoldSeconds);
			if (!comboAvailable) {
				// red warning visual status container box
				ImGui::BeginChild("StatusIndicatorBox", ImVec2(65, 25), true);
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), " BLOCKED ");
				ImGui::EndChild();
				
				// hover tooltip execution string block explaining the structural assignment issue
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Conflict! Key combination is already in use.\nAdjust 'Hold seconds' value to unlock a secondary script assignment.");
				}
			} else {
				// green safe status
				ImGui::BeginChild("StatusIndicatorBox", ImVec2(65, 25), true);
				ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), " READY ");
				ImGui::EndChild();
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Configuration slot is open and ready to register target script execution.");
				}
			}
			ImGui::EndGroup();

			ImGui::Separator();

			ImGui::Text("Target Script Path:");
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText("##captureScriptInput", capturedScriptPathBuffer, IM_ARRAYSIZE(capturedScriptPathBuffer));

			ImGui::Spacing();
			ImGui::Separator();

			// finalize
			bool canFinalize = capturedVKey != 0 && capturedScriptPathBuffer[0] != '\0' && comboAvailable;
			if (!canFinalize) ImGui::BeginDisabled();
			
			// "Finalize" button assignment hook
			if (ImGui::Button("Finalize Assignment", ImVec2(235, 30))) {
				AddBinding(capturedVKey, NameForVKey(capturedVKey), capturedCtrl, capturedShift, capturedAlt, capturedHoldSeconds, ResolveScriptPath(capturedScriptPathBuffer));
				
				// safely clean context loops, reset local parameter buffers, and return control
				capturedVKey = 0;
				capturedHoldSeconds = 0.0f;
				capturedScriptPathBuffer[0] = '\0';
				showCapturePrompt = false;
			}
			if (!canFinalize) ImGui::EndDisabled();

			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(235, 30))) {
				capturedVKey = 0;
				capturedHoldSeconds = 0.0f;
				capturedScriptPathBuffer[0] = '\0';
				showCapturePrompt = false;
			}

			ImGui::End();
		}

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

			// configuration mapping tracking table rows
			ImGui::Text("Current Menu Toggle Hotkey: %s", NameForVKey(menuToggleVKey).c_str());
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
				ImGui::SameLine(180);
				std::string scriptName = std::filesystem::path(bindings[i].scriptPath).filename().string();
				ImGui::TextWrapped("-> %s", scriptName.c_str());
				ImGui::PopID();
				ImGui::Separator();
			}
			ImGui::EndChild();

			if (removeIndex != -1) {
				RemoveBinding(removeIndex);
			}

			if (bindings.empty()) ImGui::BeginDisabled();
			if (ImGui::Button("Clear All Registered Hotkeys")) {
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
