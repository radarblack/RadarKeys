#include "KeyBindMenu.h"
#include "RawInput.h"
#include "LuaBridge.h"
#include "DebuggerMenu.h"
#include "LuaKeyState.h"
#include "ModKeyBindings.h"
#include "ModInfoRegistry.h"
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
#include <cctype>
#include <sstream>
#include <deque>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

namespace RadarKeys {
	bool showCapturePrompt = false; 
	namespace KeyBindMenu {
		std::vector<KeyBind> bindings;
		static float capturedHoldSeconds = 0.0f;
		static bool capturedInstantMode = false;
		static int capturedInstantTriggerType = 0;
		static bool isAssigningMenuToggleKey = false; 
		static bool isAssigningModKey = false;
		static bool requestCaptureFocus = false;

		// ==================== UI Strings ====================
		static const char* UI_FMT_SCRIPT_FUNCTION_BRACKETS = "%s [%s]";
		static const char* UI_LBL_BIND_TYPE = "Bind Type:";
		static const char* UI_RADIO_SINGLE_KEY = "Single Key";
		static const char* UI_RADIO_MULTI_KEY_COMBO = "Multi-Key Combo";
		static const char* UI_LBL_KEY = "Key";
		static const char* UI_LBL_PRESS = "PRESS";
		static const char* UI_LBL_HOLD = "HOLD";
		static const char* UI_LBL_KEY_ELLIPSIS = "KEY...";
		static const char* UI_CHK_CTRL = "Ctrl";
		static const char* UI_CHK_SHIFT = "Shift";
		static const char* UI_CHK_ALT = "Alt";
		static const char* UI_BTN_RESET = "Reset";
		static const char* UI_LBL_KEYS = "Keys";
		static const char* UI_LBL_HOLD_INDENT = "  HOLD";
		static const char* UI_LBL_2_3_KEYS = "  2-3 KEYS";
		static const char* UI_TIP_COMBO_HOLD = "Hold every key in the combo down for %.1fs.\nReleasing any key before then cancels the capture.";
		static const char* UI_CHK_TOGGLE = "Toggle";
		static const char* UI_TIP_UNCHECK_INSTANT_FIRST = "Uncheck Instant first to use Toggle or Long Press.";
		static const char* UI_CHK_LONG_PRESS = "Long Press";
		static const char* UI_BTN_MINUS = " - ";
		static const char* UI_BTN_PLUS = " + ";
		static const char* UI_CHK_INSTANT = "Instant";
		static const char* UI_TIP_UNCHECK_TOGGLE_FIRST = "Uncheck Toggle and Long Press first to use Instant.";
		static const char* UI_OPT_ON_PRESS = "On Press";
		static const char* UI_OPT_ON_RELEASE = "On Release";
		static const char* UI_OPT_REPEAT = "Repeat";
		static const char* UI_TIP_REPEAT_ACCEL =
			"Acceleration multiplier for the Repeat interval.\n"
			"Each time the repeat fires, the wait before the next fire\n"
			"is divided by this amount - values above 1.00x make it fire\n"
			"progressively faster the longer the key is held (acceleration);\n"
			"values below 1.00x make it fire progressively slower instead\n"
			"(deceleration).\n"
			"1.00x = constant rate (no acceleration or deceleration).\n"
			"e.g. 1.20x ramps up gradually; 2.00x ramps up quickly;\n"
			"0.80x eases off gradually; 0.20x slows down quickly.";
		static const char* UI_TXT_DETECTED_READ_ONLY = "Detected from the script - read only";
		static const char* UI_LBL_READY = "[ READY ]";
		static const char* UI_LBL_UNFIT = "[ UNFIT ]";
		static const char* UI_TIP_COMBO_VALID = "The key combination is valid. Key assignment can finalize.";
		static const char* UI_TIP_COMBO_CONFLICT = "Conflict! Key combination is already in use.\nThis includes bindings declared by mods and existing manual bindings.\nChange the combination before finalizing.";
		static const char* UI_LBL_SCRIPT_MODE = "Script Mode:";
		static const char* UI_RADIO_SINGLE = "Single";
		static const char* UI_RADIO_DUAL = "Dual";
		static const char* UI_TIP_TARGET_GLOBAL_FUNCS = "Target specific global functions inside the file";
		static const char* UI_LBL_SCRIPT_PATH = "Script Path:";
		static const char* UI_LBL_ENABLE_FUNCTION = "Enable Function:";
		static const char* UI_LBL_DISABLE_FUNCTION = "Disable Function:";
		static const char* UI_TIP_TARGET_ENABLE_SCRIPT = "Target a specific function inside the Enable Script";
		static const char* UI_LBL_ENABLE_SCRIPT_PATH = "Enable Script Path:";
		static const char* UI_TIP_TARGET_DISABLE_SCRIPT = "Target a specific function inside the Disable Script";
		static const char* UI_LBL_DISABLE_SCRIPT_PATH = "Disable Script Path:";
		static const char* UI_TIP_TARGET_SCRIPT_FILE = "Target a specific function inside this script file";
		static const char* UI_TXT_DETECTED_FROM_SCRIPT = "Detected from the script:";
		static const char* UI_TXT_NO_KEYS_QUERIED = "(No Keys from any mods are queried by the core module.)";
		static const char* UI_FMT_S = "%s";
		static const char* UI_BTN_FINALIZE = "Finalize";
		static const char* UI_BTN_CANCEL = "Cancel";
		static const char* UI_TXT_TAKES_EFFECT_NEXT_FRAME = "This will immediately take effect once the frame updates after assigning.";
		static const char* UI_WINDOW_TITLE = "RadarKeys - Key Bindings";
		static const char* UI_BTN_DEBUGGER = "Debugger";
		static const char* UI_LOG_DEBUGGER_OPENED = "Debugger Overlay opened";
		static const char* UI_LOG_DEBUGGER_CLOSED = "Debugger Overlay closed";
		static const char* UI_LBL_MENU_HOTKEY_PREFIX = "Menu Hotkey: [";
		static const char* UI_LBL_MENU_HOTKEY_SUFFIX = "]";
		static const char* UI_HDR_KEY_BINDINGS = "Key Bindings";
		static const char* UI_TXT_DESCRIBED_KEYS_HINT = "Keys described from the mod script will put the information in the list.";
		static const char* UI_TXT_NO_KEYS_ASSIGNED = "(No Keys are assigned yet.)";
		static const char* UI_TXT_NOT_YET_DESCRIBED = "-> (Key is not yet described through RadarKeys module.)";
		static const char* UI_BTN_ENABLE = "Enable";
		static const char* UI_BTN_DISABLE = "Disable";
		static const char* UI_TIP_CLICK_HOLD_REMOVE = "Click to %s.\nHold for 1.5 seconds to remove.";
		static const char* UI_WORD_ENABLE = "enable";
		static const char* UI_WORD_DISABLE = "disable";
		static const char* UI_LBL_ERROR = "Error!";
		static const char* UI_TIP_CONFLICT_REASSIGNABLE = "Another binding is using this same key - it's disabled until resolved.\nClick the key name to reassign this one.";
		static const char* UI_TIP_CONFLICT_NOT_DESCRIBED = "Another binding is using this same key - it's disabled until resolved.\nUnable to reassign an override - Key is not yet described through RadarKeys module.";
		static const char* UI_TIP_CLICK_HOLD_RESET = "Click to %s.\nHold for 1.5 seconds to reset to the mod's default key.";
		static const char* UI_TIP_CLICK_NO_REMOVE = "Click to %s.\nMod keys can't be removed - only disabled.";
		static const char* UI_BTN_SCRIPT_PLACEHOLDER = "Script";
		static const char* UI_TIP_REASSIGN_COMBO = "Click to reassign this combo.\nSaved in radar_keybinds.conf in the (...modules/radarKeys) folder";
		static const char* UI_TIP_REASSIGN_KEY = "Click to reassign this key.\nSaved in radar_keybinds.conf in the (...modules/radarKeys) folder";
		static const char* UI_TIP_CANNOT_REASSIGN_UNDESCRIBED = "Unable to reassign an override - Key is not yet described through RadarKeys module.";
		static const char* UI_POPUP_REMOVE_BINDING = "Remove Binding?";
		static const char* UI_FMT_REMOVE_CONFIRM = "Remove \"%s\"?";
		static const char* UI_TXT_CANNOT_BE_UNDONE = "This can't be undone.";
		static const char* UI_TXT_BINDING_NO_LONGER_EXISTS = "This binding no longer exists.";
		static const char* UI_BTN_YES = "Yes";
		static const char* UI_BTN_NO = "No";
		static const char* UI_POPUP_RESET_MOD_KEY = "Reset Mod Key?";
		static const char* UI_FMT_RESET_CONFIRM = "Reset \"%s [%s]\" to the mod's default key?";
		static const char* UI_TXT_CLEARS_REASSIGNMENT = "This clears the reassignment made in this menu.";
		static const char* UI_BTN_CLEAR_ALL_HOTKEYS = "Clear All Hotkeys";
		static const char* UI_BTN_ADD_NEW_BINDING = "Add New Binding...";

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

		void MarkInitializationComplete() {
			FlushActivityLog();
			std::string currentLog = GetLogFileName();
			std::error_code ec;
			size_t newBaseline = std::filesystem::file_size(currentLog, ec);
			if (ec) {
				spdlog::warn("KeyBindMenu::MarkInitializationComplete: couldn't stat {}: {}", currentLog, ec.message());
				return;
			}
			activityLogBaselineBytes = newBaseline;
			activityLogLines.clear();
			activityLogBytes = 0;
			activityLogWritesSinceFlush = 0;
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
			snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", (int)hours.count(), (int)minutes.count(), (int)duration.count());

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
			{"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15}, {"F16", VK_F16},
			{"F17", VK_F17}, {"F18", VK_F18}, {"F19", VK_F19}, {"F20", VK_F20},
			{"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24},

			{"Space", VK_SPACE}, {"Tab", VK_TAB}, {"Enter", VK_RETURN}, {"Backspace", VK_BACK},
			{"Insert", VK_INSERT}, {"Delete", VK_DELETE},
			{"Home", VK_HOME}, {"End", VK_END},
			{"Page Up", VK_PRIOR}, {"Page Down", VK_NEXT},
			{"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},

			{"Numpad 0", VK_NUMPAD0}, {"Numpad 1", VK_NUMPAD1}, {"Numpad 2", VK_NUMPAD2},
			{"Numpad 3", VK_NUMPAD3}, {"Numpad 4", VK_NUMPAD4}, {"Numpad 5", VK_NUMPAD5},
			{"Numpad 6", VK_NUMPAD6}, {"Numpad 7", VK_NUMPAD7}, {"Numpad 8", VK_NUMPAD8},
			{"Numpad 9", VK_NUMPAD9}, {"Numpad *", VK_MULTIPLY}, {"Numpad +", VK_ADD}, 
			{"Numpad -", VK_SUBTRACT}, {"Numpad .", VK_DECIMAL}, {"Numpad /", VK_DIVIDE},

			{",", VK_OEM_COMMA}, {".", VK_OEM_PERIOD},
			{";", VK_OEM_1}, {"=", VK_OEM_PLUS}, {"-", VK_OEM_MINUS}, {"/", VK_OEM_2},
			{"`", VK_OEM_3}, {"[", VK_OEM_4}, {"\\", VK_OEM_5}, {"]", VK_OEM_6}, {"'", VK_OEM_7},

			{"Caps Lock", VK_CAPITAL}, {"Num Lock", VK_NUMLOCK}, {"Scroll Lock", VK_SCROLL},
			{"Print Screen", VK_SNAPSHOT}, {"Pause", VK_PAUSE}, {"Menu Key", VK_APPS},

			{"Volume Up", VK_VOLUME_UP}, {"Volume Down", VK_VOLUME_DOWN}, {"Volume Mute", VK_VOLUME_MUTE},
			{"Media Play/Pause", VK_MEDIA_PLAY_PAUSE}, {"Media Stop", VK_MEDIA_STOP},
			{"Media Next", VK_MEDIA_NEXT_TRACK}, {"Media Previous", VK_MEDIA_PREV_TRACK},

			{"Ctrl", VK_CONTROL}, {"Shift", VK_SHIFT}, {"Alt", VK_MENU},

			{"Mouse Wheel", VK_MBUTTON},
			{"Mouse 4", VK_XBUTTON1},
			{"Mouse 5", VK_XBUTTON2},
			{"Right Click", VK_RBUTTON}
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

		std::vector<USHORT> ParseComboKeyNames(const std::string& raw) {
			std::vector<USHORT> result;
			std::stringstream ss(raw);
			std::string part;
			while (std::getline(ss, part, '+')) {
				while (!part.empty() && std::isspace((unsigned char)part.front())) part.erase(part.begin());
				while (!part.empty() && std::isspace((unsigned char)part.back())) part.pop_back();
				int vKey = VKeyForName(part);
				if (vKey < 0) {
					result.clear();
					return result;
				}
				result.push_back((USHORT)vKey);
			}
			if (result.size() < 2 || result.size() > 3) result.clear();
			return result;
		}

		bool IsMouseVKey(USHORT vKey) {
			return vKey == VK_LBUTTON || vKey == VK_RBUTTON || vKey == VK_MBUTTON ||
				vKey == VK_XBUTTON1 || vKey == VK_XBUTTON2;
		}

		std::vector<USHORT> CanonicalizeComboKeys(const std::vector<USHORT>& keys) {
			std::vector<USHORT> ordered = keys;
			std::stable_sort(ordered.begin(), ordered.end(), [](USHORT a, USHORT b) {
				return IsMouseVKey(a) && !IsMouseVKey(b);
			});
			return ordered;
		}

		std::string ComboKeysDisplayName(const std::vector<USHORT>& keys) {
			std::vector<USHORT> ordered = CanonicalizeComboKeys(keys);
			std::string result;
			for (size_t i = 0; i < ordered.size(); i++) {
				if (i) result += " + ";
				result += NameForVKey(ordered[i]);
			}
			return result;
		}

		bool VectorsEqualUnordered(std::vector<USHORT> a, std::vector<USHORT> b) {
			if (a.size() != b.size()) return false;
			std::sort(a.begin(), a.end());
			std::sort(b.begin(), b.end());
			return a == b;
		}

		std::string CombinedDisplayName(const KeyBind& bind) {
			if (bind.IsCombo()) {
				std::string result = ComboKeysDisplayName(bind.comboKeys);
				if (bind.holdSeconds > 0.0f) {
					char buf[32];
					snprintf(buf, sizeof(buf), " (hold %.1fs)", bind.holdSeconds);
					result += buf;
				}
				return result;
			}
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

		std::unordered_set<USHORT> activeBindVKeys;
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

		enum TriggerMask : unsigned {
			Trigger_None     = 0,
			Trigger_OnPress  = 1u << 0,
			Trigger_OnRelease= 1u << 1,
			Trigger_LongPress= 1u << 2,
			Trigger_Repeat   = 1u << 3
		};

		unsigned ManualTriggerMask(const KeyBind& bind) {
			unsigned mask = Trigger_None;
			if (bind.holdSeconds > 0.0f) {
				mask |= Trigger_LongPress;
				if (bind.isInstant && bind.instantTriggerType == 1) mask |= Trigger_OnRelease;
				else if (bind.isInstant && bind.instantTriggerType == 0) mask |= Trigger_OnRelease;
				if (bind.isInstant && bind.instantTriggerType == 2) mask |= Trigger_Repeat;
			} else if (bind.isInstant) {
				switch (bind.instantTriggerType) {
				case 0: mask |= Trigger_OnPress; break;
				case 1: mask |= Trigger_OnRelease; break;
				case 2: mask |= Trigger_Repeat; break;
				default: mask |= Trigger_OnRelease; break;
				}
			} else {
				mask |= Trigger_OnRelease;
			}
			return mask;
		}

		unsigned ModTriggerMask(const LuaKeyState::TrackedKeyInfo& info) {
			unsigned mask = Trigger_None;
			if (info.usesOnPress) mask |= Trigger_OnPress;
			if (info.usesOnRelease) mask |= Trigger_OnRelease;
			if (info.usesHoldTime) mask |= Trigger_LongPress;
			if (info.usesRepeat) mask |= Trigger_Repeat;
			return mask;
		}

		unsigned ModComboTriggerMask(const LuaKeyState::TrackedComboKeyInfo& info) {
			unsigned mask = Trigger_None;
			if (info.usesOnPress) mask |= Trigger_OnPress;
			if (info.usesOnRelease) mask |= Trigger_OnRelease;
			if (info.usesHoldTime) mask |= Trigger_LongPress;
			if (info.usesRepeat) mask |= Trigger_Repeat;
			return mask;
		}

		unsigned ManualCaptureTriggerMask() {
			KeyBind capture{};
			capture.holdSeconds = capturedHoldSeconds;
			capture.isInstant = capturedInstantMode;
			capture.instantTriggerType = capturedInstantTriggerType;
			return ManualTriggerMask(capture);
		}

		bool IsSingleTriggerConflict(unsigned manualMask, const LuaKeyState::TrackedKeyInfo& info) {
			unsigned modMask = ModTriggerMask(info);
			return modMask != Trigger_None && (manualMask & modMask) != 0;
		}

		bool IsComboTriggerConflict(unsigned manualMask, const LuaKeyState::TrackedComboKeyInfo& info) {
			unsigned modMask = ModComboTriggerMask(info);
			return modMask != Trigger_None && (manualMask & modMask) != 0;
		}

		unsigned GetModSingleTriggerMask(const std::string& scriptName, const std::string& functionName) {
			unsigned mask = Trigger_None;
			for (const auto& info : LuaKeyState::GetTrackedKeyInfo()) {
				if (info.scriptName != scriptName || info.functionName != functionName) continue;
				mask |= ModTriggerMask(info);
			}
			return mask;
		}

		unsigned GetModComboTriggerMask(const std::string& scriptName, const std::string& functionName) {
			unsigned mask = Trigger_None;
			for (const auto& info : LuaKeyState::GetTrackedComboKeyInfo()) {
				if (info.scriptName != scriptName || info.functionName != functionName) continue;
				mask |= ModComboTriggerMask(info);
			}
			return mask;
		}

		bool IsModSingleAssignmentAvailable(USHORT vKey, const std::string& scriptName, const std::string& functionName) {
			if (IsReservedVKey(vKey)) return false;
			unsigned modMask = GetModSingleTriggerMask(scriptName, functionName);
			if (modMask == Trigger_None) return true;

			for (const auto& bind : bindings) {
				if (bind.IsCombo() || bind.vKey != vKey) continue;
				if ((ManualTriggerMask(bind) & modMask) != 0) return false;
			}

			for (const auto& other : LuaKeyState::GetTrackedKeyInfo()) {
				if (!other.hasDescription || other.vKey != vKey) continue;
				if (other.scriptName == scriptName && other.functionName == functionName) continue;
				if ((modMask & ModTriggerMask(other)) != 0) return false;
			}
			return true;
		}

		bool IsModComboAssignmentAvailable(const std::vector<USHORT>& comboKeys, const std::string& scriptName, const std::string& functionName) {
			for (USHORT k : comboKeys) {
				if (IsReservedVKey(k)) return false;
			}
			unsigned modMask = GetModComboTriggerMask(scriptName, functionName);
			if (modMask == Trigger_None) return true;

			for (int i = 0; i < (int)bindings.size(); ++i) {
				const KeyBind& bind = bindings[i];
				if (!bind.IsCombo() || !VectorsEqualUnordered(bind.comboKeys, comboKeys)) continue;
				if ((ManualTriggerMask(bind) & modMask) != 0) return false;
			}

			for (const auto& other : LuaKeyState::GetTrackedComboKeyInfo()) {
				if (!VectorsEqualUnordered(other.activeKeys, comboKeys)) continue;
				if (other.scriptName == scriptName && other.functionName == functionName) continue;
				unsigned otherMask = ModComboTriggerMask(other);
				if ((modMask & otherMask) != 0) return false;
			}
			return true;
		}

		bool IsComboAvailable(USHORT vKey, bool needCtrl, bool needShift, bool needAlt, float holdSeconds, unsigned manualMask, int editingIndex = -1) {
			if (IsReservedVKey(vKey)) return false;
			for (int i = 0; i < (int)bindings.size(); ++i) {
				if (i == editingIndex) continue;
				const auto& bind = bindings[i];
				if (bind.IsCombo()) continue;
				if (bind.vKey == vKey && bind.needCtrl == needCtrl && bind.needShift == needShift && bind.needAlt == needAlt) {
					if (bind.holdSeconds > 0.0f && holdSeconds > 0.0f) return false;
					if (bind.holdSeconds <= 0.0f && holdSeconds <= 0.0f) return false;
				}
			}

			for (const auto& info : LuaKeyState::GetTrackedKeyInfo()) {
				if (!info.hasDescription || info.vKey != vKey) continue;
				if (IsSingleTriggerConflict(manualMask, info)) return false;
			}
			return true;
		}

		bool IsMultiKeyComboAvailable(const std::vector<USHORT>& comboKeys, int editingIndex,
			const std::string& ignoredScriptName = "", const std::string& ignoredFunctionName = "") {
			for (USHORT k : comboKeys) {
				if (IsReservedVKey(k)) return false;
			}
			for (int i = 0; i < (int)bindings.size(); i++) {
				if (i == editingIndex) continue;
				const KeyBind& b = bindings[i];
				if (b.IsCombo() && VectorsEqualUnordered(b.comboKeys, comboKeys)) return false;
			}

			unsigned manualMask = ManualCaptureTriggerMask();
			for (const auto& info : LuaKeyState::GetTrackedComboKeyInfo()) {
				if (!ignoredScriptName.empty() &&
					info.scriptName == ignoredScriptName &&
					info.functionName == ignoredFunctionName) {
					continue;
				}
				if (VectorsEqualUnordered(info.activeKeys, comboKeys) &&
					IsComboTriggerConflict(manualMask, info)) return false;
			}

			return true;
		}

		bool IsComboConflictedWithBindings(const std::vector<USHORT>& comboKeys, int ignoredBindingIndex = -1, unsigned triggerMask = 0) {
			for (int i = 0; i < (int)bindings.size(); ++i) {
				if (i == ignoredBindingIndex) continue;
				const KeyBind& bind = bindings[i];
				if (!bind.IsCombo() || !VectorsEqualUnordered(bind.comboKeys, comboKeys)) continue;
				if (triggerMask == 0 || (ManualTriggerMask(bind) & triggerMask) != 0) return true;
			}
			return false;
		}

		struct PendingPress {
			bool ctrlOnPressed = false;
			bool shiftOnPressed = false;
			bool altOnPressed = false;
			bool holdFired = false;
			bool tapFired = false;
			std::chrono::steady_clock::time_point pressTime;
			std::chrono::steady_clock::time_point lastRepeatTime;
			double repeatSpeedMult = 1.0;
		};
		std::map<USHORT, PendingPress> pendingPresses;

		constexpr double kRepeatIntervalSeconds = 0.3;
		constexpr double kNearMissHoldFraction = 0.8;
		constexpr double kMaxRepeatSpeedMult = 20.0;
		constexpr float kMinRepeatAccelMult = 0.1f;
		constexpr double kMinRepeatSpeedMult = 0.05;
		constexpr double kRemoveHoldSeconds = 1.5;
		const KeyBind* FindMatchingBinding(USHORT vKey, bool ctrlHeld, bool shiftHeld, bool altHeld, bool preferHold) {
			const KeyBind* plainFallbackMatch = nullptr;

			for (const auto& bind : bindings) {
				if (bind.vKey != vKey) continue;
				if (bind.disabled) continue;
				bool categoryMatches = preferHold ? (bind.holdSeconds > 0.0f) : (bind.holdSeconds <= 0.0f);
				if (!categoryMatches) continue;

				if (bind.needCtrl == ctrlHeld && bind.needShift == shiftHeld && bind.needAlt == altHeld) {
					return &bind;
				}
				if (!bind.needCtrl && !bind.needShift && !bind.needAlt) {
					plainFallbackMatch = &bind;
				}
			}
			return plainFallbackMatch;
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
					std::string luaPayload = "CallFunction|" + targetFunc + "|" + targetPath;
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

		USHORT ResolveDisplayVKey(const LuaKeyState::TrackedKeyInfo& info) {
			if (!info.hasDescription) {
				return info.vKey;
			}
			std::string overrideKeyName = ModKeyBindings::GetOverride(info.scriptName, info.functionName);
			if (overrideKeyName.empty()) {
				return info.vKey;
			}
			int resolved = VKeyForName(overrideKeyName);
			return (resolved > 0) ? (USHORT)resolved : info.vKey;
		}

		bool IsVKeySuppressedByActiveCombo(USHORT vKey) {
			for (const auto& bind : bindings) {
				if (!bind.IsCombo()) continue;

				bool isMember = false;
				for (USHORT k : bind.comboKeys) {
					if (k == vKey) { isMember = true; break; }
				}
				if (!isMember) continue;

				bool allHeld = true;
				for (USHORT k : bind.comboKeys) {
					if (!RawInput::IsKeyHeldReal(k)) { allHeld = false; break; }
				}
				if (allHeld) return true;
			}
			for (const auto& info : LuaKeyState::GetTrackedComboKeyInfo()) {
				if (!info.isPressed) continue;
				for (USHORT k : info.activeKeys) {
					if (k == vKey) return true;
				}
			}

			return false;
		}

		std::vector<USHORT> ComputeConflictedVKeys() {
			std::unordered_set<USHORT> conflicted;
			for (const auto& bind : bindings) {
				if (!bind.IsCombo()) continue;
				bool allHeld = true;
				for (USHORT k : bind.comboKeys) {
					if (!RawInput::IsKeyHeldReal(k)) { allHeld = false; break; }
				}
				if (!allHeld) continue;
				for (USHORT k : bind.comboKeys) conflicted.insert(k);
			}

			for (const auto& info : LuaKeyState::GetTrackedComboKeyInfo()) {
				if (!info.isPressed) continue;
				for (USHORT k : info.activeKeys) conflicted.insert(k);
			}

			for (const auto& info : LuaKeyState::GetTrackedKeyInfo()) {
				if (info.isConflicted) {
					conflicted.insert(info.vKey);
				}

				if (!info.hasDescription) continue;
				USHORT activeVKey = ResolveDisplayVKey(info);
				for (const auto& bind : bindings) {
					if (bind.IsCombo()) continue;
					if (bind.vKey != activeVKey) continue;
					if (IsSingleTriggerConflict(ManualTriggerMask(bind), info)) {
						conflicted.insert(activeVKey);
						break;
					}
				}
			}
			return std::vector<USHORT>(conflicted.begin(), conflicted.end());
		}

		std::vector<USHORT> ComputeDisabledModVKeys() {
			std::vector<USHORT> result;
			for (const auto& info : LuaKeyState::GetTrackedKeyInfo()) {
				if (!info.hasDescription) continue;
				if (!ModKeyBindings::IsDisabled(info.scriptName, info.functionName)) continue;
				result.push_back(ResolveDisplayVKey(info));
			}
			return result;
		}

		std::vector<std::vector<USHORT>> ComputeDisabledModCombos() {
			std::vector<std::vector<USHORT>> result;
			for (const auto& info : LuaKeyState::GetTrackedComboKeyInfo()) {
				if (!ModKeyBindings::IsDisabled(info.scriptName, info.functionName)) continue;
				result.push_back(info.activeKeys);
			}
			return result;
		}

		void Update() {
			LuaKeyState::SetSuppressedVKeys(ComputeConflictedVKeys());
			LuaKeyState::SetDisabledVKeys(ComputeDisabledModVKeys());
			LuaKeyState::SetDisabledCombos(ComputeDisabledModCombos());

			if (showCapturePrompt) {
				for (USHORT vKey : activeBindVKeys) {
					LuaKeyState::PhysicalOnButtonDown(vKey);
					LuaKeyState::PhysicalOnButtonUp(vKey);
				}
				return;
			}

			for (USHORT vKey : activeBindVKeys) {
				bool suppressedByCombo = IsVKeySuppressedByActiveCombo(vKey);

				if (LuaKeyState::PhysicalOnButtonDown(vKey)) {
					bool ctrlHeld = RawInput::IsKeyHeldReal(VK_CONTROL), shiftHeld = RawInput::IsKeyHeldReal(VK_SHIFT), altHeld = RawInput::IsKeyHeldReal(VK_MENU);
					DebuggerMenu::LogButtonPress(std::string(ctrlHeld ? "Ctrl+" : "") + (shiftHeld ? "Shift+" : "") + (altHeld ? "Alt+" : "") + NameForVKey(vKey) + " pressed");
					LogActivity(std::string(ctrlHeld ? "Ctrl+" : "") + (shiftHeld ? "Shift+" : "") + (altHeld ? "Alt+" : "") + NameForVKey(vKey) + " pressed");

					bool hasHoldOptionOnKey = false;
					for (const auto& bind : bindings) {
						if (bind.vKey == vKey && bind.holdSeconds > 0.0f && !bind.disabled) {
							if ((bind.needCtrl == ctrlHeld && bind.needShift == shiftHeld && bind.needAlt == altHeld) ||
								(!bind.needCtrl && !bind.needShift && !bind.needAlt)) {
								hasHoldOptionOnKey = true;
								break;
							}
						}
					}

					const KeyBind* toRun = FindMatchingBinding(vKey, ctrlHeld, shiftHeld, altHeld, false);
					bool deferForOnRelease = toRun && toRun->isInstant && toRun->instantTriggerType == 1;
					bool firedImmediately = false;
					if (toRun && !hasHoldOptionOnKey && !deferForOnRelease && !suppressedByCombo) {
						FireBinding(*toRun);
						firedImmediately = true;
					} else if (toRun && suppressedByCombo) {
						LogActivity(NameForVKey(vKey) + " press suppressed - part of an active Multi-Key Combo");
					}

					PendingPress pending;
					pending.ctrlOnPressed = ctrlHeld;
					pending.shiftOnPressed = shiftHeld;
					pending.altOnPressed = altHeld;
					pending.pressTime = std::chrono::steady_clock::now();
					pending.lastRepeatTime = pending.pressTime;
					pending.tapFired = firedImmediately;
					pendingPresses[vKey] = pending;
				}

				auto pendingIt = pendingPresses.find(vKey);
				if (pendingIt == pendingPresses.end()) {
					continue;
				}
				PendingPress& pending = pendingIt->second;

				const KeyBind* holdBind = FindMatchingBinding(vKey, pending.ctrlOnPressed, pending.shiftOnPressed, pending.altOnPressed, true);
				const KeyBind* tapBind = FindMatchingBinding(vKey, pending.ctrlOnPressed, pending.shiftOnPressed, pending.altOnPressed, false);

				if (!suppressedByCombo && !pending.holdFired && holdBind && LuaKeyState::PhysicalOnButtonHoldTime(vKey, holdBind->holdSeconds)) {
					DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " held past threshold " + std::to_string(holdBind->holdSeconds) + "s");
					LogActivity(NameForVKey(vKey) + " held past threshold " + std::to_string(holdBind->holdSeconds) + "s");
					FireBinding(*holdBind);
					pending.holdFired = true;
					pending.lastRepeatTime = std::chrono::steady_clock::now();
				}

				const KeyBind* repeatBind = nullptr;
				if (pending.holdFired && holdBind && holdBind->isInstant && holdBind->instantTriggerType == 2) {
					repeatBind = holdBind;
				} else if (!holdBind && tapBind && tapBind->isInstant && tapBind->instantTriggerType == 2) {
					repeatBind = tapBind;
				}
				if (repeatBind && !suppressedByCombo) {
					double sinceLastRepeat = std::chrono::duration<double>(std::chrono::steady_clock::now() - pending.lastRepeatTime).count();
					double effectiveInterval = kRepeatIntervalSeconds / pending.repeatSpeedMult;
					if (sinceLastRepeat >= effectiveInterval) {
						DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " repeat-fired");
						LogActivity(NameForVKey(vKey) + " repeat-fired");
						FireBinding(*repeatBind);
						pending.lastRepeatTime = std::chrono::steady_clock::now();
						pending.repeatSpeedMult = (std::clamp)(pending.repeatSpeedMult * (double)repeatBind->repeatAccelMult, kMinRepeatSpeedMult, kMaxRepeatSpeedMult);
					}
				}

				if (LuaKeyState::PhysicalOnButtonUp(vKey)) {
					if (!pending.holdFired && !pending.tapFired && !suppressedByCombo) {
						if (holdBind && holdBind->isInstant && holdBind->instantTriggerType != 2) {
							double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - pending.pressTime).count();
							if (heldSeconds < kNearMissHoldFraction * holdBind->holdSeconds) {
								DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " released early (Instant)");
								LogActivity(NameForVKey(vKey) + " released early (Instant)");
								FireBinding(*holdBind);
							} else {
								LogActivity(NameForVKey(vKey) + " released near hold threshold - Instant suppressed");
							}
						}
						else if (tapBind && tapBind->holdSeconds <= 0.0f && !(tapBind->isInstant && tapBind->instantTriggerType == 2)) {
							std::string reason = (tapBind->isInstant && tapBind->instantTriggerType == 1) ? "released (On Release)" : "tapped cleanly (Hold bypassed)";
							DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " " + reason);
							LogActivity(NameForVKey(vKey) + " " + reason);
							FireBinding(*tapBind);
						}
					}
					pendingPresses.erase(pendingIt);
				}
			}

			for (KeyBind& bind : bindings) {
				if (!bind.IsCombo()) continue;

				bool allHeld = true;
				for (USHORT k : bind.comboKeys) {
					if (!RawInput::IsKeyHeldReal(k)) { allHeld = false; break; }
				}
				bool hasHold = bind.holdSeconds > 0.0f;

				if (allHeld && !bind.comboActive) {
					bind.comboActive = true;
					bind.comboHoldFired = false;
					bind.comboTapFired = false;
					bind.comboPressTime = std::chrono::steady_clock::now();
					bind.comboLastRepeatTime = bind.comboPressTime;
					bind.runtimeRepeatSpeedMult = 1.0;

					std::string label = CombinedDisplayName(bind);
					DebuggerMenu::LogButtonPress(label + " pressed");
					LogActivity(label + " pressed");

					bool deferForOnRelease = !hasHold && bind.isInstant && bind.instantTriggerType == 1;
					if (!hasHold && !deferForOnRelease) {
						FireBinding(bind);
						bind.comboTapFired = true;
					}
				}

				if (bind.comboActive) {
					if (hasHold && !bind.comboHoldFired) {
						double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - bind.comboPressTime).count();
						if (heldSeconds >= bind.holdSeconds) {
							DebuggerMenu::LogButtonPress(CombinedDisplayName(bind) + " held past threshold " + std::to_string(bind.holdSeconds) + "s");
							LogActivity(CombinedDisplayName(bind) + " held past threshold " + std::to_string(bind.holdSeconds) + "s");
							FireBinding(bind);
							bind.comboHoldFired = true;
							bind.comboLastRepeatTime = std::chrono::steady_clock::now();
						}
					}

					bool repeatEligible = bind.isInstant && bind.instantTriggerType == 2 && (hasHold ? bind.comboHoldFired : true);
					if (repeatEligible) {
						double sinceLastRepeat = std::chrono::duration<double>(std::chrono::steady_clock::now() - bind.comboLastRepeatTime).count();
						double effectiveInterval = kRepeatIntervalSeconds / bind.runtimeRepeatSpeedMult;
						if (sinceLastRepeat >= effectiveInterval) {
							DebuggerMenu::LogButtonPress(CombinedDisplayName(bind) + " repeat-fired");
							LogActivity(CombinedDisplayName(bind) + " repeat-fired");
							FireBinding(bind);
							bind.comboLastRepeatTime = std::chrono::steady_clock::now();
							bind.runtimeRepeatSpeedMult = (std::clamp)(bind.runtimeRepeatSpeedMult * (double)bind.repeatAccelMult, kMinRepeatSpeedMult, kMaxRepeatSpeedMult);
						}
					}
				}

				if (!allHeld && bind.comboActive) {
					if (!bind.comboHoldFired && !bind.comboTapFired) {
						if (hasHold && bind.isInstant && bind.instantTriggerType != 2) {
							double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - bind.comboPressTime).count();
							if (heldSeconds < kNearMissHoldFraction * bind.holdSeconds) {
								DebuggerMenu::LogButtonPress(CombinedDisplayName(bind) + " released early (Instant)");
								LogActivity(CombinedDisplayName(bind) + " released early (Instant)");
								FireBinding(bind);
							} else {
								LogActivity(CombinedDisplayName(bind) + " released near hold threshold - Instant suppressed");
							}
						}
						else if (!hasHold) {
							DebuggerMenu::LogButtonPress(CombinedDisplayName(bind) + " released (On Release)");
							LogActivity(CombinedDisplayName(bind) + " released (On Release)");
							FireBinding(bind);
						}
					}
					bind.comboActive = false;
				}
			}
		}

		void EnsureDispatcherRegistered(USHORT vKey) {
			activeBindVKeys.insert(vKey);
		}

		void RemoveDispatcherIfUnused(USHORT vKey) {
			for (const auto& bind : bindings) if (bind.vKey == vKey) return;
			activeBindVKeys.erase(vKey);
			pendingPresses.erase(vKey);
			LuaKeyState::RetireIfUndescribed(vKey);
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
				if (!genericOn.empty() && genericOn.find(gameDirStr) == 0) {
					genericOn.erase(0, gameDirStr.length());
				}
				std::string genericOff = b.scriptPathOff.empty() ? "" : std::filesystem::path(b.scriptPathOff).generic_string();
				if (!genericOff.empty() && genericOff.find(gameDirStr) == 0) {
					genericOff.erase(0, gameDirStr.length());
				}

				if (b.IsCombo()) {
					std::string keysJoined;
					for (size_t i = 0; i < b.comboKeys.size(); i++) {
						if (i) keysJoined += ",";
						keysJoined += NameForVKey(b.comboKeys[i]);
					}
					outFile << "COMBO2|" << keysJoined << "|" << b.holdSeconds << "|";
					if (b.isToggle) {
						outFile << "1|" << genericOn << "|" << genericOff << "|" << b.functionOn << "|" << b.functionOff;
					} else {
						outFile << "0|" << genericOn << "|" << b.functionTap;
					}
					outFile << "|" << (b.isInstant ? "1" : "0") << "|" << b.instantTriggerType << "|" << b.repeatAccelMult << "|" << (b.disabled ? "1" : "0") << "\n";
					continue;
				}

				outFile << "BIND|" << b.keyName << "|" << (b.needCtrl ? "1|" : "0|") << (b.needShift ? "1|" : "0|") << (b.needAlt ? "1|" : "0|") << b.holdSeconds << "|";
				if (b.isToggle) {
					outFile << "1|" << genericOn << "|" << genericOff << "|" << b.functionOn << "|" << b.functionOff;
				} else {
					outFile << "0|" << genericOn << "|" << b.functionTap;
				}
				outFile << "|" << (b.isInstant ? "1" : "0") << "|" << b.instantTriggerType << "|" << b.repeatAccelMult << "|" << (b.disabled ? "1" : "0") << "\n";
			}
			for (const auto& entry : ModKeyBindings::GetAllOverrides()) {
				outFile << "MODKEY|" << entry.scriptName << "|" << entry.functionName << "|" << entry.keyName << "|" << (entry.disabled ? "1" : "0") << "\n";
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
				ModKeyBindings::LoadFromEntries({});
				return;
			}

			std::vector<ModKeyBindings::OverrideEntry> modKeyEntries;
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
				else if (parts[0] == "MODKEY" && parts.size() >= 4) {
					ModKeyBindings::OverrideEntry entry;
					entry.scriptName = trim(parts[1]);
					entry.functionName = trim(parts[2]);
					entry.keyName = trim(parts[3]);
					entry.disabled = parts.size() >= 5 && trim(parts[4]) == "1";
					if (!entry.scriptName.empty() && !entry.functionName.empty() && (!entry.keyName.empty() || entry.disabled)) {
						modKeyEntries.push_back(std::move(entry));
					}
				}
				else if (parts[0] == "COMBO2" && parts.size() >= 6) {
					std::vector<std::string> keyNames = split(trim(parts[1]), ",");
					std::vector<USHORT> comboKeys;
					bool allValid = keyNames.size() >= 2 && keyNames.size() <= 3;
					for (std::string& kn : keyNames) {
						int vk = VKeyForName(trim(kn));
						if (vk == -1) { allValid = false; break; }
						comboKeys.push_back((USHORT)vk);
					}
					if (!allValid) {
						spdlog::warn("KeyBindMenu::LoadBindings: skipping invalid COMBO2 line: {}", line);
						LogActivity("Skipped invalid COMBO2 line while loading bindings: " + line, false);
						continue;
					}

					float holdSeconds = 0.0f;
					try { holdSeconds = std::stof(trim(parts[2])); }
					catch (...) { holdSeconds = 0.0f; }

					bool isToggle = trim(parts[3]) == "1";
					std::string pathOn, pathOff, funcOn, funcOff, funcTap;
					size_t instantFieldStart;
					if (isToggle && parts.size() >= 8) {
						pathOn = trim(parts[4]);
						pathOff = trim(parts[5]);
						funcOn = trim(parts[6]);
						funcOff = trim(parts[7]);
						instantFieldStart = 8;
					} else if (!isToggle && parts.size() >= 6) {
						pathOn = trim(parts[4]);
						funcTap = trim(parts[5]);
						instantFieldStart = 6;
					} else {
						spdlog::warn("KeyBindMenu::LoadBindings: skipping incomplete COMBO2 line: {}", line);
						LogActivity("Skipped incomplete COMBO2 line while loading bindings: " + line, false);
						continue;
					}

					bool isInstant = false;
					int instantTriggerType = 0;
					float repeatAccelMult = 1.0f;
					if (parts.size() >= instantFieldStart + 2) {
						isInstant = trim(parts[instantFieldStart]) == "1";
						try { instantTriggerType = std::stoi(trim(parts[instantFieldStart + 1])); }
						catch (...) { instantTriggerType = 0; }
					}
					if (parts.size() >= instantFieldStart + 3) {
						try { repeatAccelMult = std::stof(trim(parts[instantFieldStart + 2])); }
						catch (...) { repeatAccelMult = 1.0f; }
						if (repeatAccelMult < kMinRepeatAccelMult) repeatAccelMult = kMinRepeatAccelMult;
					}
					bool disabled = (parts.size() >= instantFieldStart + 4) && trim(parts[instantFieldStart + 3]) == "1";

					KeyBind b{};
					b.comboKeys = comboKeys;
					b.isToggle = isToggle;
					b.scriptPathOn = ResolveScriptPath(pathOn);
					b.scriptPathOff = pathOff.empty() ? "" : ResolveScriptPath(pathOff);
					b.holdSeconds = holdSeconds;
					b.isInstant = isInstant;
					b.instantTriggerType = instantTriggerType;
					b.repeatAccelMult = repeatAccelMult;
					b.disabled = disabled;
					b.functionOn = funcOn;
					b.functionOff = funcOff;
					b.functionTap = funcTap;
					bindings.push_back(b);
				}
				else if (parts[0] == "COMBO" && parts.size() >= 3) {
					std::vector<std::string> keyNames = split(trim(parts[1]), ",");
					std::vector<USHORT> comboKeys;
					bool allValid = keyNames.size() >= 2 && keyNames.size() <= 3;
					for (std::string& kn : keyNames) {
						int vk = VKeyForName(trim(kn));
						if (vk == -1) { allValid = false; break; }
						comboKeys.push_back((USHORT)vk);
					}
					if (!allValid) {
						spdlog::warn("KeyBindMenu::LoadBindings: skipping invalid COMBO line: {}", line);
						LogActivity("Skipped invalid COMBO line while loading bindings: " + line, false);
						continue;
					}

					std::string pathOn = trim(parts[2]);
					std::string funcTap = parts.size() >= 4 ? trim(parts[3]) : "";

					KeyBind b{};
					b.comboKeys = comboKeys;
					b.scriptPathOn = ResolveScriptPath(pathOn);
					b.functionTap = funcTap;
					bindings.push_back(b);
				}
				else if (parts[0] == "BIND" && parts.size() >= 7) {
					if (parts.size() < 8) {
						spdlog::warn("KeyBindMenu::LoadBindings: skipping incomplete BIND line: {}", line);
						continue;
					}
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

					bool isInstant = false;
					int instantTriggerType = 0;
					float repeatAccelMult = 1.0f;
					size_t instantFieldStart = isToggle ? 11 : 9;
					if (parts.size() >= instantFieldStart + 2) {
						isInstant = trim(parts[instantFieldStart]) == "1";
						try { instantTriggerType = std::stoi(trim(parts[instantFieldStart + 1])); }
						catch (...) { instantTriggerType = 0; }
					}
					if (parts.size() >= instantFieldStart + 3) {
						try { repeatAccelMult = std::stof(trim(parts[instantFieldStart + 2])); }
						catch (...) { repeatAccelMult = 1.0f; }
						if (repeatAccelMult < kMinRepeatAccelMult) repeatAccelMult = kMinRepeatAccelMult;
					}
					bool disabled = (parts.size() >= instantFieldStart + 4) && trim(parts[instantFieldStart + 3]) == "1";

					KeyBind b{ (USHORT)vKey, trim(parts[2]) == "1", trim(parts[3]) == "1", trim(parts[4]) == "1", keyName, isToggle, resolvedOn, resolvedOff, false, holdSeconds };
					b.isInstant = isInstant;
					b.instantTriggerType = instantTriggerType;
					b.repeatAccelMult = repeatAccelMult;
					b.disabled = disabled;
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
			ModKeyBindings::LoadFromEntries(modKeyEntries);
			spdlog::debug("KeyBindMenu::LoadBindings: loaded {} binding(s) from {}", bindings.size(), GetBindsFileName());
			LogActivity("Loaded " + std::to_string(bindings.size()) + " binding(s) from " + GetBindsFileName());
			MarkDisplayCacheDirty();
		}

		void AddBinding(USHORT vKey, const std::string& keyName, bool needCtrl, bool needShift, bool needAlt, float holdSeconds, bool isToggle, const std::string& pathOn, const std::string& pathOff, const std::string& funcOn, const std::string& funcOff, const std::string& funcTap, bool isInstant, int instantTriggerType, float repeatAccelMult = 1.0f) {
			KeyBind b{ vKey, needCtrl, needShift, needAlt, keyName, isToggle, pathOn, pathOff, false, holdSeconds };
			b.isInstant = isInstant;
			b.instantTriggerType = instantTriggerType;
			b.repeatAccelMult = repeatAccelMult;
			b.functionOn = funcOn;
			b.functionOff = funcOff;
			b.functionTap = funcTap;

			bindings.push_back(b);
			EnsureDispatcherRegistered(vKey);
			SaveBindings();
			MarkDisplayCacheDirty();
			DebuggerMenu::LogBindEvent("Bound " + CombinedDisplayName(bindings.back()) + " -> mode toggle: " + (isToggle ? "YES" : "NO"));
			LogActivity("Bound " + CombinedDisplayName(bindings.back()) + " (toggle: " + (isToggle ? "YES" : "NO") + ")");
		}

		void AddComboBinding(const std::vector<USHORT>& comboKeys, bool isToggle, const std::string& pathOn, const std::string& pathOff, const std::string& funcOn, const std::string& funcOff, const std::string& funcTap, float holdSeconds, bool isInstant, int instantTriggerType, float repeatAccelMult = 1.0f) {
			KeyBind b{};
			b.comboKeys = comboKeys;
			b.isToggle = isToggle;
			b.scriptPathOn = pathOn;
			b.scriptPathOff = pathOff;
			b.holdSeconds = holdSeconds;
			b.isInstant = isInstant;
			b.instantTriggerType = instantTriggerType;
			b.repeatAccelMult = repeatAccelMult;
			b.functionOn = funcOn;
			b.functionOff = funcOff;
			b.functionTap = funcTap;

			bindings.push_back(b);
			SaveBindings();
			MarkDisplayCacheDirty();
			DebuggerMenu::LogBindEvent("Bound combo " + CombinedDisplayName(bindings.back()));
			LogActivity("Bound combo " + CombinedDisplayName(bindings.back()));
		}

		void RemoveBinding(int index) {
			if (index < 0 || index >= (int)bindings.size()) {
				LogActivity("Attempted to remove binding at invalid index " + std::to_string(index), false);
				return;
			}
			
			std::string removedDesc = CombinedDisplayName(bindings[index]) + " -> " + bindings[index].scriptPathOn;
			bool wasCombo = bindings[index].IsCombo();
			USHORT vKey = bindings[index].vKey;
			bindings.erase(bindings.begin() + index);
			if (!wasCombo) RemoveDispatcherIfUnused(vKey);
			SaveBindings();
			MarkDisplayCacheDirty();
			DebuggerMenu::LogBindEvent("Unbound " + removedDesc);
			LogActivity("Unbound " + removedDesc);
		}

		void RemoveAllBindings() {
			size_t count = bindings.size();
			for (const KeyBind& bind : bindings) {
				if (!bind.IsCombo()) LuaKeyState::RetireIfUndescribed(bind.vKey);
			}
			activeBindVKeys.clear();
			pendingPresses.clear();
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
			for (const auto& bind : bindings) {
				if (!bind.IsCombo()) EnsureDispatcherRegistered(bind.vKey);
			}
			RegisterMenuToggleKey(menuToggleVKey);
			LogActivity("Menu hotkey set to " + NameForVKey(menuToggleVKey));
			MarkInitializationComplete();
		}

		static USHORT capturedVKey = 0;
		static bool capturedCtrl = false;
		static bool capturedShift = false;
		static bool capturedAlt = false;
		static char capturedScriptPathOnBuffer[512] = "";
		static char capturedScriptPathOffBuffer[512] = "";
		static char capturedFuncOnBuffer[128] = "";
		static char capturedFuncOffBuffer[128] = "";
		static char capturedFuncTapBuffer[128] = "";
		static bool capturedToggleMode = false;
		static bool capturedLongPressMode = false;
		static float capturedRepeatAccelMult = 1.0f;
		static bool capturedHasFuncOn = false;
		static bool capturedHasFuncOff = false;
		static int capturedToggleType = 0; 
		static bool capturedInstantUserSet = false;
		static int editingBindingIndex = -1;
		static std::string modKeyCaptureScriptName;
		static std::string modKeyCaptureFunctionName;
		static bool captureIsCombo = false;
		static std::vector<USHORT> capturedComboKeys;
		static std::vector<USHORT> comboHoldKeys;
		static std::chrono::steady_clock::time_point comboHoldStartTime;
		static bool comboHoldActive = false;
		static USHORT singleHoldKey = 0;
		static std::chrono::steady_clock::time_point singleHoldStartTime;
		static bool singleHoldActive = false;
		constexpr double kComboHoldSeconds = 2.0;

		void ResetComboCaptureState() {
			capturedComboKeys.clear();
			comboHoldKeys.clear();
			comboHoldActive = false;
			singleHoldKey = 0;
			singleHoldActive = false;
		}

		std::vector<USHORT> ScanCurrentlyHeldKeys() {
			std::vector<USHORT> held;
			for (int i = 0; i < vkNameTableCount; i++) {
				USHORT vk = vkNameTable[i].vKey;
				if (RawInput::IsKeyHeldReal(vk)) held.push_back(vk);
			}
			return held;
		}

		void UpdateComboCapture() {
			if (!capturedComboKeys.empty()) return;

			std::vector<USHORT> currentlyHeld = ScanCurrentlyHeldKeys();

			if (!comboHoldActive) {
				if (currentlyHeld.size() >= 2 && currentlyHeld.size() <= 3) {
					comboHoldKeys = currentlyHeld;
					comboHoldStartTime = std::chrono::steady_clock::now();
					comboHoldActive = true;
				}
				return;
			}

			for (USHORT k : comboHoldKeys) {
				if (!RawInput::IsKeyHeldReal(k)) {
					comboHoldActive = false;
					comboHoldKeys.clear();
					LogActivity("Multi-key combo capture cancelled - a key was released before the hold completed");
					return;
				}
			}

			if (currentlyHeld.size() > comboHoldKeys.size() && currentlyHeld.size() <= 3) {
				comboHoldKeys = currentlyHeld;
				comboHoldStartTime = std::chrono::steady_clock::now();
			}

			double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - comboHoldStartTime).count();
			if (heldSeconds >= kComboHoldSeconds) {
				capturedComboKeys = comboHoldKeys;
				comboHoldActive = false;
				comboHoldKeys.clear();
				if (!isAssigningModKey && !capturedInstantUserSet) {
					capturedInstantMode = true;
					capturedInstantTriggerType = 0;
				}
				LogActivity("Multi-key combo captured: " + ComboKeysDisplayName(capturedComboKeys));
			}
		}

		void DrawCenteredPlaceholder(float areaWidth, float top, float areaHeight, ImVec4 color, const char* line1, const char* line2 = nullptr) {
			float lineHeight = ImGui::GetTextLineHeight();
			float lineSpacing = ImGui::GetStyle().ItemSpacing.y;
			int lineCount = line2 ? 2 : 1;
			float blockHeight = lineHeight * lineCount + lineSpacing * (lineCount - 1);

			ImGui::SetCursorPosY(top + (std::max)(0.0f, (areaHeight - blockHeight) * 0.5f));
			ImGui::SetCursorPosX((std::max)(0.0f, (areaWidth - ImGui::CalcTextSize(line1).x) * 0.5f));
			ImGui::TextColored(color, "%s", line1);
			if (line2) {
				ImGui::SetCursorPosX((std::max)(0.0f, (areaWidth - ImGui::CalcTextSize(line2).x) * 0.5f));
				ImGui::TextColored(color, "%s", line2);
			}
		}

		struct ModKeyReadOnlyInfo {
			bool found = false;
			bool anyToggle = false;
			bool anyLongPress = false;
			double longPressSeconds = 0.0;
			bool anyInstant = false;
			int instantType = 0;
			std::vector<std::string> breakdownLines;
		};

		ModKeyReadOnlyInfo ComputeModKeyReadOnlyInfo(const std::string& scriptName, const std::string& functionName) {
			ModKeyReadOnlyInfo result;
			std::vector<LuaKeyState::TrackedKeyInfo> trackedKeys = LuaKeyState::GetTrackedKeyInfo();

			USHORT targetVKey = 0;
			for (const auto& row : trackedKeys) {
				if (row.hasDescription && row.scriptName == scriptName && row.functionName == functionName) {
					targetVKey = row.vKey;
					result.found = true;
					break;
				}
			}
			if (!result.found) {
				return result;
			}

			int bestInstantPriority = -1;
			for (const auto& row : trackedKeys) {
				if (!row.hasDescription || row.vKey != targetVKey) {
					continue;
				}

				std::string triggerLabel;
				if (row.usesHoldTime) {
					result.anyLongPress = true;
					result.longPressSeconds = row.lastHoldSeconds;
					char buf[32];
					snprintf(buf, sizeof(buf), "%.1fs", row.lastHoldSeconds);
					triggerLabel = std::string("Long Press (") + buf + ")";
				} else if (row.usesRepeat) {
					if (bestInstantPriority < 2) bestInstantPriority = 2;
					triggerLabel = "Repeat";
				} else if (row.usesOnRelease) {
					if (bestInstantPriority < 1) bestInstantPriority = 1;
					triggerLabel = "On Release";
				} else if (row.usesOnPress) {
					if (bestInstantPriority < 0) bestInstantPriority = 0;
					triggerLabel = "On Press";
				} else {
					triggerLabel = "(not observed yet)";
				}

				if (row.hasToggleState) {
					result.anyToggle = true;
				}

				result.breakdownLines.push_back(triggerLabel + " -> " + row.scriptName + " [" + row.functionName + "]");
			}

			if (bestInstantPriority >= 0) {
				result.anyInstant = true;
				result.instantType = bestInstantPriority;
			}
			return result;
		}

		ModKeyReadOnlyInfo ComputeComboModKeyReadOnlyInfo(const std::string& scriptName, const std::string& functionName) {
			ModKeyReadOnlyInfo result;
			int bestInstantPriority = -1;
			for (const auto& row : LuaKeyState::GetTrackedComboKeyInfo()) {
				if (row.scriptName != scriptName || row.functionName != functionName) {
					continue;
				}
				result.found = true;
				if (row.hasToggleState) {
					result.anyToggle = true;
				}
				std::string triggerLabel = "Multi-key combo";
				if (row.usesHoldTime) {
					result.anyLongPress = true;
					triggerLabel += " / Long Press";
				}
				if (row.usesRepeat) {
					bestInstantPriority = (std::max)(bestInstantPriority, 2);
					triggerLabel += " / Repeat";
				}
				if (row.usesOnRelease) {
					bestInstantPriority = (std::max)(bestInstantPriority, 1);
					triggerLabel += " / On Release";
				}
				if (row.usesOnPress) {
					bestInstantPriority = (std::max)(bestInstantPriority, 0);
					triggerLabel += " / On Press";
				}
				result.breakdownLines.push_back(triggerLabel + " -> " + row.scriptName + " [" + row.functionName + "]");
				break;
			}
			if (bestInstantPriority >= 0) {
				result.anyInstant = true;
				result.instantType = bestInstantPriority;
			}
			return result;
		}

		void DrawKeyCapturePrompt() {
			ImGui::SetNextWindowSize(ImVec2(340, 330), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f - 170, ImGui::GetIO().DisplaySize.y * 0.5f - 165), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints(ImVec2(340, 330), ImVec2(FLT_MAX, FLT_MAX));
			if (requestCaptureFocus) {
				ImGui::SetNextWindowFocus();
				requestCaptureFocus = false;
			}
		
			const char* windowTitle = isAssigningModKey ? "Reassign mod key..." : "Assigning key bind...";
			if (!ImGui::Begin(windowTitle, nullptr, ImGuiWindowFlags_NoCollapse)) {
				ImGui::End();
				return;
			}

			ModKeyReadOnlyInfo modKeyInfo;
			if (isAssigningModKey) {
				modKeyInfo = captureIsCombo
					? ComputeComboModKeyReadOnlyInfo(modKeyCaptureScriptName, modKeyCaptureFunctionName)
					: ComputeModKeyReadOnlyInfo(modKeyCaptureScriptName, modKeyCaptureFunctionName);
				capturedToggleMode = modKeyInfo.anyToggle;
				capturedLongPressMode = modKeyInfo.anyLongPress;
				capturedHoldSeconds = (float)modKeyInfo.longPressSeconds;
				capturedInstantMode = modKeyInfo.anyInstant;
				capturedInstantTriggerType = modKeyInfo.instantType;

				ImGui::TextWrapped(UI_FMT_SCRIPT_FUNCTION_BRACKETS, modKeyCaptureScriptName.c_str(), modKeyCaptureFunctionName.c_str());
				ImGui::Separator();
			}

			if (!isAssigningMenuToggleKey && !isAssigningModKey) {
				bool wasCombo = captureIsCombo;
				ImGui::TextUnformatted(UI_LBL_BIND_TYPE); ImGui::SameLine();
				if (ImGui::RadioButton(UI_RADIO_SINGLE_KEY, !captureIsCombo)) captureIsCombo = false;
				ImGui::SameLine();
				if (ImGui::RadioButton(UI_RADIO_MULTI_KEY_COMBO, captureIsCombo)) captureIsCombo = true;
				if (captureIsCombo != wasCombo) {
					capturedVKey = 0;
					ResetComboCaptureState();
				}
				ImGui::Separator();
			}
		
			if (!captureIsCombo) {
			if (capturedVKey == 0) {
				capturedCtrl  = ImGui::GetIO().KeyCtrl;
				capturedShift = ImGui::GetIO().KeyShift;
				capturedAlt   = ImGui::GetIO().KeyAlt;

				if (!singleHoldActive) {
					USHORT pressedKey = 0;
					if (ImGui::IsMouseClicked(2)) pressedKey = VK_MBUTTON;
					else if (ImGui::IsMouseClicked(3)) pressedKey = VK_XBUTTON1;
					else if (ImGui::IsMouseClicked(4)) pressedKey = VK_XBUTTON2;
					else {
						for (int i = 1; i < 256; i++) {
							if (i == VK_CONTROL || i == VK_SHIFT || i == VK_MENU || i == VK_LWIN || i == VK_RWIN ||
								i == VK_LCONTROL || i == VK_RCONTROL || i == VK_LSHIFT || i == VK_RSHIFT || i == VK_LMENU || i == VK_RMENU)
								continue;
							if (i == VK_LBUTTON && ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) continue;
							if (ImGui::IsKeyPressed((ImGuiKey)i)) { pressedKey = (USHORT)i; break; }
						}
					}
					if (pressedKey != 0) {
						singleHoldKey = pressedKey;
						singleHoldStartTime = std::chrono::steady_clock::now();
						singleHoldActive = true;
					}
				}

				if (singleHoldActive) {
					if (!RawInput::IsKeyHeldReal(singleHoldKey)) {
						singleHoldActive = false;
						singleHoldKey = 0;
						LogActivity("Single-key capture cancelled - key was released before the hold completed");
					} else {
						double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - singleHoldStartTime).count();
						if (heldSeconds >= kComboHoldSeconds) {
							capturedVKey = singleHoldKey;
							capturedCtrl = ImGui::GetIO().KeyCtrl;
							capturedShift = ImGui::GetIO().KeyShift;
							capturedAlt = ImGui::GetIO().KeyAlt;
							singleHoldActive = false;
							singleHoldKey = 0;
							if (!isAssigningModKey && !capturedInstantUserSet) {
								capturedInstantMode = true;
								capturedInstantTriggerType = 0;
							}
							LogActivity("Single key captured: " + NameForVKey(capturedVKey));
						}
					}
				}
			}
		
			ImGui::BeginChild("KeyDisplayFrame", ImVec2(105, 95), true, ImGuiWindowFlags_NoScrollbar);
			auto [availWidth, availHeight] = ImGui::GetContentRegionAvail();
			const float contentStartX = ImGui::GetCursorPosX();
			ImGui::SetCursorPosX(contentStartX + (std::max)(0.0f, (availWidth - ImGui::CalcTextSize(UI_LBL_KEY).x) * 0.5f)); ImGui::TextUnformatted(UI_LBL_KEY); ImGui::Separator();
			float lowerBoxTopY = ImGui::GetCursorPosY(), lowerBoxRemainingHeight = availHeight - lowerBoxTopY;
		
			if (capturedVKey == 0) {
				const bool showingHold = singleHoldActive && singleHoldKey != 0;
				float lineHeight = ImGui::GetTextLineHeight();
				float lineSpacing = ImGui::GetStyle().ItemSpacing.y;
				float progressHeight = showingHold ? 8.0f : 0.0f;
				float progressGap = showingHold ? lineSpacing : 0.0f;
				float blockHeight = lineHeight * 2.0f + lineSpacing + progressGap + progressHeight;
				float startVerticalY = lowerBoxTopY + ((lowerBoxRemainingHeight - blockHeight) * 0.5f);
				ImGui::SetCursorPosY(startVerticalY);

				std::string holdName = showingHold ? NameForVKey(singleHoldKey) : "";
				const char* line1 = showingHold ? holdName.c_str() : UI_LBL_PRESS;
				const char* line2 = showingHold ? UI_LBL_HOLD : UI_LBL_KEY_ELLIPSIS;
				float w1 = ImGui::CalcTextSize(line1).x;
				float w2 = ImGui::CalcTextSize(line2).x;
				ImGui::SetCursorPosX(contentStartX + (std::max)(0.0f, (availWidth - w1) * 0.5f));
				ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", line1);
				ImGui::SetCursorPosX(contentStartX + (std::max)(0.0f, (availWidth - w2) * 0.5f));
				ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", line2);
				if (showingHold) {
					double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - singleHoldStartTime).count();
					float progress = (float)(std::min)(heldSeconds / kComboHoldSeconds, 1.0);
					ImGui::Spacing();
					ImGui::ProgressBar(progress, ImVec2(availWidth - 4.0f, 8.0f), "");
				}
			} else {
				std::string keyName = NameForVKey(capturedVKey);
				ImGui::SetCursorPosY(lowerBoxTopY + ((lowerBoxRemainingHeight - ImGui::GetTextLineHeight()) * 0.5f));
				ImGui::SetCursorPosX(contentStartX + (std::max)(0.0f, (availWidth - ImGui::CalcTextSize(keyName.c_str()).x) * 0.5f));
				ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%s", keyName.c_str());
			}
			ImGui::EndChild(); ImGui::SameLine();
		
			ImGui::BeginGroup();
			if (!isAssigningMenuToggleKey && !isAssigningModKey) {
				ImGui::Checkbox(UI_CHK_CTRL, &capturedCtrl); ImGui::Checkbox(UI_CHK_SHIFT, &capturedShift); ImGui::Checkbox(UI_CHK_ALT, &capturedAlt); 
			}
			
			if (ImGui::Button(UI_BTN_RESET, ImVec2(55, 22))) { 
			    capturedVKey = 0; 
			    capturedCtrl = capturedShift = capturedAlt = capturedToggleMode = capturedLongPressMode = capturedHasFuncOn = capturedHasFuncOff = false; 
			    capturedHoldSeconds = 0.0f;
			    capturedToggleType = 0;
		    capturedInstantMode = false;
		    capturedInstantTriggerType = 0;
		    capturedRepeatAccelMult = 1.0f;
		    capturedInstantUserSet = false;
			    LogActivity("Keybind has been reset");
			}
			ImGui::EndGroup(); ImGui::SameLine(205);
			} else {
				UpdateComboCapture();

				ImGui::BeginChild("ComboKeyDisplayFrame", ImVec2(105, 95), true, ImGuiWindowFlags_NoScrollbar);
				auto [availWidth, availHeight] = ImGui::GetContentRegionAvail();
				const float contentStartX = ImGui::GetCursorPosX();
				ImGui::SetCursorPosX(contentStartX + (std::max)(0.0f, (availWidth - ImGui::CalcTextSize(UI_LBL_KEYS).x) * 0.5f)); ImGui::TextUnformatted(UI_LBL_KEYS); ImGui::Separator();
				float lowerBoxTopY = ImGui::GetCursorPosY(), lowerBoxRemainingHeight = availHeight - lowerBoxTopY;

				if (capturedComboKeys.empty() && !comboHoldActive) {
					DrawCenteredPlaceholder(availWidth, lowerBoxTopY, lowerBoxRemainingHeight, ImVec4(0.4f, 0.8f, 1.0f, 1.0f), UI_LBL_HOLD_INDENT, UI_LBL_2_3_KEYS);
				} else {
					bool isFinal = !capturedComboKeys.empty();
					const std::vector<USHORT>& shownKeys = isFinal ? capturedComboKeys : comboHoldKeys;
					std::string names = ComboKeysDisplayName(shownKeys);

					ImGui::SetWindowFontScale(0.8f);

					std::vector<std::string> displayLines;
					std::string currentLine;
					std::string token;
					for (size_t i = 0; i <= names.size(); ++i) {
						bool atEnd = (i == names.size());
						char c = atEnd ? '\0' : names[i];
						if (c == ' ' || atEnd) {
							if (!token.empty()) {
								std::string candidate = currentLine.empty() ? token : currentLine + " " + token;
								if (!currentLine.empty() && ImGui::CalcTextSize(candidate.c_str()).x > availWidth) {
									displayLines.push_back(currentLine);
									currentLine = token;
								} else {
									currentLine = candidate;
								}
								token.clear();
							}
						} else {
							token += c;
						}
					}
					if (!currentLine.empty()) displayLines.push_back(currentLine);
					if (displayLines.empty()) displayLines.push_back(names);

					float lineHeight = ImGui::GetTextLineHeight();
					float lineSpacing = ImGui::GetStyle().ItemSpacing.y;
					float textHeight = lineHeight * displayLines.size() + lineSpacing * (displayLines.size() - 1);
					float reserveForBar = isFinal ? 0.0f : 12.0f;
					ImGui::SetCursorPosY(lowerBoxTopY + (std::max)(0.0f, (lowerBoxRemainingHeight - textHeight - reserveForBar) * 0.5f));
					ImGui::PushStyleColor(ImGuiCol_Text, isFinal ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) : ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
					for (const std::string& line : displayLines) {
						float lineWidth = ImGui::CalcTextSize(line.c_str()).x;
						ImGui::SetCursorPosX(contentStartX + (std::max)(0.0f, (availWidth - lineWidth) * 0.5f));
						ImGui::TextUnformatted(line.c_str());
					}
					ImGui::PopStyleColor();
					ImGui::SetWindowFontScale(1.0f);

					if (!isFinal) {
						double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - comboHoldStartTime).count();
						float progress = (float)(std::min)(heldSeconds / kComboHoldSeconds, 1.0);
						ImGui::ProgressBar(progress, ImVec2(availWidth - 4.0f, 8.0f), "");
					}
				}
				ImGui::EndChild();
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(UI_TIP_COMBO_HOLD, kComboHoldSeconds);
				}
				ImGui::SameLine();

				ImGui::BeginGroup();
				if (ImGui::Button(UI_BTN_RESET, ImVec2(55, 22))) {
					ResetComboCaptureState();
					LogActivity("Multi-key combo has been reset");
				}
				ImGui::EndGroup(); ImGui::SameLine(205);
			}
		
			ImGui::BeginGroup();
			if (!isAssigningMenuToggleKey) {
				if (isAssigningModKey) ImGui::BeginDisabled();
				if (capturedInstantMode) ImGui::BeginDisabled();
				ImGui::Checkbox(UI_CHK_TOGGLE, &capturedToggleMode);
				if (capturedInstantMode && ImGui::IsItemHovered()) {
					ImGui::SetTooltip(UI_TIP_UNCHECK_INSTANT_FIRST);
				}
				ImGui::Checkbox(UI_CHK_LONG_PRESS, &capturedLongPressMode);
				if (capturedInstantMode && ImGui::IsItemHovered()) {
					ImGui::SetTooltip(UI_TIP_UNCHECK_INSTANT_FIRST);
				}
				if (capturedInstantMode) ImGui::EndDisabled();
				
				if (capturedLongPressMode) {
				    ImGui::SetNextItemWidth(75);
				    ImGui::InputFloat("##capturedHoldInput", &capturedHoldSeconds, 0.0f, 0.0f, "%.1fs");
				    if (capturedHoldSeconds < 0.0f) capturedHoldSeconds = 0.0f;
				    if (ImGui::Button(UI_BTN_MINUS, ImVec2(35, 20))) { if ((capturedHoldSeconds -= 0.5f) < 0.0f) capturedHoldSeconds = 0.0f; } ImGui::SameLine(40);
				    if (ImGui::Button(UI_BTN_PLUS, ImVec2(35, 20))) capturedHoldSeconds += 0.5f;
				}

				bool toggleOrLongPress = capturedToggleMode || capturedLongPressMode;
				if (toggleOrLongPress) ImGui::BeginDisabled();
				if (ImGui::Checkbox(UI_CHK_INSTANT, &capturedInstantMode)) {
					capturedInstantUserSet = true;
				}
				if (toggleOrLongPress) ImGui::EndDisabled();
				if (toggleOrLongPress && ImGui::IsItemHovered()) {
					ImGui::SetTooltip(UI_TIP_UNCHECK_TOGGLE_FIRST);
				}

				if (capturedInstantMode) {
					static const char* instantTriggerLabels[] = { UI_OPT_ON_PRESS, UI_OPT_ON_RELEASE, UI_OPT_REPEAT };
					ImGui::SetNextItemWidth(120);
					if (ImGui::Combo("##capturedInstantTrigger", &capturedInstantTriggerType, instantTriggerLabels, IM_ARRAYSIZE(instantTriggerLabels))) {
						capturedInstantUserSet = true;
					}

					if (capturedInstantTriggerType == 2) {
						ImGui::SetNextItemWidth(55);
						ImGui::InputFloat("##capturedRepeatAccelInput", &capturedRepeatAccelMult, 0.0f, 0.0f, "%.2fx");
						if (capturedRepeatAccelMult < kMinRepeatAccelMult) capturedRepeatAccelMult = kMinRepeatAccelMult;
						if (ImGui::IsItemHovered()) {
							ImGui::SetTooltip("%s", UI_TIP_REPEAT_ACCEL);
						}
					}
				}

				if (isAssigningModKey) {
					ImGui::EndDisabled();
					ImGui::TextDisabled(UI_TXT_DETECTED_READ_ONLY);
				}
			}
			
			bool comboAvailable = false;
			if (captureIsCombo) {
				if (!capturedComboKeys.empty()) {
					if (isAssigningModKey) {
						comboAvailable = IsModComboAssignmentAvailable(
							capturedComboKeys, modKeyCaptureScriptName, modKeyCaptureFunctionName);
					} else {
						comboAvailable = IsMultiKeyComboAvailable(
							capturedComboKeys,
						editingBindingIndex,
						"", "");
					}
				}
			}
			else if (capturedVKey != 0) {
				if (isAssigningModKey) {
					comboAvailable = IsModSingleAssignmentAvailable(
						capturedVKey, modKeyCaptureScriptName, modKeyCaptureFunctionName);
				}
				else if (isAssigningMenuToggleKey) {
					comboAvailable = !(capturedVKey == VK_F2 || capturedVKey == VK_F3 || capturedVKey == VK_ESCAPE);
				} 
				else {
					static int lastCheckedIndex = -1;
					if (editingBindingIndex != -1 && lastCheckedIndex != editingBindingIndex) {
						capturedHasFuncOn = !bindings[editingBindingIndex].functionOn.empty() || !bindings[editingBindingIndex].functionTap.empty();
						capturedHasFuncOff = !bindings[editingBindingIndex].functionOff.empty();
						lastCheckedIndex = editingBindingIndex;
					}

					comboAvailable = IsComboAvailable(
						capturedVKey, capturedCtrl, capturedShift, capturedAlt,
						capturedHoldSeconds, ManualCaptureTriggerMask(), editingBindingIndex);
				}
			}
			ImGui::Spacing();
			
			const char* comboStatusLabel = comboAvailable ? UI_LBL_READY : UI_LBL_UNFIT;
			ImVec2 comboStatusSize = ImGui::CalcTextSize(comboStatusLabel);
			ImGui::BeginChild("ComboStatusBox", ImVec2(comboStatusSize.x + ImGui::GetStyle().WindowPadding.x * 2.0f, comboStatusSize.y + ImGui::GetStyle().WindowPadding.y * 2.0f), true, ImGuiWindowFlags_NoScrollbar);
			ImGui::TextColored(comboAvailable ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", comboStatusLabel);
			ImGui::EndChild();
			
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(comboAvailable ? UI_TIP_COMBO_VALID : UI_TIP_COMBO_CONFLICT);
			}
			ImGui::EndGroup(); ImGui::Separator();
		
			bool isUpperPathValid = false, isLowerPathValid = false;
			int scriptStatus = 0, lowerStatus = 0;

			static char lastStatCheckedOnBuffer[512] = "";
			static int cachedOnStatus = 0;
			static char lastStatCheckedOffBuffer[512] = "";
			static int cachedOffStatus = 0;
		
			if (!isAssigningMenuToggleKey && !isAssigningModKey) {
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
		
			if (!isAssigningMenuToggleKey && !isAssigningModKey) {
				float paddingX = ImGui::GetStyle().WindowPadding.x;
				float totalWidth = ImGui::GetWindowWidth();
				float rightEdgeX = totalWidth - paddingX;
				float elementWidth = 145.0f;
				float targetCursorPosX = rightEdgeX - elementWidth - 8.0f;

				if (capturedToggleMode) {
					ImGui::AlignTextToFramePadding();
					ImGui::Text(UI_LBL_SCRIPT_MODE); ImGui::SameLine();
					ImGui::RadioButton(UI_RADIO_SINGLE, &capturedToggleType, 0); ImGui::SameLine();
					ImGui::RadioButton(UI_RADIO_DUAL, &capturedToggleType, 1);
					ImGui::Separator(); ImGui::Spacing();

					if (capturedToggleType == 0) {
						ImGui::Checkbox("##hasFuncOn", &capturedHasFuncOn); ImGui::SameLine();
						if (ImGui::IsItemHovered()) ImGui::SetTooltip(UI_TIP_TARGET_GLOBAL_FUNCS);
						ImGui::SameLine(); ImGui::Text(UI_LBL_SCRIPT_PATH);
						
						ImGui::SetNextItemWidth(-1);
						ImGui::InputText("##captureScriptInputOn", capturedScriptPathOnBuffer, IM_ARRAYSIZE(capturedScriptPathOnBuffer));
						
						if (capturedHasFuncOn) {
							ImGui::Text(UI_LBL_ENABLE_FUNCTION); ImGui::SameLine(targetCursorPosX);
							ImGui::SetNextItemWidth(elementWidth);
							ImGui::InputText("##captureFuncOn", capturedFuncOnBuffer, IM_ARRAYSIZE(capturedFuncOnBuffer));
		
							ImGui::Text(UI_LBL_DISABLE_FUNCTION); ImGui::SameLine(targetCursorPosX);
							ImGui::SetNextItemWidth(elementWidth);
							ImGui::InputText("##captureFuncOff", capturedFuncOffBuffer, IM_ARRAYSIZE(capturedFuncOffBuffer));
						}
					} 
					else {
						ImGui::Checkbox("##hasFuncOn", &capturedHasFuncOn); ImGui::SameLine();
						if (ImGui::IsItemHovered()) ImGui::SetTooltip(UI_TIP_TARGET_ENABLE_SCRIPT);
						ImGui::SameLine(); ImGui::Text(UI_LBL_ENABLE_SCRIPT_PATH);

						ImGui::SetNextItemWidth(-1);
						ImGui::InputText("##captureScriptInputOn", capturedScriptPathOnBuffer, IM_ARRAYSIZE(capturedScriptPathOnBuffer));
						
						if (capturedHasFuncOn) {
							ImGui::SetCursorPosX(targetCursorPosX);
							ImGui::SetNextItemWidth(elementWidth);
							ImGui::InputText("##captureFuncOn", capturedFuncOnBuffer, IM_ARRAYSIZE(capturedFuncOnBuffer));
						}

						ImGui::Spacing();
						ImGui::Checkbox("##hasFuncOff", &capturedHasFuncOff); ImGui::SameLine();
						if (ImGui::IsItemHovered()) ImGui::SetTooltip(UI_TIP_TARGET_DISABLE_SCRIPT);
						
						ImGui::SameLine(); ImGui::Text(UI_LBL_DISABLE_SCRIPT_PATH);
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
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(UI_TIP_TARGET_SCRIPT_FILE);
					
					ImGui::SameLine(); ImGui::Text(UI_LBL_SCRIPT_PATH);
					ImGui::SetNextItemWidth(-1);
					ImGui::InputText("##captureScriptInputOn", capturedScriptPathOnBuffer, IM_ARRAYSIZE(capturedScriptPathOnBuffer));
					
					if (capturedHasFuncOn) {
						ImGui::SetCursorPosX(targetCursorPosX);
						ImGui::SetNextItemWidth(elementWidth);
						ImGui::InputText("##captureFuncTap", capturedFuncTapBuffer, IM_ARRAYSIZE(capturedFuncTapBuffer));
					}
				}
			}
			else if (isAssigningModKey) {
				ImGui::TextDisabled(UI_TXT_DETECTED_FROM_SCRIPT);
				if (modKeyInfo.breakdownLines.empty()) {
					ImGui::TextWrapped(UI_TXT_NO_KEYS_QUERIED);
				} else {
					for (const std::string& line : modKeyInfo.breakdownLines) {
						ImGui::TextWrapped(UI_FMT_S, line.c_str());
					}
				}
			}
		
			bool pathsValid = (capturedToggleMode && capturedToggleType != 0) ? (isUpperPathValid && isLowerPathValid) : isUpperPathValid;
			bool functionsValid = true;
			
			if (!isAssigningMenuToggleKey && !isAssigningModKey) {
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
			
			bool captureReady = captureIsCombo ? !capturedComboKeys.empty() : (capturedVKey != 0);
			bool assignmentIsValid = comboAvailable;
			bool canFinalize = captureReady && (assignmentIsValid && (isAssigningMenuToggleKey || isAssigningModKey || (pathsValid && functionsValid)));
			float paddingY = ImGui::GetStyle().WindowPadding.y;
			float buttonHeight = 30.0f;
			float bottomAnchorY = ImGui::GetWindowHeight() - paddingY - buttonHeight;
			
			ImGui::SetCursorPosY(bottomAnchorY);

			if (!canFinalize) ImGui::BeginDisabled();
			if (ImGui::Button(UI_BTN_FINALIZE, ImVec2(145, buttonHeight))) {
				if (isAssigningModKey) {
					if (captureIsCombo) {
						std::string comboKeyName = ComboKeysDisplayName(capturedComboKeys);
						ModKeyBindings::SetOverride(modKeyCaptureScriptName, modKeyCaptureFunctionName, comboKeyName);
						DebuggerMenu::LogBindEvent("Mod combo reassigned: " + modKeyCaptureScriptName + " [" + modKeyCaptureFunctionName + "] -> " + comboKeyName);
						LogActivity("Mod combo reassigned: " + modKeyCaptureScriptName + " [" + modKeyCaptureFunctionName + "] -> " + comboKeyName);
						MarkDisplayCacheDirty();

						ResetComboCaptureState();
						captureIsCombo = false;
					}
					else {
						USHORT oldVKey = 0;
						for (const auto& row : LuaKeyState::GetTrackedKeyInfo()) {
							if (row.scriptName == modKeyCaptureScriptName && row.functionName == modKeyCaptureFunctionName) {
								oldVKey = row.vKey;
								break;
							}
						}

						ModKeyBindings::SetOverride(modKeyCaptureScriptName, modKeyCaptureFunctionName, NameForVKey(capturedVKey));
						DebuggerMenu::LogBindEvent("Mod key reassigned: " + modKeyCaptureScriptName + " [" + modKeyCaptureFunctionName + "] -> " + NameForVKey(capturedVKey));
						LogActivity("Mod key reassigned: " + modKeyCaptureScriptName + " [" + modKeyCaptureFunctionName + "] -> " + NameForVKey(capturedVKey));
						if (oldVKey != 0 && oldVKey != capturedVKey) {
							LuaKeyState::ReassignBinding(oldVKey, capturedVKey, modKeyCaptureScriptName, modKeyCaptureFunctionName);
						}
						MarkDisplayCacheDirty();
					}

					capturedVKey = 0; capturedHoldSeconds = 0.0f;
					capturedToggleMode = capturedLongPressMode = false;
					capturedInstantMode = false; capturedInstantTriggerType = 0; capturedRepeatAccelMult = 1.0f;
					capturedInstantUserSet = false;
					showCapturePrompt = isAssigningModKey = false;
				}
				else if (isAssigningMenuToggleKey) {
					if (menuToggleHandle != 0) RawInput::UnRegisterAction(menuToggleVKey, menuToggleHandle);
					menuToggleVKey = capturedVKey; 
					menuToggleHandle = RawInput::RegisterAction(menuToggleVKey, OnMenuToggleKeyPressed);
					SaveBindings(); 
					DebuggerMenu::LogBindEvent("Menu Hotkey reassigned to: " + NameForVKey(capturedVKey));
					LogActivity("Menu Hotkey reassigned to " + NameForVKey(capturedVKey));
					capturedToggleMode = capturedLongPressMode = false;
					capturedInstantMode = false; capturedInstantTriggerType = 0; capturedRepeatAccelMult = 1.0f;
					capturedInstantUserSet = false;
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

					if (captureIsCombo) {
						if (editingBindingIndex != -1) {
							KeyBind editedBind{};
							editedBind.comboKeys = capturedComboKeys;
							editedBind.isToggle = capturedToggleMode;
							editedBind.scriptPathOn = finalPathOn;
							editedBind.scriptPathOff = finalPathOff;
							editedBind.holdSeconds = finalHoldSeconds;
							editedBind.isInstant = capturedInstantMode;
							editedBind.instantTriggerType = capturedInstantTriggerType;
							editedBind.repeatAccelMult = capturedRepeatAccelMult;
							editedBind.functionOn = finalFuncOn;
							editedBind.functionOff = finalFuncOff;
							editedBind.functionTap = finalFuncTap;

							bindings[editingBindingIndex] = editedBind;
							SaveBindings();
							MarkDisplayCacheDirty();
							LogActivity("Edited combo binding -> " + CombinedDisplayName(editedBind));
						} else {
							AddComboBinding(capturedComboKeys, capturedToggleMode, finalPathOn, finalPathOff, finalFuncOn, finalFuncOff, finalFuncTap, finalHoldSeconds, capturedInstantMode, capturedInstantTriggerType, capturedRepeatAccelMult);
						}
					}
					else if (editingBindingIndex != -1) {
						USHORT oldVKey = bindings[editingBindingIndex].vKey;
						KeyBind editedBind{ capturedVKey, capturedCtrl, capturedShift, capturedAlt, NameForVKey(capturedVKey), capturedToggleMode, finalPathOn, finalPathOff, false, finalHoldSeconds };
						editedBind.isInstant = capturedInstantMode;
						editedBind.instantTriggerType = capturedInstantTriggerType;
						editedBind.repeatAccelMult = capturedRepeatAccelMult;
						editedBind.functionOn = finalFuncOn;
						editedBind.functionOff = finalFuncOff;
						editedBind.functionTap = finalFuncTap;
						
						bindings[editingBindingIndex] = editedBind;
						RemoveDispatcherIfUnused(oldVKey); EnsureDispatcherRegistered(capturedVKey); SaveBindings();
						MarkDisplayCacheDirty();
						LogActivity("Edited binding -> " + CombinedDisplayName(editedBind));
					} else {
						AddBinding(capturedVKey, NameForVKey(capturedVKey), capturedCtrl, capturedShift, capturedAlt, finalHoldSeconds, capturedToggleMode, finalPathOn, finalPathOff, finalFuncOn, finalFuncOff, finalFuncTap, capturedInstantMode, capturedInstantTriggerType, capturedRepeatAccelMult);
					}
					capturedScriptPathOnBuffer[0] = capturedScriptPathOffBuffer[0] = '\0';
					capturedFuncOnBuffer[0] = capturedFuncOffBuffer[0] = capturedFuncTapBuffer[0] = '\0'; 
					capturedToggleMode = capturedLongPressMode = capturedHasFuncOn = capturedHasFuncOff = false;
					capturedInstantMode = false; capturedInstantTriggerType = 0; capturedRepeatAccelMult = 1.0f;
					capturedInstantUserSet = false;
					ResetComboCaptureState();
					captureIsCombo = false;
				}
				capturedVKey = 0; capturedHoldSeconds = 0.0f; 
				showCapturePrompt = isAssigningMenuToggleKey = isAssigningModKey = false; editingBindingIndex = -1;
			}
			if (!canFinalize) ImGui::EndDisabled(); ImGui::SameLine();
			
			float paddingX = ImGui::GetStyle().WindowPadding.x;
			float targetCancelX = ImGui::GetWindowWidth() - paddingX - 145.0f - 8.0f;
			ImGui::SameLine(targetCancelX);
			
			if (ImGui::Button(UI_BTN_CANCEL, ImVec2(145, buttonHeight))) {
				capturedVKey = 0; capturedHoldSeconds = 0.0f; 
				capturedScriptPathOnBuffer[0] = capturedScriptPathOffBuffer[0] = '\0';
				capturedFuncOnBuffer[0] = capturedFuncOffBuffer[0] = capturedFuncTapBuffer[0] = '\0'; 
				capturedToggleMode = capturedLongPressMode = capturedHasFuncOn = capturedHasFuncOff = false;
				capturedInstantMode = false; capturedInstantTriggerType = 0; capturedRepeatAccelMult = 1.0f;
				capturedInstantUserSet = false;
				ResetComboCaptureState();
				captureIsCombo = false;
				showCapturePrompt = isAssigningMenuToggleKey = isAssigningModKey = false; editingBindingIndex = -1;
				LogActivity("Key Assignment Prompt cancelled");
			}

			if (isAssigningModKey) {
				ImGui::TextDisabled(UI_TXT_TAKES_EFFECT_NEXT_FRAME);
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

			static float minWindowHeightFloor = 220.0f;
			ImGui::SetNextWindowSize(ImVec2(finalMinWidthFloor, minWindowHeightFloor), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints(ImVec2(finalMinWidthFloor, minWindowHeightFloor), ImVec2(FLT_MAX, FLT_MAX));

			if (!ImGui::Begin(UI_WINDOW_TITLE, p_open)) { ImGui::End(); return; }
			if (ImGui::Button(UI_BTN_DEBUGGER)) {
				DebuggerMenu::menuOpen = !DebuggerMenu::menuOpen;
				LogActivity(DebuggerMenu::menuOpen ? UI_LOG_DEBUGGER_OPENED : UI_LOG_DEBUGGER_CLOSED);
			}
			ImGui::SameLine();
			
			std::string buttonLabel = UI_LBL_MENU_HOTKEY_PREFIX + NameForVKey(menuToggleVKey) + UI_LBL_MENU_HOTKEY_SUFFIX;
			if (ImGui::Button(buttonLabel.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { 
				isAssigningMenuToggleKey = showCapturePrompt = true; 
				requestCaptureFocus = true;
				LogActivity("Menu Key Reassignment Prompt opened");
			}
			ImGui::Separator();

			ImGui::Text(UI_HDR_KEY_BINDINGS);
			const float kListMinHeight = 150.0f;
			float paddingX = ImGui::GetStyle().WindowPadding.x;
			float paddingY = ImGui::GetStyle().WindowPadding.y;
			float footerHeight = 45.0f;
			float cursorYBeforeList = ImGui::GetCursorPosY();
			float listRemainingHeight = ImGui::GetWindowHeight() - cursorYBeforeList - ImGui::GetTextLineHeightWithSpacing() - footerHeight - paddingY;
			if (listRemainingHeight < kListMinHeight) listRemainingHeight = kListMinHeight;
			minWindowHeightFloor = cursorYBeforeList + ImGui::GetTextLineHeightWithSpacing() + footerHeight + paddingY + kListMinHeight;

			{
				LuaKeyState::SweepStaleDescriptions();
				LuaKeyState::SweepStaleComboDescriptions();
				ModInfoRegistry::SweepStale();
				std::vector<LuaKeyState::TrackedKeyInfo> trackedKeys = LuaKeyState::GetTrackedKeyInfo();
				std::vector<LuaKeyState::TrackedComboKeyInfo> trackedCombos = LuaKeyState::GetTrackedComboKeyInfo();
				std::unordered_map<std::string, ModInfoRegistry::ModInfo> modInfoByScript;
				for (const ModInfoRegistry::ModInfo& modInfo : ModInfoRegistry::GetTrackedModInfo()) {
					modInfoByScript[modInfo.scriptName] = modInfo;
				}
				auto displayScriptName = [&modInfoByScript](const std::string& scriptName) -> const std::string& {
					auto it = modInfoByScript.find(scriptName);
					if (it != modInfoByScript.end() && !it->second.modName.empty()) {
						return it->second.modName;
					}
					return scriptName;
				};

				std::unordered_set<USHORT> manualBoundVKeys;
				for (const KeyBind& bind : bindings) {
					manualBoundVKeys.insert(bind.vKey);
				}
				std::vector<USHORT> conflictedList = ComputeConflictedVKeys();
				std::unordered_set<USHORT> conflictedVKeys(conflictedList.begin(), conflictedList.end());

				struct UnifiedRow {
					bool isManual = false;
					bool isComboScript = false;
					int bindIndex = -1;
					LuaKeyState::TrackedKeyInfo info;
					LuaKeyState::TrackedComboKeyInfo comboInfo;
					bool conflicted = false;
					USHORT displayVKey = 0;
				};
				std::vector<UnifiedRow> rows;
				rows.reserve(trackedKeys.size() + trackedCombos.size() + bindings.size());

				for (LuaKeyState::TrackedKeyInfo& info : trackedKeys) {
					if (!info.hasDescription && manualBoundVKeys.count(info.vKey) > 0) {
						continue;
					}
					USHORT displayVKey = ResolveDisplayVKey(info);
					bool conflicted = conflictedVKeys.count(displayVKey) > 0;
					UnifiedRow row;
					row.isManual = false;
					row.info = std::move(info);
					row.conflicted = conflicted;
					row.displayVKey = displayVKey;
					rows.push_back(std::move(row));
				}
				for (size_t i = 0; i < trackedCombos.size(); ++i) {
					LuaKeyState::TrackedComboKeyInfo& cinfo = trackedCombos[i];
					unsigned cinfoMask = ModComboTriggerMask(cinfo);
					bool conflicted = IsComboConflictedWithBindings(cinfo.activeKeys, -1, cinfoMask);
					if (!conflicted) {
						for (size_t j = 0; j < trackedCombos.size(); ++j) {
							if (i == j) continue;
							if (!VectorsEqualUnordered(cinfo.activeKeys, trackedCombos[j].activeKeys)) continue;
							unsigned otherMask = ModComboTriggerMask(trackedCombos[j]);
							if (cinfoMask != 0 && otherMask != 0 && (cinfoMask & otherMask) != 0) {
								conflicted = true;
								break;
							}
						}
					}
					UnifiedRow row;
					row.isManual = false;
					row.isComboScript = true;
					row.comboInfo = std::move(cinfo);
					row.conflicted = conflicted;
					rows.push_back(std::move(row));
				}
				for (int i = 0; i < (int)bindings.size(); i++) {
					UnifiedRow row;
					row.isManual = true;
					row.bindIndex = i;
					if (bindings[i].IsCombo()) {
						unsigned manualMask = ManualTriggerMask(bindings[i]);
						row.conflicted = IsComboConflictedWithBindings(bindings[i].comboKeys, i, manualMask);
						if (!row.conflicted) {
							for (const auto& cinfo : trackedCombos) {
								if (VectorsEqualUnordered(bindings[i].comboKeys, cinfo.activeKeys) && IsComboTriggerConflict(manualMask, cinfo)) {
									row.conflicted = true;
									break;
								}
							}
						}
					} else {
						row.conflicted = conflictedVKeys.count(bindings[i].vKey) > 0;
					}
					row.displayVKey = bindings[i].vKey;
					rows.push_back(std::move(row));
				}
				std::stable_partition(rows.begin(), rows.end(), [](const UnifiedRow& r) { return r.conflicted; });

				if (!rows.empty()) {
					ImGui::TextDisabled(UI_TXT_DESCRIBED_KEYS_HINT);
				}

				static std::unordered_map<int, std::chrono::steady_clock::time_point> disableHoldStart;
				static int pendingRemoveConfirmIndex = -1;
				static bool removeConfirmPopupRequested = false;
				static std::unordered_map<std::string, std::chrono::steady_clock::time_point> modKeyHoldStart;
				static std::string pendingResetScriptName;
				static std::string pendingResetFunctionName;
				static bool pendingResetActive = false;
				static bool resetConfirmPopupRequested = false;
				ImGui::BeginChild("KeyBindingsList", ImVec2(0, listRemainingHeight), true);
				if (rows.empty()) {
					ImGui::TextDisabled(UI_TXT_NO_KEYS_ASSIGNED);
				}
				for (size_t rowIdx = 0; rowIdx < rows.size(); ++rowIdx) {
					UnifiedRow& row = rows[rowIdx];
					ImGui::PushID((int)rowIdx);
					ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
					float rowTopY = ImGui::GetCursorPosY();
					float buttonHeight = 20.0f;
					const float keyColumnX = 250.0f;

					std::string detailText;
					if (row.isManual) {
						detailText = displayCache[row.bindIndex].detailText;
					}
					else if (row.isComboScript) {
						std::string funcStr = row.comboInfo.functionName.empty() ? "" : " [" + row.comboInfo.functionName + "]";
						detailText = "-> " + displayScriptName(row.comboInfo.scriptName) + funcStr;
					}
					else if (row.info.hasDescription) {
						std::string funcStr = row.info.functionName.empty() ? "" : " [" + row.info.functionName + "]";
						detailText = "-> " + displayScriptName(row.info.scriptName) + funcStr;
					}
					else {
						detailText = UI_TXT_NOT_YET_DESCRIBED;
					}

					const float conflictBoxHeight = 34.0f;
					ImGui::SetCursorPos(ImVec2(keyColumnX, rowTopY));
					ImGui::AlignTextToFramePadding();
					ImGui::BeginGroup();
					if (row.conflicted) {
						ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
						ImGui::BeginChild("ConflictDetailBox", ImVec2(0, conflictBoxHeight), true);
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
						ImGui::TextWrapped("%s", detailText.c_str());
						ImGui::PopStyleColor();
						ImGui::EndChild();
						ImGui::PopStyleColor();
					}
					else {
						ImGui::TextWrapped("%s", detailText.c_str());
					}
					ImGui::EndGroup();

					if (ImGui::IsItemHovered()) {
						const std::string* hoveredScriptName = nullptr;
						if (row.isComboScript) {
							hoveredScriptName = &row.comboInfo.scriptName;
						}
						else if (row.info.hasDescription) {
							hoveredScriptName = &row.info.scriptName;
						}
						if (hoveredScriptName) {
							auto modIt = modInfoByScript.find(*hoveredScriptName);
							if (modIt != modInfoByScript.end()) {
								const ModInfoRegistry::ModInfo& mi = modIt->second;
								bool hasExtra = !mi.modDescription.empty() || !mi.modCreator.empty() || !mi.modVersion.empty() || !mi.modLink.empty();
								if (hasExtra) {
									ImGui::BeginTooltip();
									ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
									if (!mi.modDescription.empty()) {
										ImGui::TextWrapped("%s", mi.modDescription.c_str());
									}
									if (!mi.modCreator.empty()) {
										ImGui::Text("Creator: %s", mi.modCreator.c_str());
									}
									if (!mi.modVersion.empty()) {
										ImGui::Text("Version: %s", mi.modVersion.c_str());
									}
									if (!mi.modLink.empty()) {
										ImGui::TextWrapped("Link: %s", mi.modLink.c_str());
									}
									ImGui::PopTextWrapPos();
									ImGui::EndTooltip();
								}
							}
						}
					}

					float detailTextHeight = ImGui::GetItemRectSize().y;
					float rowContentHeight = (detailTextHeight > buttonHeight) ? detailTextHeight : buttonHeight;
					float buttonYOffset = (rowContentHeight - buttonHeight) * 0.5f;

					ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().ItemSpacing.x, rowTopY + buttonYOffset));
					if (row.isManual) {
						int bindIdx = row.bindIndex;
						bool isDisabled = bindings[bindIdx].disabled;
						ImGui::PushStyleColor(ImGuiCol_Button, isDisabled ? ImVec4(0.5f, 0.32f, 0.08f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
						bool clicked = ImGui::Button(isDisabled ? UI_BTN_ENABLE : UI_BTN_DISABLE, ImVec2(55, buttonHeight));
						ImGui::PopStyleColor();
						ImVec2 disableBtnMin = ImGui::GetItemRectMin();
						ImVec2 disableBtnMax = ImGui::GetItemRectMax();
						if (ImGui::IsItemActive()) {
							auto holdIt = disableHoldStart.find(bindIdx);
							if (holdIt == disableHoldStart.end()) {
								disableHoldStart[bindIdx] = std::chrono::steady_clock::now();
							} else if (pendingRemoveConfirmIndex != bindIdx) {
								double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - holdIt->second).count();
								float holdProgress = (float)((std::min)(1.0, heldSeconds / kRemoveHoldSeconds));
								float barHeight = 3.0f;
								ImVec2 barMin(disableBtnMin.x, disableBtnMax.y - barHeight);
								ImVec2 barMax(disableBtnMin.x + (disableBtnMax.x - disableBtnMin.x) * holdProgress, disableBtnMax.y);
								ImGui::GetWindowDrawList()->AddRectFilled(barMin, barMax, IM_COL32(255, 70, 70, 255));
								if (heldSeconds >= kRemoveHoldSeconds) {
									pendingRemoveConfirmIndex = bindIdx;
									removeConfirmPopupRequested = true;
								}
							}
						} else {
							disableHoldStart.erase(bindIdx);
						}

						if (clicked && pendingRemoveConfirmIndex != bindIdx) {
							bindings[bindIdx].disabled = !bindings[bindIdx].disabled;
							LogActivity((bindings[bindIdx].disabled ? std::string("Disabled binding: ") : std::string("Enabled binding: ")) + CombinedDisplayName(bindings[bindIdx]));
							SaveBindings();
						}

						if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
							ImGui::SetTooltip(UI_TIP_CLICK_HOLD_REMOVE, isDisabled ? UI_WORD_ENABLE : UI_WORD_DISABLE);
						}
					}
					else if (row.conflicted) {
						ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.25f, 0.25f, 1.0f));
						ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
						ImGui::BeginChild("ConflictBadge", ImVec2(60, buttonHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoInputs);
						ImVec2 errorTextSize = ImGui::CalcTextSize(UI_LBL_ERROR);
						ImGui::SetCursorPos(ImVec2(
						    (std::max)(0.0f, (60.0f - errorTextSize.x) * 0.5f),
						    (std::max)(0.0f, (buttonHeight - errorTextSize.y) * 0.5f)));
						ImGui::TextUnformatted(UI_LBL_ERROR);
						ImGui::EndChild();
						ImGui::PopStyleVar();
						ImGui::PopStyleColor(2);
						if (ImGui::IsItemHovered()) {
							ImGui::SetTooltip("%s", (row.isComboScript || row.info.hasDescription)
								? UI_TIP_CONFLICT_REASSIGNABLE
								: UI_TIP_CONFLICT_NOT_DESCRIBED);
						}
					}
					else if (row.isComboScript || row.info.hasDescription) {
						const std::string& mkScriptName = row.isComboScript ? row.comboInfo.scriptName : row.info.scriptName;
						const std::string& mkFunctionName = row.isComboScript ? row.comboInfo.functionName : row.info.functionName;
						std::string mkHoldKey = mkScriptName + "\x1f" + mkFunctionName;
						bool hasOverride = !ModKeyBindings::GetOverride(mkScriptName, mkFunctionName).empty();
						bool isDisabled = ModKeyBindings::IsDisabled(mkScriptName, mkFunctionName);
						ImGui::PushStyleColor(ImGuiCol_Button, isDisabled ? ImVec4(0.5f, 0.32f, 0.08f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
						bool clicked = ImGui::Button(isDisabled ? UI_BTN_ENABLE : UI_BTN_DISABLE, ImVec2(55, buttonHeight));
						ImGui::PopStyleColor();
						ImVec2 modKeyBtnMin = ImGui::GetItemRectMin();
						ImVec2 modKeyBtnMax = ImGui::GetItemRectMax();
						bool isPendingThisKey = pendingResetActive && pendingResetScriptName == mkScriptName && pendingResetFunctionName == mkFunctionName;
						if (hasOverride && ImGui::IsItemActive()) {
							auto holdIt = modKeyHoldStart.find(mkHoldKey);
							if (holdIt == modKeyHoldStart.end()) {
								modKeyHoldStart[mkHoldKey] = std::chrono::steady_clock::now();
							} else if (!isPendingThisKey) {
								double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - holdIt->second).count();
								float holdProgress = (float)((std::min)(1.0, heldSeconds / kRemoveHoldSeconds));
								float barHeight = 3.0f;
								ImVec2 barMin(modKeyBtnMin.x, modKeyBtnMax.y - barHeight);
								ImVec2 barMax(modKeyBtnMin.x + (modKeyBtnMax.x - modKeyBtnMin.x) * holdProgress, modKeyBtnMax.y);
								ImGui::GetWindowDrawList()->AddRectFilled(barMin, barMax, IM_COL32(255, 70, 70, 255));
								if (heldSeconds >= kRemoveHoldSeconds) {
									pendingResetScriptName = mkScriptName;
									pendingResetFunctionName = mkFunctionName;
									pendingResetActive = true;
									resetConfirmPopupRequested = true;
								}
							}
						} else {
							modKeyHoldStart.erase(mkHoldKey);
						}

						if (clicked && !isPendingThisKey) {
							ModKeyBindings::SetDisabled(mkScriptName, mkFunctionName, !isDisabled);
							LogActivity((!isDisabled ? std::string("Disabled mod key: ") : std::string("Enabled mod key: ")) + mkScriptName + " [" + mkFunctionName + "]");
						}
						if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
							ImGui::SetTooltip(hasOverride
								? UI_TIP_CLICK_HOLD_RESET
								: UI_TIP_CLICK_NO_REMOVE, isDisabled ? UI_WORD_ENABLE : UI_WORD_DISABLE);
						}
					}
					else {
						ImGui::BeginDisabled();
						ImGui::Button(UI_BTN_SCRIPT_PLACEHOLDER, ImVec2(55, buttonHeight));
						ImGui::EndDisabled();
					}
					ImGui::SameLine();

					if (row.isManual) {
						const std::string& itemLabel = displayCache[row.bindIndex].itemLabel;
						if (ImGui::Button(itemLabel.c_str(), ImVec2(130, buttonHeight))) {
							int i = row.bindIndex;
							editingBindingIndex = i;
							captureIsCombo = bindings[i].IsCombo();
							capturedComboKeys = bindings[i].comboKeys;
							comboHoldKeys.clear();
							comboHoldActive = false;
							capturedVKey = bindings[i].vKey;
							capturedCtrl = bindings[i].needCtrl;
							capturedShift = bindings[i].needShift;
							capturedAlt = bindings[i].needAlt;
							capturedToggleMode = bindings[i].isToggle;
							capturedLongPressMode = (bindings[i].holdSeconds > 0.0f);
							capturedHoldSeconds = bindings[i].holdSeconds;
							capturedInstantMode = bindings[i].isInstant;
							capturedInstantTriggerType = bindings[i].instantTriggerType;
							capturedRepeatAccelMult = bindings[i].repeatAccelMult;
							capturedInstantUserSet = true; // editing an existing binding - these came from it, not a default
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
							requestCaptureFocus = true;
							LogActivity("Key Assignment Edit Prompt opened " + itemLabel);
						}
					}
					else if (row.isComboScript) {
						ImVec4 keyNameColor;
						bool displayKeyPressed = row.comboInfo.isPressed;
						if (row.comboInfo.hasToggleState) {
							keyNameColor = row.comboInfo.toggleEnabled ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
						}
						else {
							keyNameColor = displayKeyPressed ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
						}

						auto openComboReassignPrompt = [&row]() {
							modKeyCaptureScriptName = row.comboInfo.scriptName;
							modKeyCaptureFunctionName = row.comboInfo.functionName;
							captureIsCombo = true;
							ResetComboCaptureState();
							capturedVKey = 0;
							capturedCtrl = capturedShift = capturedAlt = false;
							capturedScriptPathOnBuffer[0] = capturedScriptPathOffBuffer[0] = '\0';
							capturedFuncOnBuffer[0] = capturedFuncOffBuffer[0] = capturedFuncTapBuffer[0] = '\0';
							capturedHasFuncOn = capturedHasFuncOff = false;
							capturedToggleType = 0;
							capturedInstantUserSet = false;
							editingBindingIndex = -1;
							isAssigningMenuToggleKey = false;
							isAssigningModKey = true;
							requestCaptureFocus = true;
							showCapturePrompt = true;
						};

						ImGui::PushStyleColor(ImGuiCol_Text, keyNameColor);
						std::string comboLabel = ComboKeysDisplayName(row.comboInfo.activeKeys);
						if (ImGui::Button(comboLabel.c_str(), ImVec2(130, buttonHeight))) {
							openComboReassignPrompt();
						}
						if (ImGui::IsItemHovered()) {
							ImGui::SetTooltip(UI_TIP_REASSIGN_COMBO);
						}
						ImGui::PopStyleColor();
					}
					else {
						ImVec4 keyNameColor;
						bool displayKeyPressed = (row.displayVKey == row.info.vKey) ? row.info.isPressed : RawInput::IsKeyHeldReal(row.displayVKey);
						if (row.info.hasToggleState) {
							keyNameColor = row.info.toggleEnabled ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
						}
						else {
							keyNameColor = displayKeyPressed ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
						}

						auto openReassignPrompt = [&row]() {
							modKeyCaptureScriptName = row.info.scriptName;
							modKeyCaptureFunctionName = row.info.functionName;
							captureIsCombo = false;
							capturedVKey = 0;
							capturedCtrl = capturedShift = capturedAlt = false;
							capturedScriptPathOnBuffer[0] = capturedScriptPathOffBuffer[0] = '\0';
							capturedFuncOnBuffer[0] = capturedFuncOffBuffer[0] = capturedFuncTapBuffer[0] = '\0';
							capturedHasFuncOn = capturedHasFuncOff = false;
							capturedToggleType = 0;
							capturedInstantUserSet = false;
							editingBindingIndex = -1;
							isAssigningMenuToggleKey = false;
							isAssigningModKey = true;
							requestCaptureFocus = true;
							showCapturePrompt = true;
						};

						ImGui::PushStyleColor(ImGuiCol_Text, keyNameColor);
						if (row.info.hasDescription) {
							if (ImGui::Button(NameForVKey(row.displayVKey).c_str(), ImVec2(130, buttonHeight))) {
								openReassignPrompt();
							}
							if (ImGui::IsItemHovered()) {
								ImGui::SetTooltip(UI_TIP_REASSIGN_KEY);
							}
						}
						else {
							ImGui::BeginDisabled();
							ImGui::Button(NameForVKey(row.displayVKey).c_str(), ImVec2(130, buttonHeight));
							ImGui::EndDisabled();
							if (ImGui::IsItemHovered()) {
								ImGui::SetTooltip(UI_TIP_CANNOT_REASSIGN_UNDESCRIBED);
							}
						}
						ImGui::PopStyleColor();
					}

					ImGui::SetCursorPosY(rowTopY + rowContentHeight + 4.0f);
					ImGui::PopID(); ImGui::Separator();
				}
				ImGui::EndChild();

				if (removeConfirmPopupRequested) {
					ImGui::OpenPopup(UI_POPUP_REMOVE_BINDING);
					removeConfirmPopupRequested = false;
				}
				ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
				bool removeConfirmOpen = ImGui::BeginPopupModal(UI_POPUP_REMOVE_BINDING, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
				ImGui::PopStyleColor();
				if (removeConfirmOpen) {
					bool indexValid = pendingRemoveConfirmIndex >= 0 && pendingRemoveConfirmIndex < (int)bindings.size();
					if (indexValid) {
						ImGui::Text(UI_FMT_REMOVE_CONFIRM, CombinedDisplayName(bindings[pendingRemoveConfirmIndex]).c_str());
						ImGui::TextDisabled(UI_TXT_CANNOT_BE_UNDONE);
					} else {
						ImGui::Text(UI_TXT_BINDING_NO_LONGER_EXISTS);
					}
					ImGui::Spacing();
					if (ImGui::Button(UI_BTN_YES, ImVec2(80, 0))) {
						if (indexValid) {
							RemoveBinding(pendingRemoveConfirmIndex);
						}
						pendingRemoveConfirmIndex = -1;
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					if (ImGui::Button(UI_BTN_NO, ImVec2(80, 0))) {
						pendingRemoveConfirmIndex = -1;
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}

				if (resetConfirmPopupRequested) {
					ImGui::OpenPopup(UI_POPUP_RESET_MOD_KEY);
					resetConfirmPopupRequested = false;
				}
				ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
				bool resetConfirmOpen = ImGui::BeginPopupModal(UI_POPUP_RESET_MOD_KEY, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
				ImGui::PopStyleColor();
				if (resetConfirmOpen) {
					ImGui::Text(UI_FMT_RESET_CONFIRM, pendingResetScriptName.c_str(), pendingResetFunctionName.c_str());
					ImGui::TextDisabled(UI_TXT_CLEARS_REASSIGNMENT);
					ImGui::Spacing();
					if (ImGui::Button(UI_BTN_YES, ImVec2(80, 0))) {
						ModKeyBindings::SetOverride(pendingResetScriptName, pendingResetFunctionName, "");
						LogActivity("Reset mod key to default: " + pendingResetScriptName + " [" + pendingResetFunctionName + "]");
						pendingResetActive = false;
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					if (ImGui::Button(UI_BTN_NO, ImVec2(80, 0))) {
						pendingResetActive = false;
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
			}
			float bottomControlPanelY = ImGui::GetWindowHeight() - paddingY - 35.0f; 
			ImGui::SetCursorPosY(bottomControlPanelY);
			
			if (bindings.empty()) ImGui::BeginDisabled();
			if (ImGui::Button(UI_BTN_CLEAR_ALL_HOTKEYS, ImVec2(145, 24))) RemoveAllBindings();
			if (bindings.empty()) ImGui::EndDisabled();
			
			ImGui::SameLine(ImGui::GetContentRegionMax().x - 165.0f);
			if (ImGui::Button(UI_BTN_ADD_NEW_BINDING, ImVec2(165, 24))) {
				editingBindingIndex = -1;
				captureIsCombo = false;
				ResetComboCaptureState();
				capturedVKey = 0;
				capturedInstantMode = false;
				capturedInstantTriggerType = 0;
				capturedInstantUserSet = false;
				showCapturePrompt = true;
				requestCaptureFocus = true;
				LogActivity("Key Assignment Binding Prompt opened");
			}
			ImGui::End();

			if (showCapturePrompt) DrawKeyCapturePrompt();
		}
	}
}
