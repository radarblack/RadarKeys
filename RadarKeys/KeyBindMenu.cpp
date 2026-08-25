#include "KeyBindMenu.h"
#include "RawInput.h"
#include "LuaBridge.h"
#include "DebuggerMenu.h"
#include "Util.h"
#include "spdlog/spdlog.h"
#include "imgui/imgui.h"

#include <fstream>
#include <filesystem>
#include <map>

namespace RadarKeys {
	namespace KeyBindMenu {

		std::vector<KeyBind> bindings;

		// mod/radarKeys/ config. existing root files not migrated - re-add bindings needed.
		const std::string bindsFileName = "mod/radarKeys/radar_keybinds.conf";

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

		// ctrl/shift/alt ordering matches the common Windows accelerator-key convention.
		std::string CombinedDisplayName(const KeyBind& bind) {
			std::string result;
			if (bind.needCtrl) result += "Ctrl + ";
			if (bind.needShift) result += "Shift + ";
			if (bind.needAlt) result += "Alt + ";
			result += bind.keyName;
			return result;
		}

		// adjusted so that if you just type the file name, it automatically assumes it is in the mod/modules folder.
		// otherwise, you can type the full path and it will run from there instead.
		std::string ResolveScriptPath(const std::string& typedPath)
		{
    		char path[MAX_PATH];
    		GetModuleFileNameA(nullptr, path, MAX_PATH);

    		return (std::filesystem::path(path).parent_path() /
            "mod" / "modules" / typedPath).string();
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
		bool IsComboAvailable(USHORT vKey, bool needCtrl, bool needShift, bool needAlt) {
			if (IsReservedVKey(vKey)) {
				return false;
			}
			for (const KeyBind& bind : bindings) {
				if (bind.vKey == vKey && bind.needCtrl == needCtrl && bind.needShift == needShift && bind.needAlt == needAlt) {
					return false;
				}
			}
			return true;
		}

		// fires once per vKey: exact modifier match first, then falls back to plain key binding if no match.
		void OnBoundKeyPressed(USHORT vKey, RawInput::BUTTONEVENT buttonEvent) {
			if (buttonEvent != RawInput::BUTTONEVENT::ONDOWN) {
				return;
			}

			bool ctrlHeld = RawInput::IsKeyDown(VK_CONTROL);
			bool shiftHeld = RawInput::IsKeyDown(VK_SHIFT);
			bool altHeld = RawInput::IsKeyDown(VK_MENU);

			// logs raw ONDOWN events for bound vKeys, irrespective of script binding success.
			std::string pressedName;
			if (ctrlHeld) pressedName += "Ctrl + ";
			if (shiftHeld) pressedName += "Shift + ";
			if (altHeld) pressedName += "Alt + ";
			pressedName += NameForVKey(vKey);
			DebuggerMenu::LogButtonPress(pressedName + " pressed");

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

			const KeyBind* toRun = exactMatch != nullptr ? exactMatch : fallbackMatch;
			if (toRun != nullptr) {
				// Checks file existence; logs "not found" internally; only queues IPC to Lua if file exists.
				if (DebuggerMenu::LogScriptAttempt(toRun->scriptPath)) {
					LuaBridge::QueueMessageIn("DoScript|dofile([[" + toRun->scriptPath + "]])");
				}
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
			// this creates the folder
			std::error_code ec;
			std::filesystem::create_directories("mod/radarKeys", ec);
			if (ec) {
				spdlog::warn("KeyBindMenu::SaveBindings: couldn't create mod/radarKeys directory: {}", ec.message());
			}

			std::ofstream outFile(bindsFileName);
			if (!outFile) {
				spdlog::warn("KeyBindMenu::SaveBindings: couldn't open {} for writing", bindsFileName);
				return;
			}
			outFile << "MENUKEY|" << NameForVKey(menuToggleVKey) << "\n";
			for (const KeyBind& bind : bindings) {
				outFile << "BIND|" << bind.keyName << "|" << (bind.needCtrl ? "1" : "0") << "|" << (bind.needShift ? "1" : "0") << "|" << (bind.needAlt ? "1" : "0") << "|" << bind.scriptPath << "\n";
			}
			outFile.close();
			spdlog::debug("KeyBindMenu::SaveBindings: wrote {} binding(s) to {}", bindings.size(), bindsFileName);
		}

		void LoadBindings() {
			std::ifstream inFile(bindsFileName);
			if (!inFile) {
				spdlog::debug("KeyBindMenu::LoadBindings: no {} yet (fine on first run)", bindsFileName);
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
				else if (parts[0] == "BIND" && parts.size() >= 6) {
					std::string keyName = trim(parts[1]);
					bool needCtrl = trim(parts[2]) == "1";
					bool needShift = trim(parts[3]) == "1";
					bool needAlt = trim(parts[4]) == "1";
					std::string scriptPath = trim(parts[5]);
					int vKey = VKeyForName(keyName);
					if (vKey == -1) {
						spdlog::warn("KeyBindMenu::LoadBindings: unknown key name '{}', skipping binding", keyName);
						continue;
					}
					bindings.push_back(KeyBind{ (USHORT)vKey, needCtrl, needShift, needAlt, keyName, scriptPath });
				}
				else if (parts[0] == "BIND") {
					// handles old config formats safely by skipping invalid entries.
					spdlog::warn("KeyBindMenu::LoadBindings: skipping old-format/malformed BIND line: {}", line);
				}
			}
			spdlog::debug("KeyBindMenu::LoadBindings: loaded {} binding(s) from {}", bindings.size(), bindsFileName);
		}

		void AddBinding(USHORT vKey, const std::string& keyName, bool needCtrl, bool needShift, bool needAlt, const std::string& scriptPath) {
			KeyBind bind{ vKey, needCtrl, needShift, needAlt, keyName, scriptPath };
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
				spdlog::warn("KeyBindMenu::Init: unknown keyBindMenuToggleKey '{}' in ihhook_config.lua, using F7", defaultMenuKeyName);
			}

			LoadBindings(); // may override menuToggleVKey again if radar_keybinds.conf has a persisted MENUKEY
			for (const KeyBind& bind : bindings) {
				EnsureDispatcherRegistered(bind.vKey);
			}
			RegisterMenuToggleKey(menuToggleVKey);
		}

		void Draw(bool* p_open) {
			ImGui::SetNextWindowSize(ImVec2(460, 460), ImGuiCond_::ImGuiCond_FirstUseEver);
			if (!ImGui::Begin("RadarKeys - Key Bindings", p_open)) {
				ImGui::End();
				return;
			}

			if (ImGui::Button("Debugger")) {
				DebuggerMenu::menuOpen = !DebuggerMenu::menuOpen;
			}
			ImGui::Separator();

			// trmap the menu's own toggle key (no modifier support here - see IsReservedVKey comment)
			ImGui::Text("Menu opens with: %s", NameForVKey(menuToggleVKey).c_str());
			ImGui::SameLine();
			static int menuKeyComboIndex = -1;
			if (menuKeyComboIndex == -1) {
				for (int i = 0; i < vkNameTableCount; i++) {
					if (vkNameTable[i].vKey == menuToggleVKey) {
						menuKeyComboIndex = i;
						break;
					}
				}
				if (menuKeyComboIndex == -1) menuKeyComboIndex = 0;
			}
			ImGui::SetNextItemWidth(100);
			if (ImGui::BeginCombo("##menuKeyCombo", vkNameTable[menuKeyComboIndex].name)) {
				for (int i = 0; i < vkNameTableCount; i++) {
					bool selected = (i == menuKeyComboIndex);
					if (ImGui::Selectable(vkNameTable[i].name, selected)) {
						menuKeyComboIndex = i;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			if (ImGui::Button("Apply##menuKey")) {
				USHORT newVKey = vkNameTable[menuKeyComboIndex].vKey;
				if (newVKey == menuToggleVKey || IsComboAvailable(newVKey, false, false, false)) {
					RawInput::UnRegisterAction(menuToggleVKey, menuToggleHandle);
					RegisterMenuToggleKey(newVKey);
					SaveBindings();
				}
				else {
					spdlog::warn("KeyBindMenu: can't remap menu-toggle key to {} - already in use", vkNameTable[menuKeyComboIndex].name);
				}
			}

			ImGui::Separator();
			ImGui::TextWrapped("Note: Key with assigned scripts without modifiers (CTRL/SHIFT/ALT) will still trigger when pressed even when modifier keys are pressed, except if there is an assigned script to that key combination.");
			ImGui::Spacing();

			// existing bindings list, each with its own remove button, plus a bulk "Remove All"
			int removeIndex = -1;
			ImGui::BeginChild("BindingsList", ImVec2(0, 180), true);
			for (int i = 0; i < (int)bindings.size(); i++) {
				ImGui::PushID(i);
				if (ImGui::Button("Remove")) {
					removeIndex = i;
				}
				ImGui::SameLine();
				ImGui::Text("%s", CombinedDisplayName(bindings[i]).c_str());
				ImGui::SameLine(160);
				std::string scriptName = std::filesystem::path(bindings[i].scriptPath).filename().string();
				ImGui::TextWrapped("%s", scriptName.c_str());
				ImGui::PopID();
				ImGui::Separator();
			}
			ImGui::EndChild();
			if (removeIndex != -1) {
				RemoveBinding(removeIndex);
			}

			if (bindings.empty()) {
				ImGui::BeginDisabled();
			}
			if (ImGui::Button("Remove All Bindings")) {
				RemoveAllBindings();
			}
			if (bindings.empty()) {
				ImGui::EndDisabled();
			}

			ImGui::Spacing();
			ImGui::Text("Add new binding");

			static int addComboIndex = 0;
			ImGui::SetNextItemWidth(100);
			if (ImGui::BeginCombo("Key##addCombo", vkNameTable[addComboIndex].name)) {
				for (int i = 0; i < vkNameTableCount; i++) {
					bool selected = (i == addComboIndex);
					if (ImGui::Selectable(vkNameTable[i].name, selected)) {
						addComboIndex = i;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			static bool addCtrl = false;
			static bool addShift = false;
			static bool addAlt = false;
			ImGui::Checkbox("Ctrl", &addCtrl);
			ImGui::SameLine();
			ImGui::Checkbox("Shift", &addShift);
			ImGui::SameLine();
			ImGui::Checkbox("Alt", &addAlt);

			// plain text file path input (abs/rel to game root); no Win32 dialog deps.
			static char scriptPathBuffer[512] = "";
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText("##scriptPathInput", scriptPathBuffer, IM_ARRAYSIZE(scriptPathBuffer));
			ImGui::TextDisabled("Just a filename assumes mod/modules/ - or type a path (with / or \\) to use elsewhere");

			USHORT selectedVKey = vkNameTable[addComboIndex].vKey;
			bool comboAvailable = IsComboAvailable(selectedVKey, addCtrl, addShift, addAlt);
			bool canAdd = scriptPathBuffer[0] != '\0' && comboAvailable;
			if (!canAdd) {
				ImGui::BeginDisabled();
			}
			if (ImGui::Button("Add Binding")) {
				AddBinding(selectedVKey, vkNameTable[addComboIndex].name, addCtrl, addShift, addAlt, ResolveScriptPath(scriptPathBuffer));
				scriptPathBuffer[0] = '\0';
			}
			if (!canAdd) {
				ImGui::EndDisabled();
			}
			if (scriptPathBuffer[0] != '\0' && !comboAvailable) {
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "That key is already in use");
			}

			ImGui::End();
		}

	}
}
