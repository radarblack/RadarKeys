#include "KeyBindMenu.h"
#include "RawInput.h"
#include "LuaBridge.h"
#include "DebuggerMenu.h"
#include "LuaKeyState.h"
#include "ModKeyBindings.h"
#include "Util.h"
#include "HookUtils.h"
#include "spdlog/spdlog.h"
#include "imgui/imgui.h"
#include <fstream>
#include <filesystem>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>

namespace RadarKeys {
	bool showCapturePrompt = false; 
	namespace KeyBindMenu {
		std::vector<KeyBind> bindings;
		static bool isAssigningMenuToggleKey = false; 
		struct BindingDisplayCache {
			std::string itemLabel;
			std::string detailText;
			std::string fullLine;
		};

		static std::vector<BindingDisplayCache> displayCache;
		static bool displayCacheDirty = true;
		void MarkDisplayCacheDirty() { displayCacheDirty = true; }
		const std::string& GetBindsFileName() {
			static std::string cached;
			if (cached.empty()) {
				cached = (std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys" / "radar_keybinds.conf").string();
			}
			return cached;
		}

		const std::string& GetLogFileName() {
			static std::string cached;
			if (cached.empty()) {
				cached = (std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys" / "radarkeys_log.txt").string();
			}
			return cached;
		}

		bool EnsureBindsDirectory() {
			static bool ensured = false;
			if (ensured) return true;

			std::error_code ec;
			std::filesystem::path bindsDir = std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys";
			std::filesystem::create_directories(bindsDir, ec);
			if (ec) {
				spdlog::warn("KeyBindMenu: couldn't create {} directory: {}", bindsDir.string(), ec.message());
				return false;
			}
			ensured = true;
			return true;
		}

		static std::deque<std::string> activityLogLines;
		static size_t activityLogBytes = 0;
		static size_t activityLogBaselineBytes = 0;
		static size_t activityLogMaxBytes = 10240;
		static int activityLogWritesSinceFlush = 0;
		static bool activityLogReady = false;
		static constexpr int activityLogFlushEveryNWrites = 20;

		bool PreviousSessionEndedCleanly(const std::string& logPath) {
			std::ifstream in(logPath);
			if (!in) return true;
			std::string line, lastNonEmptyLine;
			while (std::getline(in, line)) {
				if (!line.empty()) lastNonEmptyLine = line;
			}
			return lastNonEmptyLine.empty() || lastNonEmptyLine == "[STATE] CLEAN_EXIT";
		}

		void EnsureActivityLogReady() {
			if (activityLogReady) return;
			EnsureBindsDirectory();

			std::string currentLog = GetLogFileName();
			std::string prevLog = currentLog;
			size_t replacePos = prevLog.find("radarkeys_log.txt");
			if (replacePos != std::string::npos) {
				prevLog.replace(replacePos, 18, "radarkeys_log_prev.txt");
			}

			std::error_code ec;
			bool wasClean = true;
			if (std::filesystem::exists(currentLog, ec)) {
				wasClean = PreviousSessionEndedCleanly(currentLog);
				std::filesystem::copy_file(currentLog, prevLog, std::filesystem::copy_options::overwrite_existing, ec);
			}

			std::ofstream clearStream(currentLog, std::ios::trunc);
			if (!wasClean) {
				clearStream << "[WARNING] The previous session did not close cleanly (Crashed or Terminated Abruptly).\n";
			}
			
			clearStream.close();
			activityLogBaselineBytes = std::filesystem::file_size(currentLog, ec);
			if (ec) activityLogBaselineBytes = 0;
			activityLogReady = true;
		}

		void FlushActivityLog() {
			std::string currentLog = GetLogFileName();
			std::error_code ec;
			std::filesystem::resize_file(currentLog, activityLogBaselineBytes, ec);
			std::ofstream out(currentLog, std::ios::app);
			
			if (!out) {
				spdlog::warn("KeyBindMenu::FlushActivityLog: couldn't open {} for writing", currentLog);
				return;
			}
			for (const std::string& line : activityLogLines) {
				out << line;
			}
		}

		void AppendActivityLogLine(const std::string& line) {
			activityLogLines.push_back(line);
			activityLogBytes += line.size();

			while (activityLogBytes > activityLogMaxBytes && !activityLogLines.empty()) {
				activityLogBytes -= activityLogLines.front().size();
				activityLogLines.pop_front();
			}

			if (++activityLogWritesSinceFlush >= activityLogFlushEveryNWrites) {
				activityLogWritesSinceFlush = 0;
				FlushActivityLog();
			}
		}

		void LogActivity(const std::string& message, bool success) {
			EnsureActivityLogReady();
			auto duration = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::duration<double>(ImGui::GetTime()));
			auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
			duration -= hours;
			auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
			duration -= minutes;
			char timeBuf[32];
			snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", hours.count(), minutes.count(), duration.count());
			AppendActivityLogLine(std::string("[") + timeBuf + "] [" + (success ? "OK" : "FAIL") + "] " + message + "\n");
		}

		void LogCleanShutdown() {
			EnsureActivityLogReady();
			AppendActivityLogLine("[STATE] CLEAN_EXIT\n");
			FlushActivityLog();
		}

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
			{"Page Up", VK_PRIOR}, {"Page Down", VK_NEXT},
			{"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
			{"Numpad 0", VK_NUMPAD0}, {"Numpad 1", VK_NUMPAD1}, {"Numpad 2", VK_NUMPAD2},
			{"Numpad 3", VK_NUMPAD3}, {"Numpad 4", VK_NUMPAD4}, {"Numpad 5", VK_NUMPAD5},
			{"Numpad 6", VK_NUMPAD6}, {"Numpad 7", VK_NUMPAD7}, {"Numpad 8", VK_NUMPAD8},
			{"Numpad 9", VK_NUMPAD9},
			{",", VK_OEM_COMMA}, {".", VK_OEM_PERIOD},
			
			{"Mouse Wheel", VK_MBUTTON},
			{"Mouse 4", VK_XBUTTON1},
			{"Mouse 5", VK_XBUTTON2}
		};
		const int vkNameTableCount = sizeof(vkNameTable) / sizeof(vkNameTable[0]);

		std::string NameForVKey(USHORT vKey) {
			for (const auto& entry : vkNameTable) {
				if (entry.vKey == vKey) return entry.name;
			}
			return "Unknown(" + std::to_string(vKey) + ")";
		}

		int VKeyForName(const std::string& name) {
			for (const auto& entry : vkNameTable) {
				if (name == entry.name) return entry.vKey;
			}
			return -1;
		}

		std::string CombinedDisplayName(const KeyBind& bind) {
			std::string result = std::string(bind.needCtrl ? "Ctrl+" : "") + (bind.needShift ? "Shift+" : "") + (bind.needAlt ? "Alt+" : "") + bind.keyName;
			if (bind.holdSeconds > 0.0f) {
				char buf[32];
				snprintf(buf, sizeof(buf), " (hold %.1fs)", bind.holdSeconds);
				result += buf;
			}
			return result;
		}

		std::string ResolveScriptPath(const std::string& typedPath) {
			if (std::filesystem::path(typedPath).is_absolute()) return typedPath;
			bool hasSeparators = typedPath.find('/') != std::string::npos || typedPath.find('\\') != std::string::npos;
			return (std::filesystem::path(GetGameDirectory()) / (hasSeparators ? std::filesystem::path(typedPath) : std::filesystem::path("mod") / "modules" / typedPath)).string();
		}

		USHORT menuToggleVKey = VK_F7;
		RawInput::ActionHandle menuToggleHandle = 0;
		bool menuOpen = false;
		std::map<USHORT, RawInput::ActionHandle> vKeyDispatchers;

		void OnMenuToggleKeyPressed(RawInput::BUTTONEVENT buttonEvent) {
			if (buttonEvent != RawInput::BUTTONEVENT::ONDOWN) {
				return;
			}
			if (showCapturePrompt) {
				return;
			}

			menuOpen = !menuOpen;
			LogActivity(menuOpen ? "Menu opened" : "Menu closed");
		}

		void RegisterMenuToggleKey(USHORT vKey) {
			menuToggleVKey = vKey;
			menuToggleHandle = RawInput::RegisterAction(vKey, OnMenuToggleKeyPressed);
		}

		bool IsReservedVKey(USHORT vKey) {
			return vKey == VK_F2 || vKey == VK_F3 || vKey == VK_ESCAPE || vKey == menuToggleVKey;
		}

		bool IsComboAvailable(USHORT vKey, bool needCtrl, bool needShift, bool needAlt, float holdSeconds) {
			if (IsReservedVKey(vKey)) return false;
			for (const auto& bind : bindings) {
				if (bind.vKey == vKey && bind.needCtrl == needCtrl && bind.needShift == needShift && bind.needAlt == needAlt) {
					if (bind.holdSeconds == holdSeconds || ((holdSeconds > 0.0f) == (bind.holdSeconds > 0.0f))) return false;
				}
			}
			return true;
		}

		struct HoldTrack {
			std::chrono::steady_clock::time_point startTime;
			bool fired = false;
			bool ctrlOnPressed = false;
			bool shiftOnPressed = false;
			bool altOnPressed = false;
		};
		std::map<USHORT, HoldTrack> holdTracks;

		const KeyBind* FindMatchingBinding(USHORT vKey, bool ctrlHeld, bool shiftHeld, bool altHeld, bool preferHold) {
			const KeyBind* exactMatch = nullptr;
			const KeyBind* plainFallbackMatch = nullptr;

			for (const auto& bind : bindings) {
				if (bind.vKey != vKey) continue;
				if (bind.needCtrl == ctrlHeld && bind.needShift == shiftHeld && bind.needAlt == altHeld) {
					if (preferHold && bind.holdSeconds > 0.0f) return &bind;
					if (!preferHold && bind.holdSeconds <= 0.0f) return &bind;
					exactMatch = &bind;
				}
				if (!bind.needCtrl && !bind.needShift && !bind.needAlt) {
					if (preferHold && bind.holdSeconds > 0.0f) plainFallbackMatch = &bind;
					if (!preferHold && bind.holdSeconds <= 0.0f) plainFallbackMatch = &bind;
				}
			}

			return exactMatch ? exactMatch : plainFallbackMatch;
		}

		void FireBinding(const KeyBind& bind) {
			std::string targetPath = bind.scriptPathOn;
			std::string targetFunc = bind.functionTap;

			if (bind.isToggle) {
				targetPath = bind.toggleState ? bind.scriptPathOff : bind.scriptPathOn;
				targetFunc = bind.toggleState ? bind.functionOff : bind.functionOn;
				bind.toggleState = !bind.toggleState;
			}

			if (DebuggerMenu::LogScriptAttempt(targetPath)) {
				if (!targetFunc.empty()) {
					std::string luaPayload = "DoScript|local f = loadfile([[" + targetPath + "]]); if f then f(); if " + targetFunc + " then " + targetFunc + "(); end end";
					LuaBridge::QueueMessageIn(luaPayload);
					LogActivity("Fired script " + targetPath + " [" + targetFunc + "]");
				} else {
					LuaBridge::QueueMessageIn("DoScript|dofile([[" + targetPath + "]])");
					LogActivity("Fired script " + targetPath);
				}
			} else {
				LogActivity("Script not found: " + targetPath, false);
			}
		}

		void OnBoundKeyPressed(USHORT vKey, RawInput::BUTTONEVENT buttonEvent) {
			if (showCapturePrompt) return;
			bool ctrlHeld = RawInput::IsKeyHeldReal(VK_CONTROL), shiftHeld = RawInput::IsKeyHeldReal(VK_SHIFT), altHeld = RawInput::IsKeyHeldReal(VK_MENU);

			if (buttonEvent == RawInput::BUTTONEVENT::ONUP) {
				auto it = holdTracks.find(vKey);
				if (it != holdTracks.end()) {
					if (!it->second.fired) {
						const KeyBind* tapBind = FindMatchingBinding(vKey, it->second.ctrlOnPressed, it->second.shiftOnPressed, it->second.altOnPressed, false);
						if (tapBind && tapBind->holdSeconds <= 0.0f) {
							DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " tapped cleanly (Hold bypassed)");
							LogActivity(NameForVKey(vKey) + " tapped cleanly (Hold bypassed)");
							FireBinding(*tapBind);
						}
					}
					holdTracks.erase(it);
				}
				return;
			}
			
			if (buttonEvent != RawInput::BUTTONEVENT::ONDOWN) return;
			DebuggerMenu::LogButtonPress(std::string(ctrlHeld ? "Ctrl+" : "") + (shiftHeld ? "Shift+" : "") + (altHeld ? "Alt+" : "") + NameForVKey(vKey) + " pressed");
			LogActivity(std::string(ctrlHeld ? "Ctrl+" : "") + (shiftHeld ? "Shift+" : "") + (altHeld ? "Alt+" : "") + NameForVKey(vKey) + " pressed");

			bool hasHoldOptionOnKey = false;
			for (const auto& bind : bindings) {
				if (bind.vKey == vKey && bind.holdSeconds > 0.0f) {
					if ((bind.needCtrl == ctrlHeld && bind.needShift == shiftHeld && bind.needAlt == altHeld) ||
						(!bind.needCtrl && !bind.needShift && !bind.needAlt)) {
						hasHoldOptionOnKey = true;
						break;
					}
				}
			}

			const KeyBind* toRun = FindMatchingBinding(vKey, ctrlHeld, shiftHeld, altHeld, false);
			if (toRun && !hasHoldOptionOnKey) {
				FireBinding(*toRun);
			} else {
				holdTracks[vKey] = HoldTrack{ std::chrono::steady_clock::now(), false, ctrlHeld, shiftHeld, altHeld };
			}
		}

		void Update() {
			if (showCapturePrompt) return;
			for (auto it = holdTracks.begin(); it != holdTracks.end(); ) {
				USHORT vKey = it->first; HoldTrack& track = it->second;
				if (track.fired || !RawInput::IsKeyHeldReal(vKey)) { it = holdTracks.erase(it); continue; }
				const KeyBind* holdBind = FindMatchingBinding(vKey, track.ctrlOnPressed, track.shiftOnPressed, track.altOnPressed, true);

				if (holdBind && holdBind->holdSeconds > 0.0f) {
					if (std::chrono::duration<float>(std::chrono::steady_clock::now() - track.startTime).count() >= holdBind->holdSeconds) {
						DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " held past threshold " + std::to_string(holdBind->holdSeconds) + "s");
						LogActivity(NameForVKey(vKey) + " held past threshold " + std::to_string(holdBind->holdSeconds) + "s");
						FireBinding(*holdBind); 
						track.fired = true;
					}
				}
				++it;
			}
		}

		void EnsureDispatcherRegistered(USHORT vKey) {
			if (vKeyDispatchers.find(vKey) != vKeyDispatchers.end()) return;
			vKeyDispatchers[vKey] = RawInput::RegisterAction(vKey, [vKey](RawInput::BUTTONEVENT ev) { OnBoundKeyPressed(vKey, ev); });
		}

		void RemoveDispatcherIfUnused(USHORT vKey) {
			for (const auto& bind : bindings) if (bind.vKey == vKey) return;
			auto it = vKeyDispatchers.find(vKey);
			if (it != vKeyDispatchers.end()) { RawInput::UnRegisterAction(vKey, it->second); vKeyDispatchers.erase(it); }
		}

		void SaveBindings() {
			EnsureBindsDirectory();
			std::ofstream outFile(GetBindsFileName());
			if (!outFile) {
				spdlog::warn("KeyBindMenu::SaveBindings: couldn't open {} for writing", GetBindsFileName());
				LogActivity("Save bindings failed: couldn't open " + GetBindsFileName() + " for writing", false);
				return;
			}
			
			outFile << "MENUKEY|" << NameForVKey(menuToggleVKey) << "\n";
			std::string gameDirStr = std::filesystem::path(GetGameDirectory()).generic_string() + "/";
			for (auto b : bindings) {
				std::string genericOn = b.scriptPathOn.empty() ? "" : std::filesystem::path(b.scriptPathOn).generic_string();
				std::string genericOff = b.scriptPathOff.empty() ? "" : std::filesystem::path(b.scriptPathOff).generic_string();

				if (!genericOn.empty() && genericOn.find(gameDirStr) == 0) {
					genericOn.erase(0, gameDirStr.length());
				}
				if (!genericOff.empty() && genericOff.find(gameDirStr) == 0) {
					genericOff.erase(0, gameDirStr.length());
				}

				outFile << "BIND|" << b.keyName << "|" << (b.needCtrl ? "1|" : "0|") << (b.needShift ? "1|" : "0|") << (b.needAlt ? "1|" : "0|") << b.holdSeconds << "|";
				if (b.isToggle) {
					outFile << "1|" << genericOn << "|" << genericOff << "|" << b.functionOn << "|" << b.functionOff << "\n";
				} else {
					outFile << "0|" << genericOn << "|" << b.functionTap << "\n";
				}
			}
			outFile.close();
			spdlog::debug("KeyBindMenu::SaveBindings: wrote {} binding(s) to {}", bindings.size(), GetBindsFileName());
			LogActivity("Saved " + std::to_string(bindings.size()) + " binding(s) to " + GetBindsFileName());
		}

		void LoadBindings() {
			std::ifstream inFile(GetBindsFileName());
			if (!inFile) {
				spdlog::debug("KeyBindMenu::LoadBindings: no {} yet (fine on first run)", GetBindsFileName());
				LogActivity("No existing bindings file yet at " + GetBindsFileName() + " (fine on first run)");
				return;
			}

			std::string line;
			while (std::getline(inFile, line)) {
				if ((line = trim(line)).empty()) continue;
				std::vector<std::string> parts = split(line, "|");
				if (parts.size() < 2) {
					spdlog::warn("KeyBindMenu::LoadBindings: skipping malformed line: {}", line);
					LogActivity("Skipped malformed line while loading bindings: " + line, false);
					continue;
				}

				if (parts[0] == "MENUKEY") {
					int vKey = VKeyForName(trim(parts[1]));
					if (vKey != -1) menuToggleVKey = (USHORT)vKey;
					else {
						spdlog::warn("KeyBindMenu::LoadBindings: unknown MENUKEY name '{}', keeping default", parts[1]);
						LogActivity("Unknown MENUKEY name '" + parts[1] + "', keeping default", false);
					}
				}
				else if (parts[0] == "BIND" && parts.size() >= 7) {
					std::string keyName = trim(parts[1]); int vKey = VKeyForName(keyName);
					if (vKey == -1) {
						spdlog::warn("KeyBindMenu::LoadBindings: unknown key name '{}', skipping binding", keyName);
						LogActivity("Unknown Key name '" + keyName + "', skipping binding", false);
						continue;
					}
					
					float holdSeconds = 0.0f;
					try { holdSeconds = std::stof(trim(parts[5])); }
					catch (...) { holdSeconds = 0.0f; }
					bool isToggle = false;
					std::string pathOn = "";
					std::string pathOff = "";
					std::string funcOn = "";
					std::string funcOff = "";
					std::string funcTap = "";
					
					if (trim(parts[6]) == "1" && parts.size() >= 9) {
						isToggle = true;
						pathOn = trim(parts[7]);
						pathOff = trim(parts[8]);
						if (parts.size() >= 11) {
							funcOn = trim(parts[9]);
							funcOff = trim(parts[10]);
						}
					} else {
						isToggle = false;
						pathOn = trim(parts[7]);
						pathOff = "";
						if (parts.size() >= 9) {
							funcTap = trim(parts[8]);
						}
					}
					
					std::string resolvedOn = ResolveScriptPath(pathOn);
					std::string resolvedOff = pathOff.empty() ? "" : ResolveScriptPath(pathOff);

					KeyBind b{ (USHORT)vKey, trim(parts[2]) == "1", trim(parts[3]) == "1", trim(parts[4]) == "1", keyName, isToggle, resolvedOn, resolvedOff, false, holdSeconds };
					b.functionOn = funcOn;
					b.functionOff = funcOff;
					b.functionTap = funcTap;
					bindings.push_back(b);
				}
				else if (parts[0] == "BIND") {
					spdlog::warn("KeyBindMenu::LoadBindings: skipping old-format/malformed BIND line: {}", line);
					LogActivity("Skipped old-format/malformed BIND line: " + line, false);
				}
			}
			spdlog::debug("KeyBindMenu::LoadBindings: loaded {} binding(s) from {}", bindings.size(), GetBindsFileName());
			LogActivity("Loaded " + std::to_string(bindings.size()) + " binding(s) from " + GetBindsFileName());
			MarkDisplayCacheDirty();
		}

		void AddBinding(USHORT vKey, const std::string& keyName, bool needCtrl, bool needShift, bool needAlt, float holdSeconds, bool isToggle, const std::string& pathOn, const std::string& pathOff, const std::string& funcOn, const std::string& funcOff, const std::string& funcTap) {
			KeyBind b{ vKey, needCtrl, needShift, needAlt, keyName, isToggle, pathOn, pathOff, false, holdSeconds };
			b.functionOn = funcOn;
			b.functionOff = funcOff;
			b.functionTap = funcTap;

			bindings.push_back(b);
			EnsureDispatcherRegistered(vKey);
			SaveBindings();
			MarkDisplayCacheDirty();
			DebuggerMenu::LogBindEvent("Nound " + CombinedDisplayName(bindings.back()) + " -> mode toggle: " + (isToggle ? "YES" : "NO"));
			LogActivity("Bound " + CombinedDisplayName(bindings.back()) + " (toggle: " + (isToggle ? "YES" : "NO") + ")");
		}

		void RemoveBinding(int index) {
			if (index < 0 || index >= (int)bindings.size()) {
				LogActivity("Attempted to remove binding at invalid index " + std::to_string(index), false);
				return;
			}
			
			std::string removedDesc = CombinedDisplayName(bindings[index]) + " -> " + bindings[index].scriptPathOn;
			USHORT vKey = bindings[index].vKey;
			bindings.erase(bindings.begin() + index);
			RemoveDispatcherIfUnused(vKey);
			SaveBindings();
			MarkDisplayCacheDirty();
			DebuggerMenu::LogBindEvent("Unbound " + removedDesc);
			LogActivity("Unbound " + removedDesc);
		}

		void RemoveAllBindings() {
			size_t count = bindings.size();
			for (const auto& entry : vKeyDispatchers) RawInput::UnRegisterAction(entry.first, entry.second);
			vKeyDispatchers.clear();
			bindings.clear();
			SaveBindings();
			MarkDisplayCacheDirty();
			DebuggerMenu::LogBindEvent("unbound all (" + std::to_string(count) + " binding(s))");
			LogActivity("Cleared all bindings (" + std::to_string(count) + " binding(s))");
		}

		void Init(const std::string& defaultMenuKeyName) {
			LogActivity("RadarKeys KeyBindMenu initializing");
			int defaultVKey = VKeyForName(defaultMenuKeyName);
			if (defaultVKey != -1) menuToggleVKey = (USHORT)defaultVKey;
			else if (!defaultMenuKeyName.empty()) {
				spdlog::warn("KeyBindMenu::Init: unknown keyBindMenuToggleKey '{}' in ihhook_config.lua, using F4", defaultMenuKeyName);
				LogActivity("Unknown keyBindMenuToggleKey '" + defaultMenuKeyName + "' in ihhook_config.lua, using default", false);
			}

			LoadBindings();
			for (const auto& bind : bindings) EnsureDispatcherRegistered(bind.vKey);
			RegisterMenuToggleKey(menuToggleVKey);
			LogActivity("Menu hotkey set to " + NameForVKey(menuToggleVKey));
		}

		static USHORT capturedVKey = 0;
		static bool capturedCtrl = false;
		static bool capturedShift = false;
		static bool capturedAlt = false;
		static float capturedHoldSeconds = 0.0f;
		static char capturedScriptPathOnBuffer[512] = "";
		static char capturedScriptPathOffBuffer[512] = "";
		static char capturedFuncOnBuffer[128] = "";
		static char capturedFuncOffBuffer[128] = "";
		static char capturedFuncTapBuffer[128] = "";
		static bool capturedToggleMode = false;
		static bool capturedLongPressMode = false;
		static bool capturedHasFuncOn = false;
		static bool capturedHasFuncOff = false;
		static int capturedToggleType = 0; 
		static int editingBindingIndex = -1;
		static bool showModKeyCapturePrompt = false;
		static std::string modKeyCaptureScriptName;
		static std::string modKeyCaptureFunctionName;
		static USHORT modKeyCaptureVKey = 0;
		static USHORT modKeyCaptureOldVKey = 0;

		void DrawModKeyCapturePrompt() {
			ImGui::SetNextWindowSize(ImVec2(300, 160), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f - 150, ImGui::GetIO().DisplaySize.y * 0.5f - 80), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowFocus();

			if (!ImGui::Begin("Reassign mod key...", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
				ImGui::End();
				return;
			}

			ImGui::TextWrapped("%s [%s]", modKeyCaptureScriptName.c_str(), modKeyCaptureFunctionName.c_str());
			ImGui::Separator();

			if (modKeyCaptureVKey == 0) {
				if (ImGui::IsMouseClicked(2)) { modKeyCaptureVKey = VK_MBUTTON; }
				else if (ImGui::IsMouseClicked(3)) { modKeyCaptureVKey = VK_XBUTTON1; }
				else if (ImGui::IsMouseClicked(4)) { modKeyCaptureVKey = VK_XBUTTON2; }
				else {
					for (int i = 1; i < 256; i++) {
						if (i == VK_CONTROL || i == VK_SHIFT || i == VK_MENU || i == VK_LWIN || i == VK_RWIN ||
							i == VK_LCONTROL || i == VK_RCONTROL || i == VK_LSHIFT || i == VK_RSHIFT || i == VK_LMENU || i == VK_RMENU) {
							continue;
						}
						if (i == VK_LBUTTON && ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) continue;
						if (ImGui::IsKeyPressed((ImGuiKey)i)) {
							modKeyCaptureVKey = (USHORT)i;
							break;
						}
					}
				}
			}

			ImGui::BeginChild("ModKeyDisplayFrame", ImVec2(0, 50), true, ImGuiWindowFlags_NoScrollbar);
			auto [availWidth, availHeight] = ImGui::GetContentRegionAvail();
			if (modKeyCaptureVKey == 0) {
				ImGui::SetCursorPosY((availHeight - ImGui::GetTextLineHeight()) * 0.5f);
				ImGui::SetCursorPosX((availWidth - ImGui::CalcTextSize("PRESS A KEY...").x) * 0.5f);
				ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "PRESS A KEY...");
			}
			else {
				std::string keyName = NameForVKey(modKeyCaptureVKey);
				ImGui::SetCursorPosY((availHeight - ImGui::GetTextLineHeight()) * 0.5f);
				ImGui::SetCursorPosX((availWidth - ImGui::CalcTextSize(keyName.c_str()).x) * 0.5f);
				ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%s", keyName.c_str());
			}
			ImGui::EndChild();

			bool canFinalize = (modKeyCaptureVKey != 0);
			if (!canFinalize) ImGui::BeginDisabled();
			if (ImGui::Button("Assign", ImVec2(130, 24))) {
				ModKeyBindings::SetOverride(modKeyCaptureScriptName, modKeyCaptureFunctionName, NameForVKey(modKeyCaptureVKey));
				DebuggerMenu::LogBindEvent("Mod key reassigned: " + modKeyCaptureScriptName + " [" + modKeyCaptureFunctionName + "] -> " + NameForVKey(modKeyCaptureVKey));
				LogActivity("Mod key reassigned: " + modKeyCaptureScriptName + " [" + modKeyCaptureFunctionName + "] -> " + NameForVKey(modKeyCaptureVKey));
				if (modKeyCaptureVKey != modKeyCaptureOldVKey) {
					LuaKeyState::HideFromTrackedList(modKeyCaptureOldVKey);
				}
				modKeyCaptureVKey = 0;
				showModKeyCapturePrompt = showCapturePrompt = false;
			}
			if (!canFinalize) ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(130, 24))) {
				modKeyCaptureVKey = 0;
				showModKeyCapturePrompt = showCapturePrompt = false;
				LogActivity("Mod key reassignment cancelled");
			}
			ImGui::TextDisabled("Takes effect immediately, as long as the script re-checks its key every frame.");
			ImGui::End();
		}

		void DrawKeyCapturePrompt() {
			ImGui::SetNextWindowSize(ImVec2(340, 330), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f - 170, ImGui::GetIO().DisplaySize.y * 0.5f - 165), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints(ImVec2(340, 330), ImVec2(FLT_MAX, FLT_MAX));
			ImGui::SetNextWindowFocus();
		
			if (!ImGui::Begin("Assigning key bind...", nullptr, ImGuiWindowFlags_NoCollapse)) {
				ImGui::End();
				return;
			}
		
			if (capturedVKey == 0) {
				capturedCtrl  = ImGui::GetIO().KeyCtrl;
				capturedShift = ImGui::GetIO().KeyShift;
				capturedAlt   = ImGui::GetIO().KeyAlt;
		
				if (ImGui::IsMouseClicked(2)) { capturedVKey = VK_MBUTTON; }
				else if (ImGui::IsMouseClicked(3)) { capturedVKey = VK_XBUTTON1; }
				else if (ImGui::IsMouseClicked(4)) { capturedVKey = VK_XBUTTON2; }
				else {
					for (int i = 1; i < 256; i++) {
						if (i == VK_CONTROL || i == VK_SHIFT || i == VK_MENU || i == VK_LWIN || i == VK_RWIN ||
							i == VK_LCONTROL || i == VK_RCONTROL || i == VK_LSHIFT || i == VK_RSHIFT || i == VK_LMENU || i == VK_RMENU) {
							continue;
						}
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
			}
		
			ImGui::BeginChild("KeyDisplayFrame", ImVec2(105, 95), true, ImGuiWindowFlags_NoScrollbar);
			auto [availWidth, availHeight] = ImGui::GetContentRegionAvail();
			ImGui::SetCursorPosX((availWidth - ImGui::CalcTextSize("Key").x) * 0.5f); ImGui::Text(" Key"); ImGui::Separator();
			float lowerBoxTopY = ImGui::GetCursorPosY(), lowerBoxRemainingHeight = availHeight - lowerBoxTopY;
		
			if (capturedVKey == 0) {
				float startVerticalY = lowerBoxTopY + ((lowerBoxRemainingHeight - (ImGui::GetTextLineHeightWithSpacing() * 2.0f)) * 0.5f);
				ImGui::SetNextItemOpen(true); 
				ImGui::SetCursorPosY(startVerticalY);
				ImGui::SetCursorPosX((availWidth - ImGui::CalcTextSize("PRESS").x) * 0.5f); ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), " PRESS");
				ImGui::SetCursorPosX((availWidth - ImGui::CalcTextSize("KEY...").x) * 0.5f); ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "  KEY..");
			} else {
				std::string keyName = NameForVKey(capturedVKey);
				ImGui::SetCursorPosY(lowerBoxTopY + ((lowerBoxRemainingHeight - ImGui::GetTextLineHeight()) * 0.5f));
				ImGui::SetCursorPosX((availWidth - ImGui::CalcTextSize(keyName.c_str()).x) * 0.5f); ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), " %s", keyName.c_str());
			}
			ImGui::EndChild(); ImGui::SameLine();
		
			ImGui::BeginGroup();
			if (!isAssigningMenuToggleKey) {
				ImGui::Checkbox("Ctrl", &capturedCtrl); ImGui::Checkbox("Shift", &capturedShift); ImGui::Checkbox("Alt", &capturedAlt); 
			}
			
			if (ImGui::Button("Reset", ImVec2(55, 22))) { 
			    capturedVKey = 0; 
			    capturedCtrl = capturedShift = capturedAlt = capturedToggleMode = capturedLongPressMode = capturedHasFuncOn = capturedHasFuncOff = false; 
			    capturedHoldSeconds = 0.0f;
			    capturedToggleType = 0;
			    LogActivity("Keybind has been reset");
			}
			ImGui::EndGroup(); ImGui::SameLine(205);
		
			ImGui::BeginGroup();
			if (!isAssigningMenuToggleKey) {
				ImGui::Checkbox("Toggle", &capturedToggleMode);
				ImGui::Checkbox("Long Press", &capturedLongPressMode);
				
				if (capturedLongPressMode) {
				    ImGui::SetNextItemWidth(75);
				    ImGui::InputFloat("##capturedHoldInput", &capturedHoldSeconds, 0.0f, 0.0f, "%.1fs");
				    if (capturedHoldSeconds < 0.0f) capturedHoldSeconds = 0.0f;
				    if (ImGui::Button(" - ", ImVec2(35, 20))) { if ((capturedHoldSeconds -= 0.5f) < 0.0f) capturedHoldSeconds = 0.0f; } ImGui::SameLine(40);
				    if (ImGui::Button(" + ", ImVec2(35, 20))) capturedHoldSeconds += 0.5f;
				}
			}
			
			bool comboAvailable = true;
			if (capturedVKey != 0) {
				if (isAssigningMenuToggleKey) {
					if (capturedVKey == VK_F2 || capturedVKey == VK_F3 || capturedVKey == VK_ESCAPE) {
						comboAvailable = false;
					}
				} 
				else if (editingBindingIndex != -1) {
					static int lastCheckedIndex = -1;
					if (lastCheckedIndex != editingBindingIndex) {
						capturedHasFuncOn = !bindings[editingBindingIndex].functionOn.empty() || !bindings[editingBindingIndex].functionTap.empty();
						capturedHasFuncOff = !bindings[editingBindingIndex].functionOff.empty();
						lastCheckedIndex = editingBindingIndex;
					}

					if (IsReservedVKey(capturedVKey)) comboAvailable = false;
					for (int i = 0; i < (int)bindings.size(); i++) {
						if (i == editingBindingIndex) continue;
						const auto& b = bindings[i];
						if (b.vKey == capturedVKey && b.needCtrl == capturedCtrl && b.needShift == capturedShift && b.needAlt == capturedAlt) {
							if (b.holdSeconds == capturedHoldSeconds || ((capturedHoldSeconds > 0.0f) == (b.holdSeconds > 0.0f))) comboAvailable = false;
						}
					}
				} else {
					comboAvailable = IsComboAvailable(capturedVKey, capturedCtrl, capturedShift, capturedAlt, capturedHoldSeconds);
				}
			}
			ImGui::Spacing();
			
			const char* comboStatusLabel = comboAvailable ? "[ READY ]" : "[ UNFIT ]";
			ImVec2 comboStatusSize = ImGui::CalcTextSize(comboStatusLabel);
			ImGui::BeginChild("ComboStatusBox", ImVec2(comboStatusSize.x + ImGui::GetStyle().WindowPadding.x * 2.0f, comboStatusSize.y + ImGui::GetStyle().WindowPadding.y * 2.0f), true, ImGuiWindowFlags_NoScrollbar);
			ImGui::TextColored(comboAvailable ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", comboStatusLabel);
			ImGui::EndChild();
			
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(comboAvailable ? "The key combination is valid. Key assignment can finalize." : "Conflict! Key combination is already in use.\nYou can adjust it to be a Long Press by adding duration.");
			}
			ImGui::EndGroup(); ImGui::Separator();
		
			bool isUpperPathValid = false, isLowerPathValid = false;
			int scriptStatus = 0, lowerStatus = 0;
			static char lastStatCheckedOnBuffer[512] = "";
			static int cachedOnStatus = 0;
			static char lastStatCheckedOffBuffer[512] = "";
			static int cachedOffStatus = 0;
		
			if (!isAssigningMenuToggleKey) {
				if (capturedScriptPathOnBuffer[0] != '\0') {
					std::string txt(capturedScriptPathOnBuffer);
					if (txt.size() >= 4 && txt.substr(txt.size() - 4) == ".lua") {
						if (strcmp(capturedScriptPathOnBuffer, lastStatCheckedOnBuffer) != 0) {
							cachedOnStatus = std::filesystem::exists(ResolveScriptPath(capturedScriptPathOnBuffer)) ? 3 : 2;
							snprintf(lastStatCheckedOnBuffer, sizeof(lastStatCheckedOnBuffer), "%s", capturedScriptPathOnBuffer);
						}
						scriptStatus = cachedOnStatus;
						isUpperPathValid = (scriptStatus == 3);
					} else { scriptStatus = 1; lastStatCheckedOnBuffer[0] = '\0'; }
				} else lastStatCheckedOnBuffer[0] = '\0';

				if (capturedToggleMode && capturedScriptPathOffBuffer[0] != '\0') {
					std::string txt(capturedScriptPathOffBuffer);
					if (txt.size() >= 4 && txt.substr(txt.size() - 4) == ".lua") {
						if (strcmp(capturedScriptPathOffBuffer, lastStatCheckedOffBuffer) != 0) {
							cachedOffStatus = std::filesystem::exists(ResolveScriptPath(capturedScriptPathOffBuffer)) ? 3 : 2;
							snprintf(lastStatCheckedOffBuffer, sizeof(lastStatCheckedOffBuffer), "%s", capturedScriptPathOffBuffer);
						}
						lowerStatus = cachedOffStatus;
						isLowerPathValid = (lowerStatus == 3);
					} else { lowerStatus = 1; lastStatCheckedOffBuffer[0] = '\0'; }
				} else lastStatCheckedOffBuffer[0] = '\0';
			}
		
			if (!isAssigningMenuToggleKey) {
				float paddingX = ImGui::GetStyle().WindowPadding.x;
				float totalWidth = ImGui::GetWindowWidth();
				float rightEdgeX = totalWidth - paddingX;
				float elementWidth = 145.0f;
				float targetCursorPosX = rightEdgeX - elementWidth - 8.0f;

				if (capturedToggleMode) {
					ImGui::AlignTextToFramePadding();
					ImGui::Text("Script Mode:"); ImGui::SameLine();
					ImGui::RadioButton("Single", &capturedToggleType, 0); ImGui::SameLine();
					ImGui::RadioButton("Dual", &capturedToggleType, 1);
					ImGui::Separator(); ImGui::Spacing();

					if (capturedToggleType == 0) {
						ImGui::Checkbox("##hasFuncOn", &capturedHasFuncOn); ImGui::SameLine();
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("Target specific global functions inside the file");
						ImGui::SameLine(); ImGui::Text("Script Path:");
						
						ImGui::SetNextItemWidth(-1);
						ImGui::InputText("##captureScriptInputOn", capturedScriptPathOnBuffer, IM_ARRAYSIZE(capturedScriptPathOnBuffer));
						
						if (capturedHasFuncOn) {
							ImGui::Text("Enable Function:"); ImGui::SameLine(targetCursorPosX);
							ImGui::SetNextItemWidth(elementWidth);
							ImGui::InputText("##captureFuncOn", capturedFuncOnBuffer, IM_ARRAYSIZE(capturedFuncOnBuffer));
		
							ImGui::Text("Disable Function:"); ImGui::SameLine(targetCursorPosX);
							ImGui::SetNextItemWidth(elementWidth);
							ImGui::InputText("##captureFuncOff", capturedFuncOffBuffer, IM_ARRAYSIZE(capturedFuncOffBuffer));
						}
					} 
					else {
						ImGui::Checkbox("##hasFuncOn", &capturedHasFuncOn); ImGui::SameLine();
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("Target a specific function inside the Enable Script");
						ImGui::SameLine(); ImGui::Text("Enable Script Path:");

						ImGui::SetNextItemWidth(-1);
						ImGui::InputText("##captureScriptInputOn", capturedScriptPathOnBuffer, IM_ARRAYSIZE(capturedScriptPathOnBuffer));
						
						if (capturedHasFuncOn) {
							ImGui::SetCursorPosX(targetCursorPosX);
							ImGui::SetNextItemWidth(elementWidth);
							ImGui::InputText("##captureFuncOn", capturedFuncOnBuffer, IM_ARRAYSIZE(capturedFuncOnBuffer));
						}

						ImGui::Spacing();
						ImGui::Checkbox("##hasFuncOff", &capturedHasFuncOff); ImGui::SameLine();
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("Target a specific function inside the Disable Script");
						
						ImGui::SameLine(); ImGui::Text("Disable Script Path:");
						ImGui::SetNextItemWidth(-1);
						ImGui::InputText("##captureScriptInputOff", capturedScriptPathOffBuffer, IM_ARRAYSIZE(capturedScriptPathOffBuffer));
						
						if (capturedHasFuncOff) {
							ImGui::SetCursorPosX(targetCursorPosX);
							ImGui::SetNextItemWidth(elementWidth);
							ImGui::InputText("##captureFuncOff", capturedFuncOffBuffer, IM_ARRAYSIZE(capturedFuncOffBuffer));
						}
					}
				} 
				else {
					ImGui::Checkbox("##hasFuncTap", &capturedHasFuncOn); ImGui::SameLine();
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Target a specific function inside this script file");
					
					ImGui::SameLine(); ImGui::Text("Script Path:");
					ImGui::SetNextItemWidth(-1);
					ImGui::InputText("##captureScriptInputOn", capturedScriptPathOnBuffer, IM_ARRAYSIZE(capturedScriptPathOnBuffer));
					
					if (capturedHasFuncOn) {
						ImGui::SetCursorPosX(targetCursorPosX);
						ImGui::SetNextItemWidth(elementWidth);
						ImGui::InputText("##captureFuncTap", capturedFuncTapBuffer, IM_ARRAYSIZE(capturedFuncTapBuffer));
					}
				}
			}
		
			bool pathsValid = (capturedToggleMode && capturedToggleType != 0) ? (isUpperPathValid && isLowerPathValid) : isUpperPathValid;
			bool functionsValid = true;
			
			if (!isAssigningMenuToggleKey) {
				if (capturedToggleMode) {
					if (capturedToggleType == 0) {
						if (capturedHasFuncOn && capturedFuncOnBuffer[0] == '\0') functionsValid = false;
						if (capturedHasFuncOn && capturedFuncOffBuffer[0] == '\0') functionsValid = false;
					} else {
						if (capturedHasFuncOn && capturedFuncOnBuffer[0] == '\0') functionsValid = false;
						if (capturedHasFuncOff && capturedFuncOffBuffer[0] == '\0') functionsValid = false;
					}
				} else {
					if (capturedHasFuncOn && capturedFuncTapBuffer[0] == '\0') functionsValid = false;
				}
			}
			
			bool canFinalize = capturedVKey != 0 && comboAvailable && (isAssigningMenuToggleKey || (pathsValid && functionsValid));
			float paddingY = ImGui::GetStyle().WindowPadding.y;
			float buttonHeight = 30.0f;
			float bottomAnchorY = ImGui::GetWindowHeight() - paddingY - buttonHeight;
			
			ImGui::SetCursorPosY(bottomAnchorY);

			if (!canFinalize) ImGui::BeginDisabled();
			if (ImGui::Button("Finalize", ImVec2(145, buttonHeight))) {
				if (isAssigningMenuToggleKey) {
					if (menuToggleHandle != 0) RawInput::UnRegisterAction(menuToggleVKey, menuToggleHandle);
					menuToggleVKey = capturedVKey; 
					menuToggleHandle = RawInput::RegisterAction(menuToggleVKey, OnMenuToggleKeyPressed);
					SaveBindings(); 
					DebuggerMenu::LogBindEvent("Menu Hotkey reassigned to: " + NameForVKey(capturedVKey));
					LogActivity("Menu Hotkey reassigned to " + NameForVKey(capturedVKey));
					capturedToggleMode = capturedLongPressMode = false;
				} else {
					float finalHoldSeconds = capturedLongPressMode ? capturedHoldSeconds : 0.0f;
				
					if (capturedToggleMode && capturedToggleType == 0) {
						snprintf(capturedScriptPathOffBuffer, sizeof(capturedScriptPathOffBuffer), "%s", capturedScriptPathOnBuffer);
						capturedHasFuncOff = capturedHasFuncOn;
					}
					
					std::string finalPathOn = ResolveScriptPath(capturedScriptPathOnBuffer);
					std::string finalPathOff = capturedToggleMode ? ResolveScriptPath(capturedScriptPathOffBuffer) : "";
					std::string finalFuncOn  = (capturedToggleMode && capturedHasFuncOn) ? capturedFuncOnBuffer : "";
					std::string finalFuncOff = (capturedToggleMode && capturedHasFuncOff) ? capturedFuncOffBuffer : "";
					std::string finalFuncTap = (!capturedToggleMode && capturedHasFuncOn) ? capturedFuncTapBuffer : "";

					if (editingBindingIndex != -1) {
						USHORT oldVKey = bindings[editingBindingIndex].vKey;
						KeyBind editedBind{ capturedVKey, capturedCtrl, capturedShift, capturedAlt, NameForVKey(capturedVKey), capturedToggleMode, finalPathOn, finalPathOff, false, finalHoldSeconds };
						editedBind.functionOn = finalFuncOn;
						editedBind.functionOff = finalFuncOff;
						editedBind.functionTap = finalFuncTap;
						
						bindings[editingBindingIndex] = editedBind;
						RemoveDispatcherIfUnused(oldVKey); EnsureDispatcherRegistered(capturedVKey); SaveBindings();
						MarkDisplayCacheDirty();
						LogActivity("Edited binding -> " + CombinedDisplayName(editedBind));
					} else {
						AddBinding(capturedVKey, NameForVKey(capturedVKey), capturedCtrl, capturedShift, capturedAlt, finalHoldSeconds, capturedToggleMode, finalPathOn, finalPathOff, finalFuncOn, finalFuncOff, finalFuncTap);
					}
					capturedScriptPathOnBuffer[0] = capturedScriptPathOffBuffer[0] = '\0';
					capturedFuncOnBuffer[0] = capturedFuncOffBuffer[0] = capturedFuncTapBuffer[0] = '\0'; 
					capturedToggleMode = capturedLongPressMode = capturedHasFuncOn = capturedHasFuncOff = false;
				}
				capturedVKey = 0; capturedHoldSeconds = 0.0f; 
				showCapturePrompt = isAssigningMenuToggleKey = false; editingBindingIndex = -1;
			}
			if (!canFinalize) ImGui::EndDisabled(); ImGui::SameLine();
			
			float paddingX = ImGui::GetStyle().WindowPadding.x;
			float targetCancelX = ImGui::GetWindowWidth() - paddingX - 145.0f - 8.0f;
			ImGui::SameLine(targetCancelX);
			
			if (ImGui::Button("Cancel", ImVec2(145, buttonHeight))) {
				capturedVKey = 0; capturedHoldSeconds = 0.0f; 
				capturedScriptPathOnBuffer[0] = capturedScriptPathOffBuffer[0] = '\0';
				capturedFuncOnBuffer[0] = capturedFuncOffBuffer[0] = capturedFuncTapBuffer[0] = '\0'; 
				capturedToggleMode = capturedLongPressMode = capturedHasFuncOn = capturedHasFuncOff = false;
				showCapturePrompt = isAssigningMenuToggleKey = false; editingBindingIndex = -1;
				LogActivity("Key Assignment Prompt cancelled");
			}
			ImGui::End();
		}

		void RebuildDisplayCacheIfNeeded() {
			if (!displayCacheDirty) return;

			displayCache.clear();
			displayCache.reserve(bindings.size());

			for (const auto& bind : bindings) {
				BindingDisplayCache entry;
				entry.itemLabel = CombinedDisplayName(bind);

				if (bind.isToggle) {
				    std::string fileOn = std::filesystem::path(bind.scriptPathOn).filename().string();
				    std::string fileOff = std::filesystem::path(bind.scriptPathOff).filename().string();
				    std::string funcOnStr = bind.functionOn.empty() ? "" : " [" + bind.functionOn + "]";
				    std::string funcOffStr = bind.functionOff.empty() ? "" : " [" + bind.functionOff + "]";
				    std::string holdPrefix = "";
				    if (bind.holdSeconds > 0.0f) {
				        char buf[32];
				        snprintf(buf, sizeof(buf), "(Hold %.1fs) ", bind.holdSeconds);
				        holdPrefix = buf;
				    }
				    if (bind.scriptPathOn == bind.scriptPathOff) {
				        entry.detailText = holdPrefix + "Toggle: " + fileOn + funcOnStr + " <-> " + funcOffStr;
				    } else {
				        entry.detailText = holdPrefix + "Toggle: " + fileOn + funcOnStr + " <-> " + fileOff + funcOffStr;
				    }
				} else {
					std::string fileOn = std::filesystem::path(bind.scriptPathOn).filename().string();
					std::string funcTapStr = bind.functionTap.empty() ? "" : " [" + bind.functionTap + "]";

					entry.detailText = "-> " + fileOn + funcTapStr;
				}
				entry.fullLine = entry.itemLabel + " " + entry.detailText;
				displayCache.push_back(std::move(entry));
			}
			displayCacheDirty = false;
		}

		// draws the ui
		void Draw(bool* p_open) {
			RebuildDisplayCacheIfNeeded();
			float longestItemWidth = 0.0f;
			for (const auto& entry : displayCache) {
				float stringPixelWidth = ImGui::CalcTextSize(entry.fullLine.c_str()).x;
				if (stringPixelWidth > longestItemWidth) {
					longestItemWidth = stringPixelWidth;
				}
			}

			float finalMinWidthFloor = longestItemWidth + 111.0f;
			if (finalMinWidthFloor < 480.0f) {
				finalMinWidthFloor = 480.0f;
			}

			ImGui::SetNextWindowSize(ImVec2(finalMinWidthFloor, 220), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints(ImVec2(finalMinWidthFloor, 220), ImVec2(FLT_MAX, FLT_MAX));

			if (!ImGui::Begin("RadarKeys - Key Bindings", p_open)) { ImGui::End(); return; }
			if (ImGui::Button("Debugger")) {
				DebuggerMenu::menuOpen = !DebuggerMenu::menuOpen;
				LogActivity(DebuggerMenu::menuOpen ? "Debugger Overlay opened" : "Debugger Overlay closed");
			}
			ImGui::SameLine();
			
			std::string buttonLabel = "Menu Hotkey: [" + NameForVKey(menuToggleVKey) + "]";
			if (ImGui::Button(buttonLabel.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { 
				isAssigningMenuToggleKey = showCapturePrompt = true; 
				LogActivity("Menu Key Reassignment Prompt opened");
			}
			ImGui::Separator();

			if (ImGui::CollapsingHeader("Keys Polled by Scripts", ImGuiTreeNodeFlags_DefaultOpen)) {
				std::vector<LuaKeyState::TrackedKeyInfo> trackedKeys = LuaKeyState::GetTrackedKeyInfo();
				ImGui::BeginChild("TrackedScriptKeys", ImVec2(0, 150), true);
				if (trackedKeys.empty()) {
					ImGui::TextDisabled("(none yet - a key shows up here the first time a script queries it via RadarKeys.OnButtonDown/ButtonHeld/etc)");
				}
				else {
					for (const LuaKeyState::TrackedKeyInfo& info : trackedKeys) {
						ImGui::PushID(info.vKey);
						ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
						float rowTopY = ImGui::GetCursorPosY();
						float buttonHeight = 20.0f;
						const float keyColumnX = 250.0f;

						std::string detailText;
						if (info.hasDescription) {
							std::string funcStr = info.functionName.empty() ? "" : " [" + info.functionName + "]";
							detailText = "-> " + info.scriptName + funcStr;
						}
						else {
							detailText = "-> (undescribed - call RadarKeys.DescribeKey(...) to label this)";
						}

						ImGui::SetCursorPos(ImVec2(keyColumnX, rowTopY));
						ImGui::AlignTextToFramePadding();
						ImGui::BeginGroup();
						ImGui::TextWrapped("%s", detailText.c_str());
						ImGui::EndGroup();

						float detailTextHeight = ImGui::GetItemRectSize().y;
						float rowContentHeight = (detailTextHeight > buttonHeight) ? detailTextHeight : buttonHeight;
						float buttonYOffset = (rowContentHeight - buttonHeight) * 0.5f;

						ImVec4 keyNameColor;
						if (info.hasToggleState) {
							keyNameColor = info.toggleEnabled ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
						}
						else {
							keyNameColor = info.isPressed ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
						}

						ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().ItemSpacing.x, rowTopY + buttonYOffset));
						ImGui::BeginDisabled();
						ImGui::Button("Script", ImVec2(55, buttonHeight));
						ImGui::EndDisabled();
						ImGui::SameLine();

						ImGui::PushStyleColor(ImGuiCol_Text, keyNameColor);
						if (info.hasDescription) {
							if (ImGui::Button(NameForVKey(info.vKey).c_str(), ImVec2(130, buttonHeight))) {
								modKeyCaptureScriptName = info.scriptName;
								modKeyCaptureFunctionName = info.functionName;
								modKeyCaptureVKey = 0;
								modKeyCaptureOldVKey = info.vKey;
								showModKeyCapturePrompt = true;
								showCapturePrompt = true;
							}
							if (ImGui::IsItemHovered()) {
								ImGui::SetTooltip("Click to reassign this key.\nSaved to radar_keybinds_mod.conf - takes effect immediately.");
							}
						}
						else {
							ImGui::BeginDisabled();
							ImGui::Button(NameForVKey(info.vKey).c_str(), ImVec2(130, buttonHeight));
							ImGui::EndDisabled();
							if (ImGui::IsItemHovered()) {
								ImGui::SetTooltip("Can't reassign yet - this key hasn't been described via RadarKeys.DescribeKey(), so there's no script/function to save an override under.");
							}
						}
						ImGui::PopStyleColor();

						ImGui::SetCursorPosY(rowTopY + rowContentHeight + 4.0f);
						ImGui::PopID(); ImGui::Separator();
					}
				}
				ImGui::EndChild();
				ImGui::TextDisabled("Exact name string to use from Lua - compare against what your script has assigned.");
			}
			ImGui::Separator();

			float paddingX = ImGui::GetStyle().WindowPadding.x;
			float paddingY = ImGui::GetStyle().WindowPadding.y;
			float footerHeight = 45.0f; 
			float listRemainingHeight = ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - footerHeight - paddingY;
			if (listRemainingHeight < 100.0f) listRemainingHeight = 100.0f; 

			int removeIndex = -1;
			ImGui::BeginChild("ActiveBindingsOverviewList", ImVec2(0, listRemainingHeight), true);
			for (int i = 0; i < (int)bindings.size(); i++) {
				ImGui::PushID(i);
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
				float rowTopY = ImGui::GetCursorPosY();
				float buttonHeight = 20.0f;
				const float keyColumnX = 250.0f;

				ImGui::SetCursorPos(ImVec2(keyColumnX, rowTopY));
				ImGui::AlignTextToFramePadding();
				ImGui::BeginGroup();
				ImGui::TextWrapped("%s", displayCache[i].detailText.c_str());
				ImGui::EndGroup();

				float detailTextHeight = ImGui::GetItemRectSize().y;
				float rowContentHeight = (detailTextHeight > buttonHeight) ? detailTextHeight : buttonHeight;
				float buttonYOffset = (rowContentHeight - buttonHeight) * 0.5f;

				ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().ItemSpacing.x, rowTopY + buttonYOffset));
				if (ImGui::Button("Remove", ImVec2(55, buttonHeight))) removeIndex = i;
				ImGui::SameLine(); 

				const std::string& itemLabel = displayCache[i].itemLabel;
				if (ImGui::Button(itemLabel.c_str(), ImVec2(130, buttonHeight))) {
					editingBindingIndex = i;
					capturedVKey = bindings[i].vKey;
					capturedCtrl = bindings[i].needCtrl;
					capturedShift = bindings[i].needShift;
					capturedAlt = bindings[i].needAlt;
					capturedToggleMode = bindings[i].isToggle;
					capturedLongPressMode = (bindings[i].holdSeconds > 0.0f);
					capturedHoldSeconds = bindings[i].holdSeconds;
					capturedHasFuncOn = !bindings[i].functionOn.empty() || !bindings[i].functionTap.empty();
					capturedHasFuncOff = !bindings[i].functionOff.empty();
					
					snprintf(capturedScriptPathOnBuffer, sizeof(capturedScriptPathOnBuffer), "%s", bindings[i].scriptPathOn.c_str());
					snprintf(capturedScriptPathOffBuffer, sizeof(capturedScriptPathOffBuffer), "%s", bindings[i].scriptPathOff.c_str());
					snprintf(capturedFuncOnBuffer, sizeof(capturedFuncOnBuffer), "%s", bindings[i].functionOn.c_str());
					snprintf(capturedFuncOffBuffer, sizeof(capturedFuncOffBuffer), "%s", bindings[i].functionOff.c_str());
					snprintf(capturedFuncTapBuffer, sizeof(capturedFuncTapBuffer), "%s", bindings[i].functionTap.c_str());
					
					if (bindings[i].isToggle) {
						capturedToggleType = (bindings[i].scriptPathOn == bindings[i].scriptPathOff) ? 0 : 1;
					} else {
						capturedToggleType = 0;
					}
					
					showCapturePrompt = true;
					LogActivity("Key Assignment Edit Prompt opened " + itemLabel);
				}

				ImGui::SetCursorPosY(rowTopY + rowContentHeight + 4.0f);
				ImGui::PopID(); ImGui::Separator();
			}
			ImGui::EndChild();

			if (removeIndex != -1) RemoveBinding(removeIndex);
			float bottomControlPanelY = ImGui::GetWindowHeight() - paddingY - 35.0f; 
			ImGui::SetCursorPosY(bottomControlPanelY);
			
			if (bindings.empty()) ImGui::BeginDisabled();
			if (ImGui::Button("Clear All Hotkeys", ImVec2(145, 24))) RemoveAllBindings();
			if (bindings.empty()) ImGui::EndDisabled();
			
			ImGui::SameLine(ImGui::GetContentRegionMax().x - 165.0f);
			if (ImGui::Button("Add New Binding...", ImVec2(165, 24))) {
				editingBindingIndex = -1;
				showCapturePrompt = true;
				LogActivity("Key Assignment Binding Prompt opened");
			}
			ImGui::End();

			if (showModKeyCapturePrompt) DrawModKeyCapturePrompt();
			else if (showCapturePrompt) DrawKeyCapturePrompt();
		}
	}
}
