#include "KeyBindMenu.h"
#include "RawInput.h"
#include "DirectInputHook.h"
#include "LuaBridge.h"
#include "DebuggerMenu.h"
#include "LuaKeyState.h"
#include "ModKeyBindings.h"
#include "ModInfoRegistry.h"
#include "LuaApi.h"
#include "Util.h"
#include "HookUtils.h"
#include "spdlog/spdlog.h"
#include "imgui/imgui.h"

#include <deque>
#include <fstream>
#include <filesystem>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <cmath>
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/base_sink.h"

namespace RadarKeys {
	std::atomic<bool> showCapturePrompt{ false };
	namespace KeyBindMenu {
		std::vector<KeyBind> bindings;
		bool EnsureBindsDirectory();
		bool ManualSingleOverlapsCombo(USHORT vKey, unsigned singleMask, int editingIndex, const std::string& ignoreScript = "", const std::string& ignoreFunc = "");
		bool ManualComboOverlapsSingle(const std::vector<USHORT>& comboKeys, unsigned comboMask, int editingIndex, const std::string& ignoreScript = "", const std::string& ignoreFunc = "");

		static float capturedHoldSeconds = 0.0f;
		static std::unordered_set<USHORT> prevHeldPadKeysCapture;
		static bool padCaptureEdgePrimed = false;
		static std::unordered_set<USHORT> prevHeldPsKeysCapture;
		static bool psCaptureEdgePrimed = false;
		static bool capturedInstantMode = false;
		static int capturedInstantTriggerType = 0;
		static bool isAssigningMenuToggleKey = false; 
		static bool isAssigningModKey = false;
		static bool requestCaptureFocus = false;

		// ==================== UI Strings ====================
		static const char* LOG_WARNING_PREVIOUS_SESSION_DID_NOT_CLOSE = "[WARNING] The previous session did not close cleanly (Crashed or Terminated Abruptly).";
		static const char* LOG_RADARKEYS_DIAGNOSTICS_SINGLE_LOG_FMT_PREVIOUS = "RadarKeys: diagnostics: single log at {} (previous session preserved at {})";
		static const char* LOG_INITDIAGNOSTICS_FAILED_FMT_DIAGNOSTICS_STAY_DEFAULT = "KeyBindMenu: InitDiagnostics failed ({}); diagnostics stay on the default sink";
		static const char* LOG_KEYBINDMENU_COULDN_T_CREATE_FMT_DIRECTORY = "KeyBindMenu: couldn't create {} directory: {}";
		static const char* LOG_OK_FMT = "[OK] {}";
		static const char* LOG_FAIL_FMT = "[FAIL] {}";
		static const char* LOG_STATE_CLEAN_EXIT = "[STATE] CLEAN_EXIT";
		static const char* LOG_LOGSINK_INITIALIZATION_COMPLETE = "[STATE] Initialization done - Rolling Log starts below";
		static const char* LOG_KEYBINDMENU_FUNCTION_NOT_TRIGGERED_NOT_REASSIGNED = "KeyBindMenu: Function not triggered for now - the mod script's assigned key has not been reassigned yet (assign a new binding in the menu to enable firing) - key: ";
		static const char* LOG_KEYBINDMENU_MAIN_MENU_FMT_F7 = "KeyBindMenu: main menu {} (F7)";
		static const char* LOG_KEYBINDMENU_LOADBINDINGS_SKIPPING_INCOMPLETE_BIND_LI = "KeyBindMenu::LoadBindings: skipping incomplete BIND line: {}";
		static const char* LOG_KEYBINDMENU_INJECTDESCRIBE_MALFORMED_FMT = "KeyBindMenu: InjectDescribe: dropped malformed payload (fields: {})";
		static const char* LOG_KEYBINDMENU_INJECTDESCRIBE_UNKNOWN_KEY_FMT = "KeyBindMenu: InjectDescribe: dropped - unrecognized key name '{}'";
		static const char* LOG_KEYBINDMENU_INJECTDESCRIBE_BAD_RANGE_FMT = "KeyBindMenu: InjectDescribe: dropped - invalid line range {}-{}";
		static const char* LOG_KEYBINDMENU_INJECTDESCRIBE_SOURCE_UNREADABLE_FMT = "KeyBindMenu: InjectDescribe: source unreadable, binding armed for fire-time rebuild: {}";
		static const char* LOG_KEYBINDMENU_INJECT_SOURCE_LOST_AUTO_DISABLED_FMT = "KeyBindMenu: Script-line binding auto-disabled - source unreadable: {}";
		static const char* LOG_KEYBINDMENU_INJECT_SOURCE_RESTORED_AUTO_ENABLED_FMT = "KeyBindMenu: Script-line binding re-enabled - source restored: {}";
		static const char* LOG_KEYBINDMENU_INJECTDESCRIBE_UPDATED_FMT = "KeyBindMenu: Updated script-lines binding: {} -> lines {}-{} of {}";
		static const char* LOG_RADARKEYS_KEYBINDMENU_INITIALIZING = "KeyBindMenu: initializing";
		static const char* LOG_KEY_ASSIGNMENT_PROMPT_CANCELLED = "KeyBindMenu: Key Assignment Prompt cancelled";
		static const char* LOG_MULTI_KEY_COMBO_CAPTURE_CANCELLED_KEY = "KeyBindMenu: Multi-key combo capture cancelled - a key was released before the hold completed";
		static const char* LOG_MULTI_KEY_COMBO_CAPTURE_CANCELLED_MORE = "KeyBindMenu: Multi-key combo capture cancelled - more than 3 keys held";
		static const char* LOG_SINGLE_KEY_CAPTURE_CANCELLED_KEY_RELEASED = "KeyBindMenu: Single-key capture cancelled - key was released before the hold completed";
		static const char* LOG_KEYBINDMENU_CAPTURED_SINGLE_KEY_VKEY_FMT = "KeyBindMenu: captured single key vKey={} name=\"{}\" ctrl={} shift={} alt={}";
		static const char* LOG_KEYBIND_HAS_BEEN_RESET = "KeyBindMenu: Keybind has been reset";
		static const char* LOG_MULTI_KEY_COMBO_HAS_BEEN_RESET = "KeyBindMenu: Multi-key combo has been reset";
		static const char* LOG_KEYBINDMENU_DEBUGGER_OVERLAY_FMT = "KeyBindMenu: Debugger overlay {}";
		static const char* LOG_MENU_KEY_REASSIGNMENT_PROMPT_OPENED = "KeyBindMenu: Menu Key Reassignment Prompt opened";
		static const char* LOG_KEY_ASSIGNMENT_BINDING_PROMPT_OPENED = "KeyBindMenu: Key Assignment Binding Prompt opened";

		static const char* UI_FMT_SCRIPT_FUNCTION_BRACKETS = "%s [%s]";
		static const char* UI_LBL_BIND_TYPE = "Bind Type:";
		static const char* UI_RADIO_SINGLE_KEY = "Single Key";
		static const char* UI_RADIO_MULTI_KEY_COMBO = "Multi-Key Combo";
		static const char* UI_RADIO_SCRIPT_LINES = "Script Lines";
		static const char* UI_LBL_LINE_START = "Line Start:";
		static const char* UI_LBL_LINE_END = "Line End:";
		static const char* UI_LBL_KEY = "Key";
		static const char* UI_LBL_PRESS = "PRESS";
		static const char* UI_LBL_HOLD = "HOLD";
		static const char* UI_LBL_KEY_ELLIPSIS = "KEY...";
		static const char* UI_BTN_RESET = "Reset";
		static const char* UI_LBL_KEYS = "Keys";
		static const char* UI_LBL_HOLD_INDENT = "  HOLD";
		static const char* UI_LBL_2_3_KEYS = "  2-3 KEYS";
		static const char* UI_TIP_COMBO_HOLD = "Hold every key in the combo down for %.1fs.\nReleasing any key before then cancels the capture.";
		static const char* UI_CHK_TOGGLE = "Toggle";
		static const char* UI_TIP_UNCHECK_INSTANT_FIRST = "Uncheck Instant first to use Toggle or Long Press.";
		static const char* UI_TIP_TOGGLE_SCRIPT_UNAVAILABLE = "Toggle is not available for script bindings";
		static const char* UI_CHK_LONG_PRESS = "Long Press";
		static const char* UI_BTN_MINUS = " - ";
		static const char* UI_BTN_PLUS = " + ";
		static const char* UI_CHK_INSTANT = "Instant";
		static const char* UI_TIP_UNCHECK_TOGGLE_FIRST = "Uncheck Toggle or Long Press first to use Instant.";
		static const char* UI_OPT_ON_PRESS = "On Press";
		static const char* UI_OPT_ON_RELEASE = "On Release";
		static const char* UI_OPT_REPEAT = "Repeat";
		static const char* UI_TIP_REPEAT_ACCEL =
			"Acceleration multiplier for the Repeat interval.\n"
			"- ? > 1.00: Faster\n"
			"- ? < 1.00: Slower";
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
		static const char* UI_HDR_CAPTURE_SUPPRESSION = "Suppress input to game (while this menu is open):";
		static const char* UI_CHK_SUPPRESS_KEYBOARD = "Keyboard";
		static const char* UI_CHK_SUPPRESS_MOUSE = "Mouse";
		static const char* UI_CHK_SUPPRESS_GAMEPAD = "Gamepad";
		static const char* UI_TIP_CAPTURE_SUPPRESSION =
			"While this menu (or the Debugger overlay) is open, checking a device here hides it from the game right\n"
			"away so Venom Snake doesn't move, aim or fire - unchecking it restores that device right away too.\n"
			"Suppression only ever applies while this window is visibly open. Closing it always restores all input to the game.";
		static const char* UI_HDR_KEY_BINDINGS = "Key Bindings";
		static const char* UI_TXT_DESCRIBED_KEYS_HINT = "Described information will be displayed when the script names are hovered by the mouse.";
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
		static const char* UI_TIP_INJECT_SOURCE_MISSING = "Script source for this trigger was not found in the modules folder - Restore the file to re-enable.";
		static const char* UI_BTN_SCRIPT_PLACEHOLDER = "Script";
		static const char* UI_TIP_REASSIGN_COMBO = "Click to reassign this combo.\nSaved in radar_keybinds.conf in the (...modules/radarKeys) folder.";
		static const char* UI_TIP_REASSIGN_KEY = "Click to reassign this key.\nSaved in radar_keybinds.conf in the (...modules/radarKeys) folder.";
		static const char* UI_LBL_KEY_GROUP_SEPARATOR = " / ";
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
		static const char* UI_BTN_CLEAR_ALL_HOTKEYS = "Disable All Hotkeys";
		static const char* UI_TIP_CLICK_HOLD_CLEAR_ALL = "Click to disable everything in the list.\nHold for 1.5 seconds to reset mod keys to default and remove manual bindings.";
		static const char* UI_POPUP_CLEAR_ALL_CONFIRM = "Clear All Hotkeys?";
		static const char* UI_TXT_CLEAR_ALL_CONFIRM = "Reset all mod key overrides to their defaults and remove every manually-assigned binding?";
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

		class BootStampSink : public spdlog::sinks::base_sink<std::mutex> {
			private:
				enum class Phase { Booting, Live };
				static constexpr size_t kMaxActivityLines = 50;
			public:
				explicit BootStampSink(spdlog::sink_ptr downstream, const std::string& logFilePath)
				: downstream_(std::move(downstream)), filePath_(logFilePath) {
					downstream_->set_pattern("[%l] %v");
				}
				void MarkInitializationComplete() {
					std::lock_guard<std::mutex> lock(mutex_);
					if (phase_ != Phase::Booting || finalized_) return;
					const std::string text(LOG_LOGSINK_INITIALIZATION_COMPLETE);
					std::string line = StampPayload(text.data(), text.size());
					spdlog::details::log_msg stamped(spdlog::string_view_t(), spdlog::level::info, spdlog::string_view_t(line.data(), line.size()));
					downstream_->log(stamped);
					downstream_->flush();
					std::error_code sizeEc;
					interactionStart_ = static_cast<std::streamoff>(std::filesystem::file_size(filePath_, sizeEc));
					if (interactionStart_ <= 0) interactionStart_ = 0;
					phase_ = Phase::Live;
				}
			protected:
				void sink_it_(const spdlog::details::log_msg& msg) override {
					std::string line = StampPayload(msg.payload.data(), msg.payload.size());
					if (phase_ == Phase::Booting) {
						spdlog::details::log_msg stamped(msg.logger_name, msg.level, spdlog::string_view_t(line.data(), line.size()));
						downstream_->log(stamped);
						return;
					}
					spdlog::string_view_t levelTag = spdlog::level::to_string_view(msg.level);
					line = "[" + std::string(levelTag.data(), levelTag.size()) + "] " + line;
					if (line.find(LOG_STATE_CLEAN_EXIT) != std::string::npos) {
						CompactTailLocked();
						AppendTail(line + "\n");
						finalized_ = true;
						return;
					}
					AppendTail(line + "\n");
					ring_.push_back(line + "\n");
					if (ring_.size() > kMaxActivityLines) {
						ring_.pop_front();
						CompactTailLocked();
					}
				}
				void flush_() override {
					downstream_->flush();
					if (tailFile_.is_open()) tailFile_.flush();
				}
			private:
				std::string StampPayload(const char* data, size_t len) {
					auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - bootTime_);
					auto hours = std::chrono::duration_cast<std::chrono::hours>(durationMs);
					durationMs -= hours;
					auto minutes = std::chrono::duration_cast<std::chrono::minutes>(durationMs);
					durationMs -= minutes;
					auto seconds = std::chrono::duration_cast<std::chrono::seconds>(durationMs);
					durationMs -= seconds;
					char stamp[32];
					snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d.%03d] ", (int)hours.count(), (int)minutes.count(), (int)seconds.count(), (int)durationMs.count());
					std::string line;
					line.reserve(len + 32);
					line.append(stamp);
					line.append(data, len);
					return line;
				}
				void AppendTail(const std::string& line) {
					if (!tailFile_.is_open()) {
						tailFile_.open(filePath_, std::ios::app | std::ios::binary);
					}
					if (tailFile_.is_open()) {
						tailFile_.write(line.data(), static_cast<std::streamsize>(line.size()));
						tailFile_.flush();
					}
				}
				void CompactTailLocked() {
					tailFile_.flush();
					std::ofstream out(filePath_, std::ios::in | std::ios::out | std::ios::binary);
					if (!out) return;
					out.seekp(interactionStart_);
					std::streamoff total = 0;
					for (const auto& l : ring_) {
						out.write(l.data(), static_cast<std::streamsize>(l.size()));
						total += static_cast<std::streamoff>(l.size());
					}
					out.close();
					std::error_code resizeEc;
					std::filesystem::resize_file(filePath_, interactionStart_ + total, resizeEc);
				}
				spdlog::sink_ptr downstream_;
				std::string filePath_;
				std::chrono::steady_clock::time_point bootTime_ = std::chrono::steady_clock::now();
				Phase phase_ = Phase::Booting;
				bool finalized_ = false;
				std::streamoff interactionStart_ = 0;
				std::deque<std::string> ring_;
				std::ofstream tailFile_;
			};

			static BootStampSink* g_activityLogSink = nullptr;

			void MarkActivityLogLive() {
				if (g_activityLogSink) {
					g_activityLogSink->MarkInitializationComplete();
				}
			}

		bool PreviousSessionEndedCleanly(const std::string& logPath);
		void InitDiagnostics() {
			static bool initialized = false;
			if (initialized) return;
			initialized = true;
			try {
				EnsureBindsDirectory();
				std::error_code logEc;
				const std::filesystem::path logPath(GetLogFileName());
				const std::filesystem::path prevPath = logPath.parent_path() / "radarkeys_log_prev.txt";
				const bool wasClean = PreviousSessionEndedCleanly(logPath.string());
				if (std::filesystem::exists(logPath, logEc)) {
					std::filesystem::copy_file(logPath, prevPath,
					std::filesystem::copy_options::overwrite_existing, logEc);
				}
				for (int rolledIndex = 1; rolledIndex <= 3; ++rolledIndex) {
					std::filesystem::remove(logPath.parent_path() /
					("radarkeys_log." + std::to_string(rolledIndex) + ".txt"), logEc);
				}
				auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
				auto sink = std::make_shared<BootStampSink>(fileSink, logPath.string());
				g_activityLogSink = sink.get();
				auto logger = std::make_shared<spdlog::logger>("radarkeys", sink);
				std::error_code verboseEc;
				const bool verboseRequested = std::filesystem::exists(
					logPath.parent_path() / "radarkeys_verbose_log.txt", verboseEc);
				logger->set_level(verboseRequested ? spdlog::level::trace : spdlog::level::debug);
				logger->flush_on(spdlog::level::info);
				spdlog::set_default_logger(logger);
				spdlog::flush_every(std::chrono::seconds(5));
				if (!wasClean) {
					spdlog::warn(LOG_WARNING_PREVIOUS_SESSION_DID_NOT_CLOSE);
				}
				if (verboseRequested) {
					spdlog::info("KeyBindMenu: Verbose (trace-level) logging ENABLED (marker file: mod/radarKeys/radarkeys_verbose_log.txt)");
				}
				spdlog::info(LOG_RADARKEYS_DIAGNOSTICS_SINGLE_LOG_FMT_PREVIOUS,
				FileNameOnly(logPath.string()), FileNameOnly(prevPath.string()));
			} catch (const std::exception& e) {
				spdlog::warn(LOG_INITDIAGNOSTICS_FAILED_FMT_DIAGNOSTICS_STAY_DEFAULT, e.what());
			}
		}

		bool EnsureBindsDirectory() {
			static bool ensured = false;
			if (ensured) return true;

			std::error_code ec;
			std::filesystem::path bindsDir = std::filesystem::path(GetGameDirectory()) / "mod" / "radarKeys";
			std::filesystem::create_directories(bindsDir, ec);
			if (ec) {
				spdlog::warn(LOG_KEYBINDMENU_COULDN_T_CREATE_FMT_DIRECTORY, FileNameOnly(bindsDir.string()), ec.message());
				return false;
			}
			ensured = true;
			return true;
		}

		bool PreviousSessionEndedCleanly(const std::string& logPath) {
			std::ifstream in(logPath);
			if (!in) return true;
			std::string line, lastNonEmptyLine;
			while (std::getline(in, line)) {
				if (!line.empty()) lastNonEmptyLine = line;
			}
			return lastNonEmptyLine.empty() ||
				(lastNonEmptyLine.size() >= 18 &&
				lastNonEmptyLine.compare(lastNonEmptyLine.size() - 18, 18, "[STATE] CLEAN_EXIT") == 0);
		}
		
		void LogActivity(const std::string& message, bool success) {
			if (success) {
				spdlog::info(LOG_OK_FMT, message);
			} else {
				spdlog::warn(LOG_FAIL_FMT, message);
			}
		}
		
		void LogCleanShutdown() {
			spdlog::info(LOG_STATE_CLEAN_EXIT);
			spdlog::default_logger()->flush();
		}

		struct VkNameEntry { const char* name; USHORT vKey; };
		const VkNameEntry vkNameTable[] = {
			// Alphabeticals
			{"A", 'A'}, {"B", 'B'}, {"C", 'C'}, {"D", 'D'}, {"E", 'E'}, {"F", 'F'},
			{"G", 'G'}, {"H", 'H'}, {"I", 'I'}, {"J", 'J'}, {"K", 'K'}, {"L", 'L'},
			{"M", 'M'}, {"N", 'N'}, {"O", 'O'}, {"P", 'P'}, {"Q", 'Q'}, {"R", 'R'},
			{"S", 'S'}, {"T", 'T'}, {"U", 'U'}, {"V", 'V'}, {"W", 'W'}, {"X", 'X'},
			{"Y", 'Y'}, {"Z", 'Z'},

			// Numericals
			{"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'},
			{"5", '5'}, {"6", '6'}, {"7", '7'}, {"8", '8'}, {"9", '9'},

			// Functions (added all of them cause for some reason, there are 24 of them in the Virtual Key Codes list. lmao)
			{"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
			{"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
			{"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
			{"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15}, {"F16", VK_F16},
			{"F17", VK_F17}, {"F18", VK_F18}, {"F19", VK_F19}, {"F20", VK_F20},
			{"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24},

			// Navigationals
			{"Space", VK_SPACE}, {"Tab", VK_TAB}, {"Enter", VK_RETURN}, {"Backspace", VK_BACK},
			{"Insert", VK_INSERT}, {"Delete", VK_DELETE},
			{"Home", VK_HOME}, {"End", VK_END},
			{"Page Up", VK_PRIOR}, {"Page Down", VK_NEXT},
			{"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},

			// Numpad
			{"Numpad 0", VK_NUMPAD0}, {"Numpad 1", VK_NUMPAD1}, {"Numpad 2", VK_NUMPAD2},
			{"Numpad 3", VK_NUMPAD3}, {"Numpad 4", VK_NUMPAD4}, {"Numpad 5", VK_NUMPAD5},
			{"Numpad 6", VK_NUMPAD6}, {"Numpad 7", VK_NUMPAD7}, {"Numpad 8", VK_NUMPAD8},
			{"Numpad 9", VK_NUMPAD9}, {"Numpad Asterisk", VK_MULTIPLY}, {"Numpad Plus", VK_ADD}, 
			{"Numpad Minus", VK_SUBTRACT}, {"Numpad Period", VK_DECIMAL}, {"Numpad Slash", VK_DIVIDE},

			// Typo-Graphics and Punctuation Symbols
			{",", VK_OEM_COMMA}, {".", VK_OEM_PERIOD},
			{";", VK_OEM_1}, {"=", VK_OEM_PLUS}, {"-", VK_OEM_MINUS}, {"/", VK_OEM_2},
			{"`", VK_OEM_3}, {"[", VK_OEM_4}, {"\\", VK_OEM_5}, {"]", VK_OEM_6}, {"'", VK_OEM_7},

			// Utility
			{"Caps Lock", VK_CAPITAL}, {"Num Lock", VK_NUMLOCK}, {"Scroll Lock", VK_SCROLL},
			{"Print Screen", VK_SNAPSHOT}, {"Pause", VK_PAUSE}, {"Menu Key", VK_APPS},

			// Media Buttons
			{"Volume Up", VK_VOLUME_UP}, {"Volume Down", VK_VOLUME_DOWN}, {"Mute", VK_VOLUME_MUTE},
			{"Play/Pause", VK_MEDIA_PLAY_PAUSE}, {"Stop", VK_MEDIA_STOP},
			{"Next", VK_MEDIA_NEXT_TRACK}, {"Previous", VK_MEDIA_PREV_TRACK},

			// Modifiers
			{"Ctrl", VK_CONTROL}, {"Shift", VK_SHIFT}, {"Alt", VK_MENU},

			// Mouse
			{"Mouse Wheel", VK_MBUTTON},
			{"Mouse 4", VK_XBUTTON1},
			{"Mouse 5", VK_XBUTTON2},
			{"Right Click", VK_RBUTTON}
		};
		const int vkNameTableCount = sizeof(vkNameTable) / sizeof(vkNameTable[0]);

		const VkNameEntry xboxVkNameTable[] = {
			{"XB A", VK_GAMEPAD_A}, {"XB B", VK_GAMEPAD_B},
			{"XB X", VK_GAMEPAD_X}, {"XB Y", VK_GAMEPAD_Y},
			{"XB LB", VK_GAMEPAD_LEFT_SHOULDER}, {"XB RB", VK_GAMEPAD_RIGHT_SHOULDER},
			{"XB LT", VK_GAMEPAD_LEFT_TRIGGER}, {"XB RT", VK_GAMEPAD_RIGHT_TRIGGER},
			{"XB Menu", VK_GAMEPAD_MENU}, {"XB View", VK_GAMEPAD_VIEW},
			{"XB Up", VK_GAMEPAD_DPAD_UP}, {"XB Down", VK_GAMEPAD_DPAD_DOWN},
			{"XB Left", VK_GAMEPAD_DPAD_LEFT}, {"XB Right", VK_GAMEPAD_DPAD_RIGHT},
			{"XB LS Up", VK_GAMEPAD_LEFT_THUMBSTICK_UP}, {"XB RS Up", VK_GAMEPAD_RIGHT_THUMBSTICK_UP},
			{"XB LS Down", VK_GAMEPAD_LEFT_THUMBSTICK_DOWN}, {"XB RS Down", VK_GAMEPAD_RIGHT_THUMBSTICK_DOWN},
			{"XB LS Left", VK_GAMEPAD_LEFT_THUMBSTICK_LEFT}, {"XB RS Left", VK_GAMEPAD_RIGHT_THUMBSTICK_LEFT},
			{"XB LS Right", VK_GAMEPAD_LEFT_THUMBSTICK_RIGHT}, {"XB RS Right", VK_GAMEPAD_RIGHT_THUMBSTICK_RIGHT},
			{"XB LS L3", VK_GAMEPAD_LEFT_THUMBSTICK_BUTTON}, {"XB RS R3", VK_GAMEPAD_RIGHT_THUMBSTICK_BUTTON}
		};
		const int xboxVkNameTableCount = sizeof(xboxVkNameTable) / sizeof(xboxVkNameTable[0]);

		const VkNameEntry psVkNameTable[] = {
			{"PS Cross", RawInput::VK_PS_CROSS}, {"PS Circle", RawInput::VK_PS_CIRCLE},
			{"PS Square", RawInput::VK_PS_SQUARE}, {"PS Triangle", RawInput::VK_PS_TRIANGLE},
			{"PS L1", RawInput::VK_PS_L1}, {"PS R1", RawInput::VK_PS_R1},
			{"PS L2", RawInput::VK_PS_L2}, {"PS R2", RawInput::VK_PS_R2},
			{"PS Share", RawInput::VK_PS_SHARE}, {"PS Options", RawInput::VK_PS_OPTIONS},
			{"PS Up", RawInput::VK_PS_DPAD_UP}, {"PS Down", RawInput::VK_PS_DPAD_DOWN},
			{"PS Left", RawInput::VK_PS_DPAD_LEFT}, {"PS Right", RawInput::VK_PS_DPAD_RIGHT},
			{"PS LS Up", RawInput::VK_PS_LS_UP}, {"PS RS Up", RawInput::VK_PS_RS_UP},
			{"PS LS Down", RawInput::VK_PS_LS_DOWN}, {"PS RS Down", RawInput::VK_PS_RS_DOWN},
			{"PS LS Left", RawInput::VK_PS_LS_LEFT}, {"PS RS Left", RawInput::VK_PS_RS_LEFT},
			{"PS LS Right", RawInput::VK_PS_LS_RIGHT}, {"PS RS Right", RawInput::VK_PS_RS_RIGHT},
			{"PS L3", RawInput::VK_PS_L3}, {"PS R3", RawInput::VK_PS_R3}
		};
		const int psVkNameTableCount = sizeof(psVkNameTable) / sizeof(psVkNameTable[0]);

		static bool IsGamepadVKeyValue(USHORT vKey) {
			return vKey >= VK_GAMEPAD_A && vKey <= VK_GAMEPAD_RIGHT_THUMBSTICK_RIGHT;
		}

		static bool IsPlaystationVKeyValue(USHORT vKey) {
			return vKey >= RawInput::VK_PS_CROSS && vKey <= RawInput::VK_PS_RS_RIGHT;
		}

		ModKeyBindings::BindSlot SlotOfVKey(USHORT vKey) {
			if (IsGamepadVKeyValue(vKey) || IsPlaystationVKeyValue(vKey)) {
				return ModKeyBindings::BindSlot::Pad;
			}
			return ModKeyBindings::BindSlot::Kbm;
		}

		std::string NameForVKey(USHORT vKey) {
			if (IsGamepadVKeyValue(vKey)) {
				for (const auto& entry : xboxVkNameTable) {
					if (entry.vKey == vKey) return entry.name;
				}
			} else if (IsPlaystationVKeyValue(vKey)) {
				for (const auto& entry : psVkNameTable) {
					if (entry.vKey == vKey) return entry.name;
				}
			}
			for (const auto& entry : vkNameTable) {
				if (entry.vKey == vKey) return entry.name;
			}
			for (const auto& entry : xboxVkNameTable) {
				if (entry.vKey == vKey) return entry.name;
			}
			for (const auto& entry : psVkNameTable) {
				if (entry.vKey == vKey) return entry.name;
			}
			return "Unknown(" + std::to_string(vKey) + ")";
		}

		int ResolveKeyNameForSlot(const std::string& name, USHORT referenceVKey) {
			int resolved = VKeyForName(name);
			if (resolved > 0) return resolved;
			std::vector<USHORT> members = ParseComboKeyNames(name);
			for (USHORT member : members) {
				if (SlotOfVKey(member) == SlotOfVKey(referenceVKey)) return (int)member;
			}
			return members.empty() ? -1 : (int)members.front();
		}

		USHORT NativeVKeyForMod(const std::string& scriptName, const std::string& functionName) {
			std::string nativeName = ModKeyBindings::GetNativeKey(scriptName, functionName);
			if (!nativeName.empty()) {
				int resolved = ResolveKeyNameForSlot(nativeName, 0);
				if (resolved > 0) return (USHORT)resolved;
			}
			for (const auto& b : bindings) {
				if (!b.isInject || !b.scriptDescribed || b.injectScriptName != scriptName || b.injectFunctionName != functionName) continue;
				if (b.nativeVKey != 0) return b.nativeVKey;
			}
			return 0;
		}

		int VKeyForName(const std::string& name) {
			for (const auto& entry : vkNameTable) {
				if (name == entry.name) return entry.vKey;
			}
			for (const auto& entry : xboxVkNameTable) {
				if (name == entry.name) return entry.vKey;
			}
			for (const auto& entry : psVkNameTable) {
				if (name == entry.name) return entry.vKey;
			}
			return -1;
		}

		std::vector<USHORT> ParseComboKeyNames(const std::string& raw) {
			std::vector<USHORT> result;
			static const char kNumpadPlusGuard = '\x1f';
			std::string guarded = raw;
			{
				const char* needle = "Numpad +";
				size_t guardPos = 0;
				while ((guardPos = guarded.find(needle, guardPos)) != std::string::npos) {
					guarded.replace(guardPos, std::strlen(needle), 1, kNumpadPlusGuard);
				}
			}
			std::stringstream ss(guarded);
			std::string part;
			while (std::getline(ss, part, '+')) {
				while (!part.empty() && std::isspace((unsigned char)part.front())) part.erase(part.begin());
				while (!part.empty() && std::isspace((unsigned char)part.back())) part.pop_back();
				if (part.size() == 1 && part[0] == kNumpadPlusGuard) part = "Numpad +";
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
			std::string liveName = bind.keyName;
			if (bind.vKey != 0) {
				std::string resolved = NameForVKey(bind.vKey);
				if (resolved.compare(0, 8, "Unknown(") != 0) {
					liveName = resolved;
				}
			}
			std::string result = std::string(bind.needCtrl ? "Ctrl+" : "") + (bind.needShift ? "Shift+" : "") + (bind.needAlt ? "Alt+" : "") + liveName;
			if (bind.holdSeconds > 0.0f) {
				char buf[32];
				snprintf(buf, sizeof(buf), " (hold %.1fs)", bind.holdSeconds);
				result += buf;
			}
			return result;
		}

		bool HasLuaExtensionCI(const std::string& fileName) {
			if (fileName.size() < 4) return false;
			std::string tail = fileName.substr(fileName.size() - 4);
			for (char& c : tail) c = (char)std::tolower((unsigned char)c);
			return tail == ".lua";
		}

		std::string ResolveScriptPath(const std::string& typedPath) {
			if (std::filesystem::path(typedPath).is_absolute()) return typedPath;
			bool hasSeparators = typedPath.find('/') != std::string::npos || typedPath.find('\\') != std::string::npos;
			return (std::filesystem::path(GetGameDirectory()) / (hasSeparators ? std::filesystem::path(typedPath) : std::filesystem::path("mod") / "modules" / typedPath)).string();
		}

		std::string BuildInjectContent(const std::string& sourcePath, int lineStart, int lineEnd) {
			if (lineStart < 1) return "";
			std::ifstream inFile(sourcePath);
			if (!inFile) return "";
			std::vector<std::string> sourceLines;
			std::string sourceLine;
			while (std::getline(inFile, sourceLine)) {
				while (!sourceLine.empty() && sourceLine.back() == '\r') sourceLine.pop_back();
				sourceLines.push_back(sourceLine);
			}
			inFile.close();
			int lastLine = lineEnd < lineStart ? lineStart : lineEnd;
			if (lastLine > (int)sourceLines.size()) lastLine = (int)sourceLines.size();
			std::string content;
			for (int i = lineStart; i <= lastLine; i++) {
				content += ((i <= (int)sourceLines.size()) ? sourceLines[i - 1] : "");
				content += "\n";
			}
			return content;
		}

		USHORT menuToggleVKey = VK_F7;
		RawInput::ActionHandle menuToggleHandle = 0;
		bool menuOpen = false;
		bool captureSuppressKeyboard = true;
		bool captureSuppressMouse = true;
		bool captureSuppressGamepad = true;

		std::unordered_set<USHORT> activeBindVKeys;
		void OnMenuToggleKeyPressed(RawInput::BUTTONEVENT buttonEvent) {
			if (buttonEvent != RawInput::BUTTONEVENT::ONDOWN) {
				return;
			}
			
			if (showCapturePrompt) {
				return;
			}

			menuOpen = !menuOpen;
			if (menuOpen) {
				MarkActivityLogLive();
			}
			LogActivity(menuOpen ? "Menu opened" : "Menu closed");
			spdlog::info(LOG_KEYBINDMENU_MAIN_MENU_FMT_F7, menuOpen ? "OPENED" : "CLOSED");
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

		bool HasHoldSiblingOnSameKey(const KeyBind& bind) {
			for (const auto& other : bindings) {
				if (&other == &bind) continue;
				if (other.holdSeconds <= 0.0f || other.disabled) continue;
				if (bind.IsCombo() || other.IsCombo()) {
					if (bind.IsCombo() && other.IsCombo() && VectorsEqualUnordered(bind.comboKeys, other.comboKeys)) return true;
					continue;
				}
				if (other.vKey == bind.vKey && other.needCtrl == bind.needCtrl &&
					other.needShift == bind.needShift && other.needAlt == bind.needAlt) return true;
			}
			return false;
		}

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
				mask |= HasHoldSiblingOnSameKey(bind) ? Trigger_OnRelease : Trigger_OnPress;
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

		unsigned ManualCaptureTriggerMask(USHORT vKey = 0, bool needCtrl = false, bool needShift = false, bool needAlt = false) {
			KeyBind capture{};
			capture.vKey = vKey;
			capture.needCtrl = needCtrl;
			capture.needShift = needShift;
			capture.needAlt = needAlt;
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
			for (const auto& bind : bindings) {
				if (!bind.isInject || !bind.scriptDescribed || bind.vKey != vKey) continue;
				if (bind.injectScriptName == scriptName && bind.injectFunctionName == functionName) continue;
				return false;
			}
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
			if (ManualSingleOverlapsCombo(vKey, modMask, -1, scriptName, functionName)) return false;
			return true;
		}

		bool IsModComboAssignmentAvailable(const std::vector<USHORT>& comboKeys, const std::string& scriptName, const std::string& functionName) {
			for (USHORT k : comboKeys) {
				if (IsReservedVKey(k)) return false;
			}
			for (const auto& bind : bindings) {
				if (!bind.isInject || !bind.scriptDescribed) continue;
				if (bind.injectScriptName == scriptName && bind.injectFunctionName == functionName) continue;
				for (USHORT k : comboKeys) {
					if (bind.vKey == k) return false;
				}
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
			if (ManualComboOverlapsSingle(comboKeys, modMask, -1, scriptName, functionName)) return false;
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
			if (ManualSingleOverlapsCombo(vKey, manualMask, editingIndex)) return false;
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

			if (ManualComboOverlapsSingle(comboKeys, manualMask, editingIndex, ignoredScriptName, ignoredFunctionName)) return false;
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
			bool comboActiveDuringPress = false;
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
		USHORT ResolveDisplayVKey(const LuaKeyState::TrackedKeyInfo& info);

		bool ManualSingleOverlapsCombo(USHORT vKey, unsigned singleMask, int editingIndex, const std::string& ignoreScript, const std::string& ignoreFunc) {
			for (int i = 0; i < (int)bindings.size(); ++i) {
				if (i == editingIndex) continue;
				const KeyBind& bind = bindings[i];
				if (!bind.IsCombo() || bind.disabled) continue;
				bool isMember = false;
				for (USHORT k : bind.comboKeys) if (k == vKey) { isMember = true; break; }
				if (isMember && (ManualTriggerMask(bind) & singleMask) != 0) return true;
			}
			for (const auto& info : LuaKeyState::GetTrackedComboKeyInfo()) {
				if (!ignoreScript.empty() && info.scriptName == ignoreScript && info.functionName == ignoreFunc) continue;
				bool isMember = false;
				for (USHORT k : info.activeKeys) if (k == vKey) { isMember = true; break; }
				if (isMember && IsComboTriggerConflict(singleMask, info)) return true;
			}
			return false;
		}

		bool ManualComboOverlapsSingle(const std::vector<USHORT>& comboKeys, unsigned comboMask, int editingIndex, const std::string& ignoreScript, const std::string& ignoreFunc) {
			for (int i = 0; i < (int)bindings.size(); ++i) {
				if (i == editingIndex) continue;
				const KeyBind& bind = bindings[i];
				if (bind.IsCombo() || bind.disabled) continue;
				bool hitsMember = false;
				for (USHORT k : comboKeys) if (k == bind.vKey) { hitsMember = true; break; }
				if (hitsMember && (ManualTriggerMask(bind) & comboMask) != 0) return true;
			}
			for (const auto& info : LuaKeyState::GetTrackedKeyInfo()) {
				if (!info.hasDescription) continue;
				if (!ignoreScript.empty() && info.scriptName == ignoreScript && info.functionName == ignoreFunc) continue;
				USHORT activeVKey = ResolveDisplayVKey(info);
				bool hitsMember = false;
				for (USHORT k : comboKeys) if (k == activeVKey) { hitsMember = true; break; }
				if (hitsMember && IsSingleTriggerConflict(comboMask, info)) return true;
			}
			return false;
		}

		const KeyBind* FindMatchingBinding(USHORT vKey, bool ctrlHeld, bool shiftHeld, bool altHeld, bool preferHold) {

			for (const auto& bind : bindings) {
				if (bind.vKey != vKey) continue;
				if (bind.disabled || bind.autoDisabled) continue;
				bool categoryMatches = preferHold ? (bind.holdSeconds > 0.0f) : (bind.holdSeconds <= 0.0f);
				if (!categoryMatches) continue;

				if (bind.needCtrl == ctrlHeld && bind.needShift == shiftHeld && bind.needAlt == altHeld) {
					return &bind;
				}
			}
			return nullptr;
		}

		std::string LuaLongBracketWrap(const std::string& path) {
			std::string eq;
			while (path.find("]" + eq + "]") != std::string::npos) eq += "=";
			return "[" + eq + "[" + path + "]" + eq + "]";
		}

		void RunInjectCompileCheck(const std::string& injectContent) {
			LuaBridge::QueueMessageIn("InjectCompile|" + injectContent);
		}

		void FireBinding(const KeyBind& bind) {
			MarkActivityLogLive();
			if (bind.isInject) {
				if (bind.scriptDescribed && ModKeyBindings::IsDisabled(bind.injectScriptName, bind.injectFunctionName)) {
					return;
				}
				if (bind.scriptDescribed && ModKeyBindings::GetOverride(bind.injectScriptName, bind.injectFunctionName).empty() && !ModKeyBindings::HasTriggerConfig(bind.injectScriptName, bind.injectFunctionName)) {
					std::string keyNames;
					if (bind.IsCombo()) {
						for (size_t i = 0; i < bind.comboKeys.size(); ++i) {
							if (i > 0) keyNames += " + ";
							keyNames += NameForVKey(bind.comboKeys[i]);
						}
					} else {
						keyNames = bind.keyName.empty() ? NameForVKey(bind.vKey) : bind.keyName;
					}
					LogActivity(std::string(LOG_KEYBINDMENU_FUNCTION_NOT_TRIGGERED_NOT_REASSIGNED) + keyNames);
					return;
				}
				std::string injectContent = BuildInjectContent(bind.scriptPathOn, bind.injectLineStart, bind.injectLineEnd);
				if (injectContent.empty()) {
					LogActivity("KeyBindMenu: Script-line injection failed - source unreadable: " + FileNameOnly(bind.scriptPathOn), false);
					return;
				}
				LuaBridge::QueueMessageIn("InjectScript|" + injectContent);
				LogActivity("KeyBindMenu: Fired script lines " + std::to_string(bind.injectLineStart) + "-" + std::to_string(bind.injectLineEnd) + " of " + FileNameOnly(bind.scriptPathOn));
				return;
			}
			std::string targetPath = bind.scriptPathOn;
			std::string targetFunc = bind.functionTap;

			if (bind.isToggle) {
				targetPath = bind.toggleState ? bind.scriptPathOff : bind.scriptPathOn;
				targetFunc = bind.toggleState ? bind.functionOff : bind.functionOn;
				bind.toggleState = !bind.toggleState;
			}

			if (!DebuggerMenu::LogScriptAttempt(targetPath)) {
				LogActivity("KeyBindMenu: Script not found: " + FileNameOnly(targetPath), false);
				return;
			}

			if (targetFunc.empty()) {
				LuaBridge::QueueMessageIn("DoScript|dofile(" + LuaLongBracketWrap(targetPath) + ")");
				LogActivity("KeyBindMenu: Fired script " + FileNameOnly(targetPath));
				return;
			}

			switch (LuaCallGlobalFunction(targetFunc)) {
				case LuaDirectCallResult::Success:
					LogActivity("KeyBindMenu: Fired script " + FileNameOnly(targetPath) + " [" + targetFunc + "] (direct)");
					return;
				case LuaDirectCallResult::RuntimeError:
					LogActivity("KeyBindMenu: Script error firing " + FileNameOnly(targetPath) + " [" + targetFunc + "]", false);
					return;
				case LuaDirectCallResult::NotFound:
				case LuaDirectCallResult::NotAvailable:
					break;
			}

			std::string luaPayload = "CallFunction|" + targetFunc + "|" + targetPath;
			LuaBridge::QueueMessageIn(luaPayload);
			LogActivity("KeyBindMenu: Fired script " + FileNameOnly(targetPath) + " [" + targetFunc + "] (queued)");
		}

		USHORT ResolveDisplayVKey(const LuaKeyState::TrackedKeyInfo& info) {
			if (!info.hasDescription) {
				return info.vKey;
			}
			std::string overrideKeyName = ModKeyBindings::GetSlotOverride(info.scriptName, info.functionName, SlotOfVKey(info.vKey));
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
					if (bind.isInject && bind.injectScriptName == info.scriptName && bind.injectFunctionName == info.functionName) continue;
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

			static bool capturePromptWasActive = false;
			if (capturePromptWasActive && !showCapturePrompt) {
				padCaptureEdgePrimed = false;
				prevHeldPadKeysCapture.clear();
				psCaptureEdgePrimed = false;
				prevHeldPsKeysCapture.clear();
			}
			capturePromptWasActive = showCapturePrompt;

			static bool lastGamepadConnected = false;
			bool nowGamepadConnected = RawInput::IsAnyGamepadConnected();
			if (nowGamepadConnected != lastGamepadConnected) {
			LogActivity(nowGamepadConnected
				? (DirectInputHook::HasPlaystationDevice()
					? "PlayStation gamepad detected (DirectInput)"
					: (RawInput::HasXInputGamepad()
						? "Gamepad detected (XInput)"
						: "Gamepad detected (DirectInput)"))
				: "Gamepad no longer detected", true);
				lastGamepadConnected = nowGamepadConnected;
			}

			LuaKeyState::SweepStaleDescriptions();
			LuaKeyState::SweepStaleComboDescriptions();
			ModInfoRegistry::SweepStale();

			static std::vector<USHORT> cachedSuppressedVKeys;
			static std::vector<USHORT> cachedDisabledVKeys;
			static std::vector<std::vector<USHORT>> cachedDisabledCombos;
			static ULONGLONG lastVKeyComputeTick = 0;
			static bool lastVKeyComputeMenuOpen = false;
			static bool lastVKeyComputeCapturePrompt = false;
			const ULONGLONG vkeyComputeNow = GetTickCount64();
			if (menuOpen != lastVKeyComputeMenuOpen ||
			showCapturePrompt != lastVKeyComputeCapturePrompt ||
			vkeyComputeNow - lastVKeyComputeTick >= 250) {
			lastVKeyComputeTick = vkeyComputeNow;
			lastVKeyComputeMenuOpen = menuOpen;
			lastVKeyComputeCapturePrompt = showCapturePrompt;
			cachedSuppressedVKeys = ComputeConflictedVKeys();
			cachedDisabledVKeys = ComputeDisabledModVKeys();
			cachedDisabledCombos = ComputeDisabledModCombos();
			LuaKeyState::SetSuppressedVKeys(cachedSuppressedVKeys);
			LuaKeyState::SetDisabledVKeys(cachedDisabledVKeys);
			LuaKeyState::SetDisabledCombos(cachedDisabledCombos);
			}

			if (showCapturePrompt) {
				for (USHORT vKey : activeBindVKeys) {
					LuaKeyState::PhysicalOnButtonDown(vKey);
					LuaKeyState::PhysicalOnButtonUp(vKey);
				}
				return;
			}

			for (USHORT vKey : activeBindVKeys) {
				bool suppressedByCombo = IsVKeySuppressedByActiveCombo(vKey);
				if (suppressedByCombo) {
					auto latchIt = pendingPresses.find(vKey);
					if (latchIt != pendingPresses.end()) latchIt->second.comboActiveDuringPress = true;
				}

				if (LuaKeyState::PhysicalOnButtonDown(vKey)) {
					bool ctrlHeld = RawInput::IsKeyHeldReal(VK_CONTROL), shiftHeld = RawInput::IsKeyHeldReal(VK_SHIFT), altHeld = RawInput::IsKeyHeldReal(VK_MENU);
					DebuggerMenu::LogButtonPress(std::string(ctrlHeld ? "Ctrl+" : "") + (shiftHeld ? "Shift+" : "") + (altHeld ? "Alt+" : "") + NameForVKey(vKey) + " pressed");

					bool hasHoldOptionOnKey = false;
					for (const auto& bind : bindings) {
						if (bind.vKey == vKey && bind.holdSeconds > 0.0f && !bind.disabled && !bind.autoDisabled) {
						if (bind.needCtrl == ctrlHeld && bind.needShift == shiftHeld && bind.needAlt == altHeld) {
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
					LuaKeyState::PhysicalOnButtonDown(vKey);
					LuaKeyState::PhysicalOnButtonUp(vKey);
					continue;
				}
				PendingPress& pending = pendingIt->second;

				const KeyBind* holdBind = FindMatchingBinding(vKey, pending.ctrlOnPressed, pending.shiftOnPressed, pending.altOnPressed, true);
				const KeyBind* tapBind = FindMatchingBinding(vKey, pending.ctrlOnPressed, pending.shiftOnPressed, pending.altOnPressed, false);

				if (!suppressedByCombo && !pending.holdFired && holdBind && LuaKeyState::PhysicalOnButtonHoldTime(vKey, holdBind->holdSeconds)) {
					DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " held past threshold " + std::to_string(holdBind->holdSeconds) + "s");
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
				if (repeatBind && !suppressedByCombo && RawInput::IsKeyHeldReal(vKey)) {
					double sinceLastRepeat = std::chrono::duration<double>(std::chrono::steady_clock::now() - pending.lastRepeatTime).count();
					double effectiveInterval = kRepeatIntervalSeconds / pending.repeatSpeedMult;
					if (sinceLastRepeat >= effectiveInterval) {
						DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " repeat-fired");
						FireBinding(*repeatBind);
						pending.lastRepeatTime = std::chrono::steady_clock::now();
						pending.repeatSpeedMult = (std::clamp)(pending.repeatSpeedMult * (double)repeatBind->repeatAccelMult, kMinRepeatSpeedMult, kMaxRepeatSpeedMult);
					}
				}

				if (LuaKeyState::PhysicalOnButtonUp(vKey)) {
					if (!pending.holdFired && !pending.tapFired && !suppressedByCombo && !pending.comboActiveDuringPress) {
						if (holdBind && holdBind->isInstant && holdBind->instantTriggerType != 2) {
							double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - pending.pressTime).count();
							if (heldSeconds < kNearMissHoldFraction * holdBind->holdSeconds) {
								DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " released early (Instant)");
								FireBinding(*holdBind);
							} else {
								LogActivity(NameForVKey(vKey) + " released near hold threshold - Instant suppressed");
							}
						}
						else if (tapBind && tapBind->holdSeconds <= 0.0f && !(tapBind->isInstant && tapBind->instantTriggerType == 2)) {
							std::string reason = (tapBind->isInstant && tapBind->instantTriggerType == 1) ? "released (On Release)" : "tapped cleanly (Hold bypassed)";
							DebuggerMenu::LogButtonPress(NameForVKey(vKey) + " " + reason);
							FireBinding(*tapBind);
						}
					}
					pendingPresses.erase(pendingIt);
				}
			}

			for (KeyBind& bind : bindings) {
				if (!bind.IsCombo()) continue;
				if (bind.disabled) continue;

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
							FireBinding(bind);
							bind.comboHoldFired = true;
							bind.comboLastRepeatTime = std::chrono::steady_clock::now();
						}
					}

					bool repeatEligible = allHeld && bind.isInstant && bind.instantTriggerType == 2 && (hasHold ? bind.comboHoldFired : true);
					if (repeatEligible) {
						double sinceLastRepeat = std::chrono::duration<double>(std::chrono::steady_clock::now() - bind.comboLastRepeatTime).count();
						double effectiveInterval = kRepeatIntervalSeconds / bind.runtimeRepeatSpeedMult;
						if (sinceLastRepeat >= effectiveInterval) {
							DebuggerMenu::LogButtonPress(CombinedDisplayName(bind) + " repeat-fired");
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
								FireBinding(bind);
							} else {
								LogActivity(CombinedDisplayName(bind) + " released near hold threshold - Instant suppressed");
							}
						}
						else if (!hasHold) {
							DebuggerMenu::LogButtonPress(CombinedDisplayName(bind) + " released (On Release)");
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
			LuaKeyState::PhysicalOnButtonDown(vKey);
			LuaKeyState::PhysicalOnButtonUp(vKey);
			LuaKeyState::RetireIfUndescribed(vKey);
		}

		static SafeQueue<std::string> pendingInjectDescribes;
		static std::map<std::string, std::chrono::steady_clock::time_point> injectDescribeTouch;
		constexpr double kInjectDescribeStaleSeconds = 5.0;

		void ApplyInjectDescribe(const std::string& payload) {
			std::vector<std::string> fields = split(payload, "\x1f");
			if (fields.size() < 6) {
				spdlog::warn(LOG_KEYBINDMENU_INJECTDESCRIBE_MALFORMED_FMT, (int)fields.size());
				return;
			}
			std::string keyName = fields[0];
			int vKey = VKeyForName(keyName);
			std::vector<USHORT> comboMembers;
			if (vKey == -1) {
				comboMembers = ParseComboKeyNames(keyName);
				if (comboMembers.empty()) {
					static std::unordered_set<std::string> droppedDescribeNamesLogged;
					if (droppedDescribeNamesLogged.insert(keyName).second) {
						spdlog::warn(LOG_KEYBINDMENU_INJECTDESCRIBE_UNKNOWN_KEY_FMT, keyName);
					}
					return;
				}
			}
			int lineStart = 0;
			int lineEnd = 0;
			std::stringstream ssStart(fields[4]);
			ssStart >> lineStart;
			std::stringstream ssEnd(fields[5]);
			ssEnd >> lineEnd;
			if (lineStart < 1 || lineEnd < lineStart) {
				spdlog::warn(LOG_KEYBINDMENU_INJECTDESCRIBE_BAD_RANGE_FMT, lineStart, lineEnd);
				return;
			}
			std::string scriptName = fields[1];
			std::string functionName = fields[2];
			std::string sourcePath = ResolveScriptPath(fields[3]);
			USHORT referenceVKey = comboMembers.empty() ? (USHORT)vKey : comboMembers.front();
			std::string slotTag = (SlotOfVKey(referenceVKey) == ModKeyBindings::BindSlot::Pad) ? "pad" : "kbm";
			std::string identity = scriptName + "\x1f" + functionName + "\x1f" + slotTag;
			bool exists = false;
			if (!comboMembers.empty()) {
				std::vector<USHORT> canonicalMembers = CanonicalizeComboKeys(comboMembers);
				bool comboFormed = false;
				for (size_t bi = 0; bi < bindings.size(); ) {
					KeyBind& bind = bindings[bi];
					std::string bindSlot = (SlotOfVKey(bind.vKey) == ModKeyBindings::BindSlot::Pad) ? "pad" : "kbm";
					if (!bind.isInject || !bind.scriptDescribed || bind.injectScriptName != scriptName || bind.injectFunctionName != functionName || bindSlot != slotTag) {
						bi++;
						continue;
					}
					if (!comboFormed) {
						if (bind.IsCombo() && VectorsEqualUnordered(bind.comboKeys, canonicalMembers)
							&& bind.scriptPathOn == sourcePath && bind.injectLineStart == lineStart && bind.injectLineEnd == lineEnd) {
							exists = true;
							break;
						}
						bind.comboKeys = canonicalMembers;
						bind.vKey = canonicalMembers.front();
						bind.keyName = keyName;
						bind.scriptPathOn = sourcePath;
						bind.injectLineStart = lineStart;
						bind.injectLineEnd = lineEnd;
						ModKeyBindings::TriggerConfig storedTrigger = ModKeyBindings::GetTriggerConfig(scriptName, functionName);
						if (storedTrigger.triggerType == 2) {
							bind.isInstant = true;
							bind.instantTriggerType = 2;
							bind.holdSeconds = 0.0f;
							bind.repeatAccelMult = storedTrigger.repeatAccelMult;
						} else {
							bind.isInstant = (storedTrigger.triggerType == 1);
							bind.instantTriggerType = storedTrigger.triggerType;
							bind.holdSeconds = (storedTrigger.triggerType == 0) ? storedTrigger.holdSeconds : 0.0f;
							bind.repeatAccelMult = 1.0f;
						}
						bind.autoDisabled = BuildInjectContent(sourcePath, lineStart, lineEnd).empty();
						MarkDisplayCacheDirty();
						spdlog::info(LOG_KEYBINDMENU_INJECTDESCRIBE_UPDATED_FMT, keyName, lineStart, lineEnd, FileNameOnly(sourcePath));
						comboFormed = true;
						exists = true;
					} else {
						bindings.erase(bindings.begin() + bi);
						continue;
					}
					bi++;
				}
				if (!exists) {
					std::string injectContent = BuildInjectContent(sourcePath, lineStart, lineEnd);
					if (injectContent.empty()) {
						spdlog::warn(LOG_KEYBINDMENU_INJECTDESCRIBE_SOURCE_UNREADABLE_FMT, FileNameOnly(sourcePath));
					} else {
						RunInjectCompileCheck(injectContent);
					}
					KeyBind newBind{};
					newBind.vKey = canonicalMembers.front();
					newBind.keyName = keyName;
					newBind.comboKeys = canonicalMembers;
					newBind.scriptPathOn = sourcePath;
					newBind.isInject = true;
					newBind.scriptDescribed = true;
					newBind.autoDisabled = injectContent.empty();
					newBind.injectScriptName = scriptName;
					newBind.injectFunctionName = functionName;
					newBind.injectLineStart = lineStart;
					newBind.injectLineEnd = lineEnd;
					ModKeyBindings::TriggerConfig storedTrigger = ModKeyBindings::GetTriggerConfig(scriptName, functionName);
					if (storedTrigger.triggerType == 2) {
						newBind.isInstant = true;
						newBind.instantTriggerType = 2;
						newBind.holdSeconds = 0.0f;
						newBind.repeatAccelMult = storedTrigger.repeatAccelMult;
					} else {
						newBind.isInstant = (storedTrigger.triggerType == 1);
						newBind.instantTriggerType = storedTrigger.triggerType;
						newBind.holdSeconds = (storedTrigger.triggerType == 0) ? storedTrigger.holdSeconds : 0.0f;
						newBind.repeatAccelMult = 1.0f;
					}
					bool overrideActiveAtCreate = !ModKeyBindings::GetOverride(scriptName, functionName).empty();
					newBind.nativeVKey = overrideActiveAtCreate ? (USHORT)0 : (USHORT)canonicalMembers.front();
					if (!overrideActiveAtCreate && ModKeyBindings::GetNativeKey(scriptName, functionName).empty()) {
						ModKeyBindings::SetNativeKeyWithoutSave(scriptName, functionName, keyName);
						SaveBindings();
					}
					bindings.push_back(newBind);
					EnsureDispatcherRegistered(newBind.vKey);
					MarkDisplayCacheDirty();
					LogActivity("KeyBindMenu: Bound " + keyName + " to script lines " + std::to_string(lineStart) + "-" + std::to_string(lineEnd) + " of " + FileNameOnly(sourcePath));
				}
				injectDescribeTouch[identity] = std::chrono::steady_clock::now();
				MarkActivityLogLive();
				return;
			}
			for (auto& bind : bindings) {
				if (!bind.isInject || !bind.scriptDescribed) continue;
				std::string bindSlot = (SlotOfVKey(bind.vKey) == ModKeyBindings::BindSlot::Pad) ? "pad" : "kbm";
				if (bind.injectScriptName != scriptName || bind.injectFunctionName != functionName || bindSlot != slotTag) continue;
				exists = true;
				if (bind.vKey == (USHORT)vKey && bind.scriptPathOn == sourcePath && bind.injectLineStart == lineStart && bind.injectLineEnd == lineEnd) {
					break;
				}
				if (bind.vKey != (USHORT)vKey) {
					RemoveDispatcherIfUnused(bind.vKey);
					bind.vKey = (USHORT)vKey;
					bind.keyName = keyName;
					EnsureDispatcherRegistered(bind.vKey);
				}
				bind.scriptPathOn = sourcePath;
				bind.injectLineStart = lineStart;
				bind.injectLineEnd = lineEnd;
				if (bind.nativeVKey == 0 && !ModKeyBindings::GetOverride(scriptName, functionName).empty() == false) bind.nativeVKey = bind.vKey;
				bind.autoDisabled = BuildInjectContent(sourcePath, lineStart, lineEnd).empty();
				ModKeyBindings::TriggerConfig storedTrigger = ModKeyBindings::GetTriggerConfig(scriptName, functionName);
				if (storedTrigger.triggerType == 2) {
					bind.isInstant = true;
					bind.instantTriggerType = 2;
					bind.holdSeconds = 0.0f;
					bind.repeatAccelMult = storedTrigger.repeatAccelMult;
				} else {
					bind.isInstant = (storedTrigger.triggerType == 1);
					bind.instantTriggerType = storedTrigger.triggerType;
					bind.holdSeconds = (storedTrigger.triggerType == 0) ? storedTrigger.holdSeconds : 0.0f;
					bind.repeatAccelMult = 1.0f;
				}
				MarkDisplayCacheDirty();
				spdlog::info(LOG_KEYBINDMENU_INJECTDESCRIBE_UPDATED_FMT, keyName, lineStart, lineEnd, FileNameOnly(sourcePath));
				break;
			}
			if (!exists) {
				bool armedAutoDisabled = false;
				std::string injectContent = BuildInjectContent(sourcePath, lineStart, lineEnd);
				if (injectContent.empty()) {
					spdlog::warn(LOG_KEYBINDMENU_INJECTDESCRIBE_SOURCE_UNREADABLE_FMT, FileNameOnly(sourcePath));
				} else {
					RunInjectCompileCheck(injectContent);
				}
				armedAutoDisabled = injectContent.empty();
				KeyBind newBind{};
				newBind.vKey = (USHORT)vKey;
				newBind.keyName = keyName;
				newBind.scriptPathOn = sourcePath;
				newBind.isInject = true;
				newBind.scriptDescribed = true;
				newBind.autoDisabled = armedAutoDisabled;
				newBind.injectScriptName = scriptName;
				newBind.injectFunctionName = functionName;
				newBind.injectLineStart = lineStart;
				newBind.injectLineEnd = lineEnd;
				ModKeyBindings::TriggerConfig storedTrigger = ModKeyBindings::GetTriggerConfig(scriptName, functionName);
				if (storedTrigger.triggerType == 2) {
					newBind.isInstant = true;
					newBind.instantTriggerType = 2;
					newBind.holdSeconds = 0.0f;
					newBind.repeatAccelMult = storedTrigger.repeatAccelMult;
				} else {
					newBind.isInstant = (storedTrigger.triggerType == 1);
					newBind.instantTriggerType = storedTrigger.triggerType;
					newBind.holdSeconds = (storedTrigger.triggerType == 0) ? storedTrigger.holdSeconds : 0.0f;
					newBind.repeatAccelMult = 1.0f;
				}
				bool overrideActiveAtCreate = !ModKeyBindings::GetOverride(scriptName, functionName).empty();
				newBind.nativeVKey = overrideActiveAtCreate ? (USHORT)0 : (USHORT)vKey;
				if (!overrideActiveAtCreate && ModKeyBindings::GetNativeKey(scriptName, functionName).empty()) {
					ModKeyBindings::SetNativeKeyWithoutSave(scriptName, functionName, keyName);
					SaveBindings();
				}
				bindings.push_back(newBind);
				EnsureDispatcherRegistered(newBind.vKey);
				MarkDisplayCacheDirty();
				LogActivity("KeyBindMenu: Bound " + keyName + " to script lines " + std::to_string(lineStart) + "-" + std::to_string(lineEnd) + " of " + FileNameOnly(sourcePath));
			}
			injectDescribeTouch[identity] = std::chrono::steady_clock::now();
			MarkActivityLogLive();
		}

		void SweepStaleScriptInjects() {
			const auto now = std::chrono::steady_clock::now();
			bool removedAny = false;
			for (int i = (int)bindings.size() - 1; i >= 0; i--) {
				if (!bindings[i].isInject || !bindings[i].scriptDescribed) continue;
				std::string bindSlot = (SlotOfVKey(bindings[i].vKey) == ModKeyBindings::BindSlot::Pad) ? "pad" : "kbm";
				auto it = injectDescribeTouch.find(bindings[i].injectScriptName + "\x1f" + bindings[i].injectFunctionName + "\x1f" + bindSlot);
				if (it == injectDescribeTouch.end() || std::chrono::duration<double>(now - it->second).count() > kInjectDescribeStaleSeconds) {
					RemoveDispatcherIfUnused(bindings[i].vKey);
					bindings.erase(bindings.begin() + i);
					removedAny = true;
				}
			}
			if (removedAny) {
				MarkDisplayCacheDirty();
			}
		}

		const KeyBind* FindAutoDisabledDescribedInject(const std::string& scriptName, const std::string& functionName) {
			for (const auto& bind : bindings) {
				if (!bind.isInject || !bind.scriptDescribed || !bind.autoDisabled) continue;
				if (bind.injectScriptName == scriptName && bind.injectFunctionName == functionName) return &bind;
			}
			return nullptr;
		}

		void ProcessInjectDescribes() {
			static auto lastHealthSweep = std::chrono::steady_clock::now() - std::chrono::seconds(2);
			auto healthNow = std::chrono::steady_clock::now();
			if (std::chrono::duration<double>(healthNow - lastHealthSweep).count() >= 1.0) {
				lastHealthSweep = healthNow;
				for (auto& bind : bindings) {
					if (!bind.isInject) continue;
					std::error_code healthEc;
					bool sourceReadable = std::filesystem::exists(bind.scriptPathOn, healthEc);
					if (!sourceReadable && !bind.autoDisabled) {
						bind.autoDisabled = true;
						spdlog::warn(LOG_KEYBINDMENU_INJECT_SOURCE_LOST_AUTO_DISABLED_FMT, FileNameOnly(bind.scriptPathOn));
					} else if (sourceReadable && bind.autoDisabled) {
						bind.autoDisabled = false;
						spdlog::info(LOG_KEYBINDMENU_INJECT_SOURCE_RESTORED_AUTO_ENABLED_FMT, FileNameOnly(bind.scriptPathOn));
					}
				}
			}
			std::optional<std::string> payloadOpt = pendingInjectDescribes.pop();
			while (payloadOpt) {
				ApplyInjectDescribe(*payloadOpt);
				payloadOpt = pendingInjectDescribes.pop();
			}
			SweepStaleScriptInjects();
		}

		void QueueInjectDescribe(const std::string& payload) {
			pendingInjectDescribes.push(payload);
		}

		void SaveBindings() {
			EnsureBindsDirectory();
			std::string bindsPath = GetBindsFileName();
			std::string tmpPath = bindsPath + ".tmp";
			std::string bakPath = bindsPath + ".bak";
			std::ofstream outFile(tmpPath);
			if (!outFile) {
				LogActivity("KeyBindMenu: Save bindings failed: couldn't open " + FileNameOnly(tmpPath) + " for writing", false);
				return;
			}
			
			outFile << "MENUKEY|" << NameForVKey(menuToggleVKey) << "\n";
			outFile << "CAPTUREBLOCK|"
				<< (captureSuppressKeyboard ? "1|" : "0|")
				<< (captureSuppressMouse ? "1|" : "0|")
				<< (captureSuppressGamepad ? "1" : "0") << "\n";
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

				if (b.isInject) {
					if (b.scriptDescribed) continue;
					outFile << "INJECT|" << b.keyName << "|" << (b.needCtrl ? "1|" : "0|") << (b.needShift ? "1|" : "0|") << (b.needAlt ? "1|" : "0|") << b.holdSeconds << "|" << (b.isInstant ? "1|" : "0|") << b.instantTriggerType << "|" << b.repeatAccelMult << "|" << (b.disabled ? "1|" : "0|") << b.injectLineStart << "|" << b.injectLineEnd << "|" << genericOn << "\n";
					continue;
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
				if (entry.keyName == entry.padKeyName && entry.triggerType == 0 && entry.holdSeconds == 0.0f && entry.repeatAccelMult == 1.0f) {
					outFile << "MODKEY|" << entry.scriptName << "|" << entry.functionName << "|" << entry.keyName << "|" << (entry.disabled ? "1" : "0") << "\n";
				} else {
					outFile << "MODKEY2|" << entry.scriptName << "|" << entry.functionName << "|" << entry.keyName << "|" << entry.padKeyName << "|" << (entry.disabled ? "1" : "0") << "|" << entry.triggerType << "|" << entry.holdSeconds << "|" << entry.repeatAccelMult << "|" << entry.nativeKeyName << "\n";
				}
			}
			outFile.close();
			if (!outFile) {
				LogActivity("KeyBindMenu: Save bindings failed while writing " + FileNameOnly(tmpPath), false);
				return;
			}
			{
				std::error_code ec;
				if (std::filesystem::exists(bindsPath, ec)) {
					std::filesystem::copy_file(bindsPath, bakPath, std::filesystem::copy_options::overwrite_existing, ec);
					ec.clear();
				}
				std::filesystem::rename(tmpPath, bindsPath, ec);
				if (ec) {
					LogActivity("KeyBindMenu: Save bindings failed: couldn't replace " + FileNameOnly(bindsPath) + " (" + ec.message() + ")", false);
					return;
				}
			}
			LogActivity("KeyBindMenu: Saved " + std::to_string(bindings.size()) + " binding(s) to " + FileNameOnly(bindsPath));
		}

		void LoadBindings() {
			std::ifstream inFile(GetBindsFileName());
			if (!inFile) {
				LogActivity("KeyBindMenu: No existing bindings file yet at " + GetBindsFileName() + " (fine on first run)");
				ModKeyBindings::LoadFromEntries({});
				return;
			}

			std::vector<ModKeyBindings::OverrideEntry> modKeyEntries;
			std::string line;
			while (std::getline(inFile, line)) {
				if ((line = trim(line)).empty()) continue;
				std::vector<std::string> parts = split(line, "|");
				if (parts.size() < 2) {
					LogActivity("KeyBindMenu: Skipped malformed line while loading bindings: " + line, false);
					continue;
				}

				if (parts[0] == "MENUKEY") {
					int vKey = VKeyForName(trim(parts[1]));
					if (vKey != -1) menuToggleVKey = (USHORT)vKey;
					else {
						LogActivity("KeyBindMenu: Unknown MENUKEY name '" + parts[1] + "', keeping default", false);
					}
				}
				else if (parts[0] == "CAPTUREBLOCK" && parts.size() >= 3) {
				captureSuppressKeyboard = trim(parts[1]) == "1";
				captureSuppressMouse = trim(parts[2]) == "1";
				if (parts.size() >= 4) {
					captureSuppressGamepad = trim(parts[3]) == "1";
				}
				}
				else if (parts[0] == "MODKEY" && parts.size() >= 4) {
					ModKeyBindings::OverrideEntry entry;
					entry.scriptName = trim(parts[1]);
					entry.functionName = trim(parts[2]);
					entry.keyName = trim(parts[3]);
					entry.padKeyName = entry.keyName;
					entry.disabled = parts.size() >= 5 && trim(parts[4]) == "1";
					if (!entry.scriptName.empty() && !entry.functionName.empty() && (!entry.keyName.empty() || entry.disabled)) {
						modKeyEntries.push_back(std::move(entry));
					}
				}
				else if (parts[0] == "MODKEY2" && parts.size() >= 6) {
					ModKeyBindings::OverrideEntry entry;
					entry.scriptName = trim(parts[1]);
					entry.functionName = trim(parts[2]);
					entry.keyName = trim(parts[3]);
					entry.padKeyName = trim(parts[4]);
					entry.disabled = trim(parts[5]) == "1";
					if (parts.size() >= 7) {
						try { entry.triggerType = std::stoi(trim(parts[6])); }
						catch (...) { entry.triggerType = 0; }
						if (entry.triggerType < 0 || entry.triggerType > 2) entry.triggerType = 0;
					}
					if (parts.size() >= 8) {
						try { entry.holdSeconds = std::stof(trim(parts[7])); }
						catch (...) { entry.holdSeconds = 0.0f; }
						if (!std::isfinite(entry.holdSeconds) || entry.holdSeconds < 0.0f) entry.holdSeconds = 0.0f;
					}
					if (parts.size() >= 9) {
						try { entry.repeatAccelMult = std::stof(trim(parts[8])); }
						catch (...) { entry.repeatAccelMult = 1.0f; }
						if (!std::isfinite(entry.repeatAccelMult) || entry.repeatAccelMult < 0.1f) entry.repeatAccelMult = 0.1f;
						else if (entry.repeatAccelMult > 20.0f) entry.repeatAccelMult = 20.0f;
					}
					if (parts.size() >= 10) {
						entry.nativeKeyName = trim(parts[9]);
					}
					if (!entry.scriptName.empty() && !entry.functionName.empty() && (!entry.keyName.empty() || !entry.padKeyName.empty() || entry.disabled || entry.triggerType != 0 || entry.holdSeconds > 0.0f)) {
						modKeyEntries.push_back(std::move(entry));
					}
				}
				else if (parts[0] == "INJECT" && parts.size() >= 13) {
					std::string injectKeyName = trim(parts[1]);
					std::vector<USHORT> injectComboMembers;
					int injectVKey = VKeyForName(injectKeyName);
					if (injectVKey == -1) {
						injectComboMembers = ParseComboKeyNames(injectKeyName);
						if (injectComboMembers.empty()) {
							LogActivity("KeyBindMenu: Unknown Key name '" + injectKeyName + "', skipping script-line binding", false);
							continue;
						}
						injectVKey = (int)injectComboMembers.front();
					}
					float injectHoldSeconds = 0.0f;
					try { injectHoldSeconds = std::stof(trim(parts[5])); }
					catch (...) { injectHoldSeconds = 0.0f; }
					if (!std::isfinite(injectHoldSeconds) || injectHoldSeconds < 0.0f) injectHoldSeconds = 0.0f;
					bool injectIsInstant = trim(parts[6]) == "1";
					int injectInstantType = 0;
					try { injectInstantType = std::stoi(trim(parts[7])); }
					catch (...) { injectInstantType = 0; }
					if (injectInstantType < 0 || injectInstantType > 2) injectInstantType = 0;
					float injectRepeatMult = 1.0f;
					try { injectRepeatMult = std::stof(trim(parts[8])); }
					catch (...) { injectRepeatMult = 1.0f; }
					if (!std::isfinite(injectRepeatMult) || injectRepeatMult < kMinRepeatAccelMult) injectRepeatMult = kMinRepeatAccelMult;
					else if (injectRepeatMult > (float)kMaxRepeatSpeedMult) injectRepeatMult = (float)kMaxRepeatSpeedMult;
					bool injectDisabled = trim(parts[9]) == "1";
					int injectLineStart = 0;
					int injectLineEnd = 0;
					try { injectLineStart = std::stoi(trim(parts[10])); }
					catch (...) { injectLineStart = 0; }
					try { injectLineEnd = std::stoi(trim(parts[11])); }
					catch (...) { injectLineEnd = 0; }
					if (injectLineStart < 1 || injectLineEnd < injectLineStart) {
						LogActivity("KeyBindMenu: Skipped malformed INJECT line while loading bindings: " + line, false);
						continue;
					}
					KeyBind newInjectBind{};
					newInjectBind.vKey = (USHORT)injectVKey;
					newInjectBind.needCtrl = trim(parts[2]) == "1";
					newInjectBind.needShift = trim(parts[3]) == "1";
					newInjectBind.needAlt = trim(parts[4]) == "1";
					newInjectBind.keyName = injectKeyName;
					if (injectComboMembers.size() >= 2) {
						newInjectBind.comboKeys = CanonicalizeComboKeys(injectComboMembers);
					}
					newInjectBind.holdSeconds = injectHoldSeconds;
					newInjectBind.isInstant = injectIsInstant;
					newInjectBind.instantTriggerType = injectInstantType;
					newInjectBind.repeatAccelMult = injectRepeatMult;
					newInjectBind.disabled = injectDisabled;
					newInjectBind.scriptPathOn = ResolveScriptPath(trim(parts[12]));
					newInjectBind.isInject = true;
					newInjectBind.injectLineStart = injectLineStart;
					newInjectBind.injectLineEnd = injectLineEnd;
					bindings.push_back(newInjectBind);
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
						LogActivity("KeyBindMenu: Skipped invalid COMBO2 line while loading bindings: " + line, false);
						continue;
					}

					float holdSeconds = 0.0f;
					try { holdSeconds = std::stof(trim(parts[2])); }
					catch (...) { holdSeconds = 0.0f; }
					if (!std::isfinite(holdSeconds) || holdSeconds < 0.0f) holdSeconds = 0.0f;

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
						LogActivity("KeyBindMenu: Skipped incomplete COMBO2 line while loading bindings: " + line, false);
						continue;
					}

					bool isInstant = false;
					int instantTriggerType = 0;
					float repeatAccelMult = 1.0f;
					if (parts.size() >= instantFieldStart + 2) {
						isInstant = trim(parts[instantFieldStart]) == "1";
						try { instantTriggerType = std::stoi(trim(parts[instantFieldStart + 1])); }
						catch (...) { instantTriggerType = 0; }
						if (instantTriggerType < 0 || instantTriggerType > 2) instantTriggerType = 0;
					}
					if (parts.size() >= instantFieldStart + 3) {
						try { repeatAccelMult = std::stof(trim(parts[instantFieldStart + 2])); }
						catch (...) { repeatAccelMult = 1.0f; }
						if (!std::isfinite(repeatAccelMult) || repeatAccelMult < kMinRepeatAccelMult) repeatAccelMult = kMinRepeatAccelMult;
						else if (repeatAccelMult > (float)kMaxRepeatSpeedMult) repeatAccelMult = (float)kMaxRepeatSpeedMult;
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
						LogActivity("KeyBindMenu: Skipped invalid COMBO line while loading bindings: " + line, false);
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
						spdlog::warn(LOG_KEYBINDMENU_LOADBINDINGS_SKIPPING_INCOMPLETE_BIND_LI, line);
						continue;
					}
					std::string keyName = trim(parts[1]); int vKey = VKeyForName(keyName);
					if (vKey == -1) {
						LogActivity("KeyBindMenu: Unknown Key name '" + keyName + "', skipping binding", false);
						continue;
					}
					
					float holdSeconds = 0.0f;
					try { holdSeconds = std::stof(trim(parts[5])); }
					catch (...) { holdSeconds = 0.0f; }
					if (!std::isfinite(holdSeconds) || holdSeconds < 0.0f) holdSeconds = 0.0f;
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
						if (instantTriggerType < 0 || instantTriggerType > 2) instantTriggerType = 0;
					}
					if (parts.size() >= instantFieldStart + 3) {
						try { repeatAccelMult = std::stof(trim(parts[instantFieldStart + 2])); }
						catch (...) { repeatAccelMult = 1.0f; }
						if (!std::isfinite(repeatAccelMult) || repeatAccelMult < kMinRepeatAccelMult) repeatAccelMult = kMinRepeatAccelMult;
						else if (repeatAccelMult > (float)kMaxRepeatSpeedMult) repeatAccelMult = (float)kMaxRepeatSpeedMult;
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
					LogActivity("KeyBindMenu: Skipped old-format/malformed BIND line: " + line, false);
				}
			}
			ModKeyBindings::LoadFromEntries(modKeyEntries);
			LogActivity("KeyBindMenu: Loaded " + std::to_string(bindings.size()) + " binding(s) from " + FileNameOnly(GetBindsFileName()));
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
			LogActivity("KeyBindMenu: Bound " + CombinedDisplayName(bindings.back()) + " (toggle: " + (isToggle ? "YES" : "NO") + ")");
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
			LogActivity("KeyBindMenu: Bound combo " + CombinedDisplayName(bindings.back()));
		}

		void RemoveBinding(int index) {
			if (index < 0 || index >= (int)bindings.size()) {
				LogActivity("KeyBindMenu: Attempted to remove binding at invalid index " + std::to_string(index), false);
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
			LogActivity("KeyBindMenu: Unbound " + removedDesc);
		}

		void RemoveAllBindings() {
			size_t count = bindings.size();
			for (const KeyBind& bind : bindings) {
				if (!bind.IsCombo()) LuaKeyState::RetireIfUndescribed(bind.vKey);
			}
			for (USHORT drainVKey : activeBindVKeys) {
				LuaKeyState::PhysicalOnButtonDown(drainVKey);
				LuaKeyState::PhysicalOnButtonUp(drainVKey);
			}
			activeBindVKeys.clear();
			pendingPresses.clear();
			bindings.clear();
			SaveBindings();
			MarkDisplayCacheDirty();
			DebuggerMenu::LogBindEvent("unbound all (" + std::to_string(count) + " binding(s))");
			LogActivity("KeyBindMenu: Cleared all bindings (" + std::to_string(count) + " binding(s))");
		}

		void DisableAllBindingsAndModKeys() {
			size_t manualCount = 0;
			for (KeyBind& bind : bindings) {
				if (!bind.disabled) {
					bind.disabled = true;
					manualCount++;
				}
			}
			if (manualCount > 0) SaveBindings();

			size_t modKeyCount = 0;
			for (const auto& info : LuaKeyState::GetTrackedKeyInfo()) {
				if (!info.hasDescription) continue;
				if (ModKeyBindings::IsDisabled(info.scriptName, info.functionName)) continue;
				ModKeyBindings::SetDisabledWithoutSave(info.scriptName, info.functionName, true);
				modKeyCount++;
			}
			for (const auto& cinfo : LuaKeyState::GetTrackedComboKeyInfo()) {
				if (ModKeyBindings::IsDisabled(cinfo.scriptName, cinfo.functionName)) continue;
				ModKeyBindings::SetDisabledWithoutSave(cinfo.scriptName, cinfo.functionName, true);
				modKeyCount++;
			}

			if (modKeyCount > 0) SaveBindings();
			MarkDisplayCacheDirty();
			LogActivity("KeyBindMenu: Disabled all hotkeys (" + std::to_string(manualCount) + " manual, " + std::to_string(modKeyCount) + " mod key(s))");
		}

		void ResetAndRemoveAllBindingsAndModKeys() {
			size_t resetCount = 0;
			for (const auto& info : LuaKeyState::GetTrackedKeyInfo()) {
				if (!info.hasDescription) continue;
				bool hadOverride = !ModKeyBindings::GetOverride(info.scriptName, info.functionName).empty();
				bool hadTriggerConfig = ModKeyBindings::HasTriggerConfig(info.scriptName, info.functionName);
				if (!hadOverride && !hadTriggerConfig) continue;
				std::string storedNativeName = ModKeyBindings::GetNativeKey(info.scriptName, info.functionName);
				int storedNativeResolved = storedNativeName.empty() ? -1 : ResolveKeyNameForSlot(storedNativeName, info.vKey);
				USHORT memberNative = 0;
				for (auto& b : bindings) {
					if (!b.isInject || !b.scriptDescribed || b.injectScriptName != info.scriptName || b.injectFunctionName != info.functionName) continue;
					USHORT beforeRestore = b.vKey;
					USHORT bindNative = b.nativeVKey;
					if (bindNative == 0 && storedNativeResolved > 0 && SlotOfVKey((USHORT)storedNativeResolved) == SlotOfVKey(b.vKey)) bindNative = (USHORT)storedNativeResolved;
					if (bindNative != 0 && bindNative != b.vKey) {
						RemoveDispatcherIfUnused(b.vKey);
						b.vKey = bindNative;
						b.keyName = NameForVKey(bindNative);
						EnsureDispatcherRegistered(b.vKey);
					}
					if (beforeRestore == info.vKey && bindNative != 0) memberNative = bindNative;
					b.isInstant = false;
					b.instantTriggerType = 0;
					b.holdSeconds = 0.0f;
					b.repeatAccelMult = 1.0f;
				}
				ModKeyBindings::SetTriggerConfigWithoutSave(info.scriptName, info.functionName, 0, 0.0f, 1.0f);
				ModKeyBindings::SetOverrideWithoutSave(info.scriptName, info.functionName, "");
				USHORT bulkNativeVKey = memberNative;
				if (bulkNativeVKey == 0 && storedNativeResolved > 0 && SlotOfVKey((USHORT)storedNativeResolved) == SlotOfVKey(info.vKey)) bulkNativeVKey = (USHORT)storedNativeResolved;
				if (bulkNativeVKey == 0) bulkNativeVKey = LuaKeyState::FindRedirectSource(info.vKey);
				if (bulkNativeVKey != 0 && bulkNativeVKey != info.vKey) {
					LuaKeyState::ReassignBinding(info.vKey, bulkNativeVKey, info.scriptName, info.functionName);
				}
				ModKeyBindings::SetDisabledWithoutSave(info.scriptName, info.functionName, false);
				resetCount++;
			}
			for (const auto& cinfo : LuaKeyState::GetTrackedComboKeyInfo()) {
				if (ModKeyBindings::GetOverride(cinfo.scriptName, cinfo.functionName).empty()) continue;
				ModKeyBindings::SetOverrideWithoutSave(cinfo.scriptName, cinfo.functionName, "");
				LuaKeyState::ClearComboRedirect(cinfo.nativeKeys);
				ModKeyBindings::SetDisabledWithoutSave(cinfo.scriptName, cinfo.functionName, false);
				resetCount++;
			}

			size_t removedCount = bindings.size();
			RemoveAllBindings();

			LogActivity("KeyBindMenu: Reset " + std::to_string(resetCount) + " mod key override(s) to default and removed " + std::to_string(removedCount) + " manual binding(s)");
		}

		void Init(const std::string& defaultMenuKeyName) {
			LogActivity(LOG_RADARKEYS_KEYBINDMENU_INITIALIZING);
			int defaultVKey = VKeyForName(defaultMenuKeyName);
			if (defaultVKey != -1) menuToggleVKey = (USHORT)defaultVKey;
			else if (!defaultMenuKeyName.empty()) {
				LogActivity("KeyBindMenu: Unknown menu hotkey '" + defaultMenuKeyName + "', using default F7", false);
			}

			LoadBindings();
			for (const auto& bind : bindings) {
				if (!bind.IsCombo()) EnsureDispatcherRegistered(bind.vKey);
			}
			RegisterMenuToggleKey(menuToggleVKey);
			LogActivity("KeyBindMenu: Menu hotkey set to " + NameForVKey(menuToggleVKey));
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
		static bool captureIsInject = false;
		static int capturedInjectLineStart = 1;
		static int capturedInjectLineEnd = 1;
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

		void CancelCaptureIfActive() {
			if (!showCapturePrompt) {
				return;
			}

			capturedVKey = 0; capturedHoldSeconds = 0.0f;
			capturedScriptPathOnBuffer[0] = capturedScriptPathOffBuffer[0] = '\0';
			capturedFuncOnBuffer[0] = capturedFuncOffBuffer[0] = capturedFuncTapBuffer[0] = '\0';
			capturedToggleMode = capturedLongPressMode = capturedHasFuncOn = capturedHasFuncOff = false;
			capturedInstantMode = false; capturedInstantTriggerType = 0; capturedRepeatAccelMult = 1.0f;
			capturedInstantUserSet = false;
			ResetComboCaptureState();
			captureIsCombo = false;
			captureIsInject = false;
			capturedInjectLineStart = 1;
			capturedInjectLineEnd = 1;
			showCapturePrompt = isAssigningMenuToggleKey = isAssigningModKey = false; editingBindingIndex = -1;
			LogActivity(LOG_KEY_ASSIGNMENT_PROMPT_CANCELLED);
		}

		std::vector<USHORT> ScanCurrentlyHeldKeys() {
			std::vector<USHORT> held;
			bool seen[RawInput::kMaxVKey] = {};
			for (int i = 0; i < vkNameTableCount; i++) {
				USHORT vk = vkNameTable[i].vKey;
				if (vk >= RawInput::kMaxVKey || seen[vk]) continue;
				if (RawInput::IsKeyHeldReal(vk)) {
					seen[vk] = true;
					held.push_back(vk);
				}
			}
			for (int i = 0; i < xboxVkNameTableCount; i++) {
				USHORT vk = xboxVkNameTable[i].vKey;
				if (vk >= RawInput::kMaxVKey || seen[vk]) continue;
				if (RawInput::IsKeyHeldReal(vk)) {
					seen[vk] = true;
					held.push_back(vk);
				}
			}
			for (int i = 0; i < psVkNameTableCount; i++) {
				USHORT vk = psVkNameTable[i].vKey;
				if (vk >= RawInput::kMaxVKey || seen[vk]) continue;
				if (RawInput::IsKeyHeldReal(vk)) {
					seen[vk] = true;
					held.push_back(vk);
				}
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
					if (currentlyHeld.size() >= 2 && currentlyHeld.size() <= 3) {
						comboHoldKeys = currentlyHeld;
						comboHoldStartTime = std::chrono::steady_clock::now();
						LogActivity("KeyBindMenu: Multi-key combo capture adjusted: now holding " + ComboKeysDisplayName(currentlyHeld));
					} else {
						comboHoldActive = false;
						comboHoldKeys.clear();
						LogActivity(LOG_MULTI_KEY_COMBO_CAPTURE_CANCELLED_KEY);
					}
					return;
				}
			}
			if (currentlyHeld.size() > 3) {
				comboHoldActive = false;
				comboHoldKeys.clear();
				LogActivity(LOG_MULTI_KEY_COMBO_CAPTURE_CANCELLED_MORE);
				return;
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
				LogActivity("KeyBindMenu: Multi-key combo captured: " + ComboKeysDisplayName(capturedComboKeys));
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
				const KeyBind* rowBind = nullptr;
				for (const auto& b : bindings) {
					if (!b.isInject || !b.scriptDescribed || b.injectScriptName != row.scriptName || b.injectFunctionName != row.functionName) continue;
					if (b.vKey != row.vKey) continue;
					rowBind = &b;
					break;
				}
				if (!row.scriptName.empty() && ModKeyBindings::HasTriggerConfig(row.scriptName, row.functionName)) {
					ModKeyBindings::TriggerConfig storedTrigger = ModKeyBindings::GetTriggerConfig(row.scriptName, row.functionName);
					if (storedTrigger.triggerType == 0 && storedTrigger.holdSeconds > 0.0f) {
						char holdBuf[32];
						snprintf(holdBuf, sizeof(holdBuf), "%.1fs", storedTrigger.holdSeconds);
						triggerLabel = std::string("Long Press (") + holdBuf + ")";
						result.anyLongPress = true;
						result.longPressSeconds = (double)storedTrigger.holdSeconds;
					} else if (storedTrigger.triggerType == 2) {
						if (bestInstantPriority < 2) bestInstantPriority = 2;
						triggerLabel = "Repeat";
					} else if (storedTrigger.triggerType == 1) {
						if (bestInstantPriority < 1) bestInstantPriority = 1;
						triggerLabel = "On Release";
					} else {
						if (bestInstantPriority < 0) bestInstantPriority = 0;
						triggerLabel = "On Press";
					}
				} else if (rowBind != nullptr) {
					if (rowBind->holdSeconds > 0.0f) {
						result.anyLongPress = true;
						result.longPressSeconds = rowBind->holdSeconds;
						char holdBuf[32];
						snprintf(holdBuf, sizeof(holdBuf), "%.1fs", rowBind->holdSeconds);
						triggerLabel = std::string("Long Press (") + holdBuf + ")";
					} else if (rowBind->isInstant && rowBind->instantTriggerType == 2) {
						if (bestInstantPriority < 2) bestInstantPriority = 2;
						triggerLabel = "Repeat";
					} else if (rowBind->isInstant && rowBind->instantTriggerType == 1) {
						if (bestInstantPriority < 1) bestInstantPriority = 1;
						triggerLabel = "On Release";
					} else {
						if (bestInstantPriority < 0) bestInstantPriority = 0;
						triggerLabel = "On Press";
					}
				} else if (row.usesHoldTime) {
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
					std::string triggerText = "On Press";
					for (const auto& b : bindings) {
						if (!b.isInject || !b.scriptDescribed || b.injectScriptName != row.scriptName || b.injectFunctionName != row.functionName) continue;
						if (b.isInstant && b.instantTriggerType == 1) triggerText = "On Release";
						else if (b.isInstant && b.instantTriggerType == 2) triggerText = "Repeat";
						else if (b.holdSeconds > 0.0f) { char holdBuf[32]; snprintf(holdBuf, sizeof(holdBuf), "%.1fs", b.holdSeconds); triggerText = std::string("Long Press (") + holdBuf + ")"; }
						break;
					}
					triggerLabel = "Script Inject - " + triggerText;
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
				bool hasStoredTrigger = ModKeyBindings::HasTriggerConfig(scriptName, functionName);
				if (hasStoredTrigger) {
					ModKeyBindings::TriggerConfig storedTrigger = ModKeyBindings::GetTriggerConfig(scriptName, functionName);
					if (storedTrigger.triggerType == 0 && storedTrigger.holdSeconds > 0.0f) {
						result.anyLongPress = true;
						result.longPressSeconds = (double)storedTrigger.holdSeconds;
						triggerLabel += " / Long Press";
					}
					if (storedTrigger.triggerType == 2) {
						bestInstantPriority = (std::max)(bestInstantPriority, 2);
						triggerLabel += " / Repeat";
					} else if (storedTrigger.triggerType == 1) {
						bestInstantPriority = (std::max)(bestInstantPriority, 1);
						triggerLabel += " / On Release";
					}
				} else if (row.usesHoldTime) {
					result.anyLongPress = true;
					triggerLabel += " / Long Press";
				}
				if (!hasStoredTrigger) {
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

			if (ImGui::IsKeyPressed((ImGuiKey)VK_ESCAPE)) {
			CancelCaptureIfActive();
			ImGui::End();
			return;
		}

			ModKeyReadOnlyInfo modKeyInfo;
			if (isAssigningModKey) {
				modKeyInfo = captureIsCombo
					? ComputeComboModKeyReadOnlyInfo(modKeyCaptureScriptName, modKeyCaptureFunctionName)
					: ComputeModKeyReadOnlyInfo(modKeyCaptureScriptName, modKeyCaptureFunctionName);

				ImGui::TextWrapped(UI_FMT_SCRIPT_FUNCTION_BRACKETS, modKeyCaptureScriptName.c_str(), modKeyCaptureFunctionName.c_str());
				ImGui::Separator();
			}

			if (!isAssigningMenuToggleKey) {
				bool wasCombo = captureIsCombo;
				bool wasInject = captureIsInject;
				ImGui::TextUnformatted(UI_LBL_BIND_TYPE); ImGui::SameLine();
				if (ImGui::RadioButton(UI_RADIO_SINGLE_KEY, !captureIsCombo && !captureIsInject)) { captureIsCombo = false; captureIsInject = false; }
				ImGui::SameLine();
				if (ImGui::RadioButton(UI_RADIO_MULTI_KEY_COMBO, captureIsCombo)) { captureIsCombo = true; captureIsInject = false; }
				if (!isAssigningModKey) {
					ImGui::SameLine();
					if (ImGui::RadioButton(UI_RADIO_SCRIPT_LINES, captureIsInject)) { captureIsInject = true; captureIsCombo = false; }
				}
				if (captureIsCombo != wasCombo) {
					capturedVKey = 0;
					ResetComboCaptureState();
				}
				if (captureIsInject != wasInject) {
					capturedToggleMode = capturedLongPressMode = capturedHasFuncOn = capturedHasFuncOff = false;
				}
				ImGui::Separator();
			}

			float captureColX = ImGui::GetCursorPosX();
			float captureColTopY = ImGui::GetCursorPosY();

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
						if (pressedKey == 0) {
							if (!psCaptureEdgePrimed) {
								prevHeldPsKeysCapture.clear();
								for (USHORT psKey : RawInput::PlaystationVKeys()) {
									if (RawInput::IsKeyHeldReal(psKey)) {
										prevHeldPsKeysCapture.insert(psKey);
									}
								}
								psCaptureEdgePrimed = true;
							}
							std::unordered_set<USHORT> nowHeldPsKeys;
							for (USHORT psKey : RawInput::PlaystationVKeys()) {
								if (RawInput::IsKeyHeldReal(psKey)) {
									nowHeldPsKeys.insert(psKey);
									if (prevHeldPsKeysCapture.count(psKey) == 0) { pressedKey = psKey; break; }
								}
							}
							prevHeldPsKeysCapture = nowHeldPsKeys;
						}
						if (pressedKey == 0) {
							if (!padCaptureEdgePrimed) {
								prevHeldPadKeysCapture.clear();
								for (USHORT gpKey : RawInput::GamepadVKeys()) {
									if (RawInput::IsKeyHeldReal(gpKey)) {
										prevHeldPadKeysCapture.insert(gpKey);
									}
								}
								padCaptureEdgePrimed = true;
							}
							std::unordered_set<USHORT> nowHeldPadKeys;
							for (USHORT gpKey : RawInput::GamepadVKeys()) {
								if (RawInput::IsKeyHeldReal(gpKey)) {
									nowHeldPadKeys.insert(gpKey);
									if (prevHeldPadKeysCapture.count(gpKey) == 0) { pressedKey = gpKey; break; }
								}
							}
							prevHeldPadKeysCapture = nowHeldPadKeys;
						}
					}
					if (pressedKey != 0) {
						singleHoldKey = pressedKey;
						capturedCtrl = ImGui::GetIO().KeyCtrl;
						capturedShift = ImGui::GetIO().KeyShift;
						capturedAlt = ImGui::GetIO().KeyAlt;
						singleHoldStartTime = std::chrono::steady_clock::now();
						singleHoldActive = true;
					}
				}

				if (singleHoldActive) {
					if (!RawInput::IsKeyHeldReal(singleHoldKey)) {
						singleHoldActive = false;
						singleHoldKey = 0;
						LogActivity(LOG_SINGLE_KEY_CAPTURE_CANCELLED_KEY_RELEASED);
					} else {
						double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - singleHoldStartTime).count();
						if (heldSeconds >= kComboHoldSeconds) {
							capturedVKey = singleHoldKey;
							singleHoldActive = false;
							singleHoldKey = 0;
							if (!isAssigningModKey && !capturedInstantUserSet) {
								capturedInstantMode = true;
								capturedInstantTriggerType = 0;
							}
							LogActivity("KeyBindMenu: Single key captured: " + NameForVKey(capturedVKey));
							spdlog::info(LOG_KEYBINDMENU_CAPTURED_SINGLE_KEY_VKEY_FMT,
								capturedVKey, NameForVKey(capturedVKey), capturedCtrl, capturedShift, capturedAlt);
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
			ImGui::EndChild();

			if (ImGui::Button(UI_BTN_RESET, ImVec2(105, 22))) {
				capturedVKey = 0;
				capturedCtrl = capturedShift = capturedAlt = capturedToggleMode = capturedLongPressMode = capturedHasFuncOn = capturedHasFuncOff = false;
				capturedHoldSeconds = 0.0f;
				capturedToggleType = 0;
				capturedInstantMode = false;
				capturedInstantTriggerType = 0;
				capturedRepeatAccelMult = 1.0f;
				capturedInstantUserSet = false;
				LogActivity(LOG_KEYBIND_HAS_BEEN_RESET);
			}
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

				if (ImGui::Button(UI_BTN_RESET, ImVec2(105, 22))) {
					ResetComboCaptureState();
					capturedToggleMode = capturedLongPressMode = capturedHasFuncOn = capturedHasFuncOff = false;
					capturedHoldSeconds = 0.0f;
					capturedToggleType = 0;
					capturedInstantMode = false;
					capturedInstantTriggerType = 0;
					capturedRepeatAccelMult = 1.0f;
					capturedInstantUserSet = false;
					LogActivity(LOG_MULTI_KEY_COMBO_HAS_BEEN_RESET);
				}
			}
		
			float captureColBottomY = ImGui::GetCursorPosY();
			float optionsColX = captureColX + 105.0f + ImGui::GetStyle().ItemSpacing.x;
			ImGui::SetCursorPos(ImVec2(optionsColX, captureColTopY));

			ImGui::BeginGroup();
			if (!isAssigningMenuToggleKey && !captureIsInject) {

				if (capturedInstantMode && !isAssigningModKey) ImGui::BeginDisabled();
				if (isAssigningModKey) ImGui::BeginDisabled();
				ImGui::Checkbox(UI_CHK_TOGGLE, &capturedToggleMode);
				if (isAssigningModKey && ImGui::IsItemHovered()) {
					ImGui::SetTooltip(UI_TIP_TOGGLE_SCRIPT_UNAVAILABLE);
				}
				if (isAssigningModKey) ImGui::EndDisabled();
				if (capturedInstantMode && !isAssigningModKey && ImGui::IsItemHovered()) {
					ImGui::SetTooltip(UI_TIP_UNCHECK_INSTANT_FIRST);
				}
				if (isAssigningModKey) {
					if (ImGui::Checkbox(UI_CHK_LONG_PRESS, &capturedLongPressMode)) {
						capturedInstantMode = !capturedLongPressMode;
						capturedInstantUserSet = true;
					}
				} else {
					ImGui::Checkbox(UI_CHK_LONG_PRESS, &capturedLongPressMode);
					if (capturedInstantMode && ImGui::IsItemHovered()) {
						ImGui::SetTooltip(UI_TIP_UNCHECK_INSTANT_FIRST);
					}
				}
				if (capturedInstantMode && !isAssigningModKey) ImGui::EndDisabled();
				
				if (capturedLongPressMode) {
				    ImGui::SetNextItemWidth(75);
				    ImGui::InputFloat("##capturedHoldInput", &capturedHoldSeconds, 0.0f, 0.0f, "%.1fs");
				    if (capturedHoldSeconds < 0.0f) capturedHoldSeconds = 0.0f;
				    if (ImGui::Button(UI_BTN_MINUS, ImVec2(35, 20))) { if ((capturedHoldSeconds -= 0.5f) < 0.0f) capturedHoldSeconds = 0.0f; } ImGui::SameLine(40);
				    if (ImGui::Button(UI_BTN_PLUS, ImVec2(35, 20))) capturedHoldSeconds += 0.5f;
				}

				bool toggleOrLongPress = capturedToggleMode || capturedLongPressMode;
				if (!isAssigningModKey && toggleOrLongPress) ImGui::BeginDisabled();
				if (isAssigningModKey) {
					bool instantSelected = capturedInstantMode;
					if (ImGui::Checkbox(UI_CHK_INSTANT, &instantSelected)) {
						capturedInstantMode = true;
						capturedLongPressMode = false;
						capturedInstantUserSet = true;
					}
				} else {
					if (ImGui::Checkbox(UI_CHK_INSTANT, &capturedInstantMode)) {
						capturedInstantUserSet = true;
					}
					if (toggleOrLongPress && ImGui::IsItemHovered()) {
						ImGui::SetTooltip(UI_TIP_UNCHECK_TOGGLE_FIRST);
					}
				}
				if (!isAssigningModKey && toggleOrLongPress) ImGui::EndDisabled();

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

					comboAvailable = IsComboAvailable(
						capturedVKey, capturedCtrl, capturedShift, capturedAlt,
						capturedHoldSeconds, ManualCaptureTriggerMask(capturedVKey, capturedCtrl, capturedShift, capturedAlt), editingBindingIndex);
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
			ImGui::EndGroup();
			ImGui::SetCursorPosY((std::max)(ImGui::GetCursorPosY(), captureColBottomY));
			ImGui::Separator();

			bool isUpperPathValid = false, isLowerPathValid = false;
			int scriptStatus = 0, lowerStatus = 0;

			static char lastStatCheckedOnBuffer[512] = "";
			static int cachedOnStatus = 0;
			static char lastStatCheckedOffBuffer[512] = "";
			static int cachedOffStatus = 0;
		
			if (!isAssigningMenuToggleKey && !isAssigningModKey) {
				if (capturedScriptPathOnBuffer[0] != '\0') {
					std::string txt(capturedScriptPathOnBuffer);
					if (HasLuaExtensionCI(txt)) {
						if (strcmp(capturedScriptPathOnBuffer, lastStatCheckedOnBuffer) != 0) {
							std::error_code statEcOn;
							cachedOnStatus = std::filesystem::exists(ResolveScriptPath(capturedScriptPathOnBuffer), statEcOn) ? 3 : 2;
							snprintf(lastStatCheckedOnBuffer, sizeof(lastStatCheckedOnBuffer), "%s", capturedScriptPathOnBuffer);
						}
						scriptStatus = cachedOnStatus;
						isUpperPathValid = (scriptStatus == 3);
					} else { scriptStatus = 1; lastStatCheckedOnBuffer[0] = '\0'; }
				} else lastStatCheckedOnBuffer[0] = '\0';

				if (capturedToggleMode && capturedScriptPathOffBuffer[0] != '\0') {
					std::string txt(capturedScriptPathOffBuffer);
					if (HasLuaExtensionCI(txt)) {
						if (strcmp(capturedScriptPathOffBuffer, lastStatCheckedOffBuffer) != 0) {
							std::error_code statEcOff;
							cachedOffStatus = std::filesystem::exists(ResolveScriptPath(capturedScriptPathOffBuffer), statEcOff) ? 3 : 2;
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

				if (captureIsInject) {
					ImGui::Text(UI_LBL_SCRIPT_PATH);
					ImGui::SetNextItemWidth(-1);
					ImGui::InputText("##captureScriptInputOn", capturedScriptPathOnBuffer, IM_ARRAYSIZE(capturedScriptPathOnBuffer));
					ImGui::AlignTextToFramePadding();
					ImGui::Text(UI_LBL_LINE_START); ImGui::SameLine();
					ImGui::SetNextItemWidth(100);
					ImGui::InputInt("##injectLineStart", &capturedInjectLineStart);
					if (capturedInjectLineStart < 1) capturedInjectLineStart = 1;
					ImGui::AlignTextToFramePadding();
					ImGui::Text(UI_LBL_LINE_END); ImGui::SameLine();
					ImGui::SetNextItemWidth(100);
					ImGui::InputInt("##injectLineEnd", &capturedInjectLineEnd);
					if (capturedInjectLineEnd < 1) capturedInjectLineEnd = 1;
					ImGui::Separator(); ImGui::Spacing();
				}
				else if (capturedToggleMode) {
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
			if (captureIsInject) {
				pathsValid = isUpperPathValid && capturedInjectLineStart >= 1 && capturedInjectLineEnd >= capturedInjectLineStart;
			}
			bool functionsValid = true;
			
			if (!isAssigningMenuToggleKey && !isAssigningModKey && !captureIsInject) {
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
			bool holdValid = !capturedLongPressMode || capturedHoldSeconds > 0.0f;
			bool canFinalize = captureReady && holdValid && (assignmentIsValid && (isAssigningMenuToggleKey || isAssigningModKey || (pathsValid && functionsValid)));
			float paddingY = ImGui::GetStyle().WindowPadding.y;
			float buttonHeight = 30.0f;
			float bottomAnchorY = ImGui::GetWindowHeight() - paddingY - buttonHeight;
			
			ImGui::SetCursorPosY(bottomAnchorY);

			if (!canFinalize) ImGui::BeginDisabled();
			auto applyTriggerChoice = [&]() {
				int triggerType = capturedInstantMode ? capturedInstantTriggerType : 0;
				float holdSeconds = 0.0f;
				float repeatMult = 1.0f;
				if (capturedInstantMode) {
					if (triggerType == 2) repeatMult = capturedRepeatAccelMult;
				} else if (capturedLongPressMode && !capturedToggleMode) {
					holdSeconds = capturedHoldSeconds;
				}
				ModKeyBindings::SetTriggerConfigWithoutSave(modKeyCaptureScriptName, modKeyCaptureFunctionName, triggerType, holdSeconds, repeatMult);
				for (auto& b : bindings) {
					if (!b.isInject || !b.scriptDescribed || b.injectScriptName != modKeyCaptureScriptName || b.injectFunctionName != modKeyCaptureFunctionName) continue;
					b.isInstant = (triggerType == 1 || triggerType == 2);
					b.instantTriggerType = triggerType;
					b.holdSeconds = holdSeconds;
					b.repeatAccelMult = repeatMult;
				}
			};
			if (ImGui::Button(UI_BTN_FINALIZE, ImVec2(145, buttonHeight))) {
				if (isAssigningModKey) {
					if (captureIsCombo) {
						std::string comboKeyName = ComboKeysDisplayName(capturedComboKeys);
						ModKeyBindings::SetOverride(modKeyCaptureScriptName, modKeyCaptureFunctionName, comboKeyName);
						DebuggerMenu::LogBindEvent("Mod combo reassigned: " + modKeyCaptureScriptName + " [" + modKeyCaptureFunctionName + "] -> " + comboKeyName);
						LogActivity("KeyBindMenu: Mod combo reassigned: " + modKeyCaptureScriptName + " [" + modKeyCaptureFunctionName + "] -> " + comboKeyName);
						applyTriggerChoice();
						MarkDisplayCacheDirty();

						ResetComboCaptureState();
						captureIsCombo = false;
					}
					else {
						USHORT oldVKey = 0;
						bool slotMemberFound = false;
						ModKeyBindings::BindSlot capturedSlot = SlotOfVKey(capturedVKey);
						for (const auto& row : LuaKeyState::GetTrackedKeyInfo()) {
							if (row.scriptName != modKeyCaptureScriptName || row.functionName != modKeyCaptureFunctionName) {
								continue;
							}
							if (!slotMemberFound && SlotOfVKey(row.vKey) == capturedSlot) {
								oldVKey = row.vKey;
								slotMemberFound = true;
							}
						}
						if (slotMemberFound) {
							ModKeyBindings::SetSlotOverride(modKeyCaptureScriptName, modKeyCaptureFunctionName, capturedSlot, NameForVKey(capturedVKey));
						} else {
							for (const auto& row : LuaKeyState::GetTrackedKeyInfo()) {
								if (row.scriptName == modKeyCaptureScriptName && row.functionName == modKeyCaptureFunctionName) {
									oldVKey = row.vKey;
									break;
								}
							}
							ModKeyBindings::SetOverride(modKeyCaptureScriptName, modKeyCaptureFunctionName, NameForVKey(capturedVKey));
						}
						DebuggerMenu::LogBindEvent("Mod key reassigned: " + modKeyCaptureScriptName + " [" + modKeyCaptureFunctionName + "] -> " + NameForVKey(capturedVKey));
						LogActivity("KeyBindMenu: Mod key reassigned: " + modKeyCaptureScriptName + " [" + modKeyCaptureFunctionName + "] -> " + NameForVKey(capturedVKey));
						if (oldVKey != 0 && oldVKey != capturedVKey) {
							LuaKeyState::ReassignBinding(oldVKey, capturedVKey, modKeyCaptureScriptName, modKeyCaptureFunctionName);
						}
						applyTriggerChoice();
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
					LogActivity("KeyBindMenu: Menu Hotkey reassigned to " + NameForVKey(capturedVKey));
					for (const auto& bind : bindings) {
						if (!bind.IsCombo() && bind.vKey == capturedVKey && !bind.disabled) {
							LogActivity("KeyBindMenu: Warning: menu hotkey " + NameForVKey(capturedVKey) + " is also a manual binding - both will fire", false);
							break;
						}
					}
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

					if (captureIsInject) {
						std::string injectSourcePath = ResolveScriptPath(capturedScriptPathOnBuffer);
						int injectStart = capturedInjectLineStart;
						int injectEnd = capturedInjectLineEnd;
						if (injectEnd < injectStart) injectEnd = injectStart;
						std::string injectContent = BuildInjectContent(injectSourcePath, injectStart, injectEnd);
						if (injectContent.empty()) {
							LogActivity("KeyBindMenu: Script-line injection failed - could not read lines " + std::to_string(injectStart) + "-" + std::to_string(injectEnd) + " of " + FileNameOnly(injectSourcePath), false);
						} else {
							RunInjectCompileCheck(injectContent);
							if (editingBindingIndex != -1 && editingBindingIndex < (int)bindings.size()) {
								USHORT oldVKey = bindings[editingBindingIndex].vKey;
								KeyBind editedBind = bindings[editingBindingIndex];
								editedBind.vKey = capturedVKey;
								editedBind.needCtrl = capturedCtrl;
								editedBind.needShift = capturedShift;
								editedBind.needAlt = capturedAlt;
								editedBind.keyName = NameForVKey(capturedVKey);
								editedBind.isToggle = false;
								editedBind.scriptPathOn = injectSourcePath;
								editedBind.scriptPathOff = "";
								editedBind.functionOn = "";
								editedBind.functionOff = "";
								editedBind.functionTap = "";
								editedBind.isInject = true;
								editedBind.injectLineStart = injectStart;
								editedBind.injectLineEnd = injectEnd;
								bindings[editingBindingIndex] = editedBind;
								RemoveDispatcherIfUnused(oldVKey);
								EnsureDispatcherRegistered(capturedVKey);
								SaveBindings();
								MarkDisplayCacheDirty();
								LogActivity("KeyBindMenu: Edited script-line binding -> " + CombinedDisplayName(editedBind));
							} else {
								KeyBind newInjectBind{};
								newInjectBind.vKey = capturedVKey;
								newInjectBind.needCtrl = capturedCtrl;
								newInjectBind.needShift = capturedShift;
								newInjectBind.needAlt = capturedAlt;
								newInjectBind.keyName = NameForVKey(capturedVKey);
								newInjectBind.scriptPathOn = injectSourcePath;
								newInjectBind.isInject = true;
								newInjectBind.injectLineStart = injectStart;
								newInjectBind.injectLineEnd = injectEnd;
								bindings.push_back(newInjectBind);
								EnsureDispatcherRegistered(capturedVKey);
								SaveBindings();
								MarkDisplayCacheDirty();
								DebuggerMenu::LogBindEvent("Bound " + newInjectBind.keyName + " -> script lines " + std::to_string(injectStart) + "-" + std::to_string(injectEnd) + " of " + injectSourcePath);
								LogActivity("KeyBindMenu: Bound " + newInjectBind.keyName + " to script lines " + std::to_string(injectStart) + "-" + std::to_string(injectEnd) + " of " + FileNameOnly(injectSourcePath));
							}
						}
					}
					else if (captureIsCombo) {
						if (editingBindingIndex != -1 && editingBindingIndex < (int)bindings.size()) {
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
							editedBind.disabled = bindings[editingBindingIndex].disabled;
							editedBind.toggleState = bindings[editingBindingIndex].toggleState;

							bindings[editingBindingIndex] = editedBind;
							SaveBindings();
							MarkDisplayCacheDirty();
							LogActivity("KeyBindMenu: Edited combo binding -> " + CombinedDisplayName(editedBind));
						} else {
							AddComboBinding(capturedComboKeys, capturedToggleMode, finalPathOn, finalPathOff, finalFuncOn, finalFuncOff, finalFuncTap, finalHoldSeconds, capturedInstantMode, capturedInstantTriggerType, capturedRepeatAccelMult);
						}
					}
					else if (editingBindingIndex != -1 && editingBindingIndex < (int)bindings.size()) {
						USHORT oldVKey = bindings[editingBindingIndex].vKey;
						KeyBind editedBind{ capturedVKey, capturedCtrl, capturedShift, capturedAlt, NameForVKey(capturedVKey), capturedToggleMode, finalPathOn, finalPathOff, false, finalHoldSeconds };
						editedBind.isInstant = capturedInstantMode;
						editedBind.instantTriggerType = capturedInstantTriggerType;
						editedBind.repeatAccelMult = capturedRepeatAccelMult;
						editedBind.functionOn = finalFuncOn;
						editedBind.functionOff = finalFuncOff;
						editedBind.functionTap = finalFuncTap;
						editedBind.disabled = bindings[editingBindingIndex].disabled;
						editedBind.toggleState = bindings[editingBindingIndex].toggleState;
						
						bindings[editingBindingIndex] = editedBind;
						RemoveDispatcherIfUnused(oldVKey); EnsureDispatcherRegistered(capturedVKey); SaveBindings();
						MarkDisplayCacheDirty();
						LogActivity("KeyBindMenu: Edited binding -> " + CombinedDisplayName(editedBind));
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
					captureIsInject = false;
					capturedInjectLineStart = 1;
					capturedInjectLineEnd = 1;
				}
				capturedVKey = 0; capturedHoldSeconds = 0.0f; 
				showCapturePrompt = isAssigningMenuToggleKey = isAssigningModKey = false; editingBindingIndex = -1;
			}
			if (!canFinalize) ImGui::EndDisabled(); ImGui::SameLine();
			
			float paddingX = ImGui::GetStyle().WindowPadding.x;
			float targetCancelX = ImGui::GetWindowWidth() - paddingX - 145.0f - 8.0f;
			ImGui::SameLine(targetCancelX);
			
			if (ImGui::Button(UI_BTN_CANCEL, ImVec2(145, buttonHeight))) {
				CancelCaptureIfActive();
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
				} else if (bind.isInject) {
					std::string injectFileOn = std::filesystem::path(bind.scriptPathOn).filename().string();
					char injectLineBuf[48];
					snprintf(injectLineBuf, sizeof(injectLineBuf), "Lines %d-%d -> ", bind.injectLineStart, bind.injectLineEnd);
					entry.detailText = std::string(injectLineBuf) + injectFileOn;
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
		void CopyPrefillTruncWarn(char* dst, size_t dstSize, const std::string& src, const char* what) {
			if (src.size() >= dstSize) LogActivity(std::string("Edit prefill truncated: ") + what + " exceeds its buffer", false);
			snprintf(dst, dstSize, "%s", src.c_str());
		}

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
			constexpr float kWindowMinHeight = 360.0f;
			ImGui::SetNextWindowSize(ImVec2(finalMinWidthFloor, kWindowMinHeight), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints(ImVec2(finalMinWidthFloor, kWindowMinHeight), ImVec2(FLT_MAX, FLT_MAX));

			bool wasOpenBeforeBegin = p_open ? *p_open : true;
			bool windowIsOpen = ImGui::Begin(UI_WINDOW_TITLE, p_open);
			bool closedThisFrame = p_open ? (wasOpenBeforeBegin && !*p_open) : false;
			if (closedThisFrame) {
				CancelCaptureIfActive();
			}
			if (!windowIsOpen) { ImGui::End(); if (showCapturePrompt) DrawKeyCapturePrompt(); return; }
			if (ImGui::Button(UI_BTN_DEBUGGER)) {
				DebuggerMenu::menuOpen = !DebuggerMenu::menuOpen;
				LogActivity(DebuggerMenu::menuOpen ? UI_LOG_DEBUGGER_OPENED : UI_LOG_DEBUGGER_CLOSED);
				spdlog::info(LOG_KEYBINDMENU_DEBUGGER_OVERLAY_FMT, DebuggerMenu::menuOpen ? "OPENED" : "CLOSED");
			}
			ImGui::SameLine();
			
			std::string buttonLabel = UI_LBL_MENU_HOTKEY_PREFIX + NameForVKey(menuToggleVKey) + UI_LBL_MENU_HOTKEY_SUFFIX;
			if (ImGui::Button(buttonLabel.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { 
				isAssigningMenuToggleKey = true; isAssigningModKey = false; showCapturePrompt = true;
				requestCaptureFocus = true;
				LogActivity(LOG_MENU_KEY_REASSIGNMENT_PROMPT_OPENED);
			}
			ImGui::Separator();

			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(UI_HDR_CAPTURE_SUPPRESSION);
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", UI_TIP_CAPTURE_SUPPRESSION);
			}
			{
				const bool prevKb = captureSuppressKeyboard;
				const bool prevMouse = captureSuppressMouse;
				const bool prevGamepad = captureSuppressGamepad;

				ImGui::Checkbox(UI_CHK_SUPPRESS_KEYBOARD, &captureSuppressKeyboard);
				ImGui::SameLine();
				ImGui::Checkbox(UI_CHK_SUPPRESS_MOUSE, &captureSuppressMouse);
				ImGui::SameLine();
				ImGui::Checkbox(UI_CHK_SUPPRESS_GAMEPAD, &captureSuppressGamepad);

				if (captureSuppressKeyboard != prevKb ||
				captureSuppressMouse != prevMouse ||
				captureSuppressGamepad != prevGamepad) {
				SaveBindings();
				LogActivity(std::string("Capture suppression set - ") +
				UI_CHK_SUPPRESS_KEYBOARD + ": " + (captureSuppressKeyboard ? "on" : "off") + ", " +
				UI_CHK_SUPPRESS_MOUSE + ": " + (captureSuppressMouse ? "on" : "off") + ", " +
				UI_CHK_SUPPRESS_GAMEPAD + ": " + (captureSuppressGamepad ? "on" : "off"));
				}
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

			{
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
					std::vector<LuaKeyState::TrackedKeyInfo> groupMembers;
					std::vector<LuaKeyState::TrackedComboKeyInfo> mergedCombos;
				};
				std::vector<UnifiedRow> rows;
				rows.reserve(trackedKeys.size() + trackedCombos.size() + bindings.size());

				for (LuaKeyState::TrackedKeyInfo& info : trackedKeys) {
					if (!info.hasDescription && manualBoundVKeys.count(info.vKey) > 0) {
						continue;
					}
					USHORT displayVKey = ResolveDisplayVKey(info);
					bool conflicted = info.isConflicted || conflictedVKeys.count(displayVKey) > 0;
					UnifiedRow* existing = nullptr;
					for (UnifiedRow& r : rows) {
						if (!r.isManual && !r.isComboScript && r.info.hasDescription
							&& r.info.scriptName == info.scriptName && r.info.functionName == info.functionName) {
							existing = &r;
							break;
						}
					}
					if (existing) {
						existing->groupMembers.push_back(info);
						if (conflicted) {
							existing->conflicted = true;
						}
						continue;
					}
					UnifiedRow row;
					row.isManual = false;
					row.info = info;
					row.groupMembers.push_back(info);
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
					if (bindings[i].isInject && bindings[i].scriptDescribed) continue;
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
				for (size_t i = 0; i < rows.size(); ) {
					if (!rows[i].isComboScript) {
						i++;
						continue;
					}
					bool merged = false;
					for (UnifiedRow& r : rows) {
						if (r.isComboScript || r.isManual || !r.info.hasDescription) continue;
						if (r.info.scriptName == rows[i].comboInfo.scriptName && r.info.functionName == rows[i].comboInfo.functionName) {
							r.mergedCombos.push_back(rows[i].comboInfo);
							if (rows[i].conflicted) r.conflicted = true;
							merged = true;
							break;
						}
					}
					if (merged) rows.erase(rows.begin() + i);
					else i++;
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
						bool isDisabled = bindings[bindIdx].disabled || bindings[bindIdx].autoDisabled;
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
							if (bindings[bindIdx].isInject && bindings[bindIdx].autoDisabled) {
								std::string missingTip = std::string(UI_TIP_INJECT_SOURCE_MISSING) + "\n" + std::filesystem::path(bindings[bindIdx].scriptPathOn).filename().string();
								ImGui::SetTooltip("%s", missingTip.c_str());
							} else {
								ImGui::SetTooltip(UI_TIP_CLICK_HOLD_REMOVE, isDisabled ? UI_WORD_ENABLE : UI_WORD_DISABLE);
							}
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
						bool isDisabled = ModKeyBindings::IsDisabled(mkScriptName, mkFunctionName) || (FindAutoDisabledDescribedInject(mkScriptName, mkFunctionName) != nullptr);
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
							const KeyBind* missingInject = FindAutoDisabledDescribedInject(mkScriptName, mkFunctionName);
							if (missingInject) {
								std::string missingTip = std::string(UI_TIP_INJECT_SOURCE_MISSING) + "\n" + std::filesystem::path(missingInject->scriptPathOn).filename().string();
								ImGui::SetTooltip("%s", missingTip.c_str());
							} else {
								ImGui::SetTooltip(hasOverride
									? UI_TIP_CLICK_HOLD_RESET
									: UI_TIP_CLICK_NO_REMOVE, isDisabled ? UI_WORD_ENABLE : UI_WORD_DISABLE);
							}
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
							capturedInstantUserSet = true;
							capturedHasFuncOn = !bindings[i].functionOn.empty() || !bindings[i].functionTap.empty();
							capturedHasFuncOff = !bindings[i].functionOff.empty();
							captureIsInject = bindings[i].isInject;
							capturedInjectLineStart = bindings[i].isInject ? bindings[i].injectLineStart : 1;
							capturedInjectLineEnd = bindings[i].isInject ? bindings[i].injectLineEnd : 1;

							CopyPrefillTruncWarn(capturedScriptPathOnBuffer, sizeof(capturedScriptPathOnBuffer), bindings[i].scriptPathOn, "script path (on)");
							CopyPrefillTruncWarn(capturedScriptPathOffBuffer, sizeof(capturedScriptPathOffBuffer), bindings[i].scriptPathOff, "script path (off)");
							CopyPrefillTruncWarn(capturedFuncOnBuffer, sizeof(capturedFuncOnBuffer), bindings[i].functionOn, "function (on)");
							CopyPrefillTruncWarn(capturedFuncOffBuffer, sizeof(capturedFuncOffBuffer), bindings[i].functionOff, "function (off)");
							CopyPrefillTruncWarn(capturedFuncTapBuffer, sizeof(capturedFuncTapBuffer), bindings[i].functionTap, "function (tap)");

							if (bindings[i].isToggle) {
								capturedToggleType = (bindings[i].scriptPathOn == bindings[i].scriptPathOff) ? 0 : 1;
							} else {
								capturedToggleType = 0;
							}

							isAssigningMenuToggleKey = false; isAssigningModKey = false; showCapturePrompt = true;
							requestCaptureFocus = true;
							LogActivity("KeyBindMenu: Key Assignment Edit Prompt opened " + itemLabel);
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
							ModKeyReadOnlyInfo seededInfo = captureIsCombo
								? ComputeComboModKeyReadOnlyInfo(modKeyCaptureScriptName, modKeyCaptureFunctionName)
								: ComputeModKeyReadOnlyInfo(modKeyCaptureScriptName, modKeyCaptureFunctionName);
							capturedToggleMode = false;
							capturedLongPressMode = seededInfo.anyLongPress;
							capturedHoldSeconds = (float)seededInfo.longPressSeconds;
							capturedInstantMode = seededInfo.anyInstant || !seededInfo.anyLongPress;
							capturedInstantTriggerType = seededInfo.instantType;
							capturedRepeatAccelMult = ModKeyBindings::GetTriggerConfig(modKeyCaptureScriptName, modKeyCaptureFunctionName).repeatAccelMult;
							capturedInstantUserSet = false;
							requestCaptureFocus = true;
							showCapturePrompt = true;
						};

						ImGui::PushStyleColor(ImGuiCol_Text, keyNameColor);
						std::string comboLabel = ComboKeysDisplayName(row.comboInfo.activeKeys);
						float comboButtonWidth = ImGui::CalcTextSize(comboLabel.c_str()).x + 24.0f;
						if (comboButtonWidth < 130.0f) comboButtonWidth = 130.0f;
						if (comboButtonWidth > 210.0f) comboButtonWidth = 210.0f;
						if (ImGui::Button(comboLabel.c_str(), ImVec2(comboButtonWidth, buttonHeight))) {
							openComboReassignPrompt();
						}
						if (ImGui::IsItemHovered()) {
							ImGui::SetTooltip(UI_TIP_REASSIGN_COMBO);
						}
						ImGui::PopStyleColor();
					}
					else {
						ImVec4 keyNameColor;
						bool anyPressed = false;
						bool anyToggle = false;
						bool anyToggleEnabled = false;
						std::vector<std::string> groupNames;
						for (const LuaKeyState::TrackedKeyInfo& member : row.groupMembers) {
							USHORT memberDisplayVKey = ResolveDisplayVKey(member);
							if (RawInput::IsKeyHeldReal(memberDisplayVKey)) {
								anyPressed = true;
							}
							if (member.hasToggleState) {
								anyToggle = true;
								anyToggleEnabled = member.toggleEnabled;
							}
							std::string memberName = NameForVKey(memberDisplayVKey);
							if (std::find(groupNames.begin(), groupNames.end(), memberName) == groupNames.end()) {
								groupNames.push_back(memberName);
							}
						}
						if (anyToggle) {
							keyNameColor = anyToggleEnabled ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
						}
						else {
							keyNameColor = anyPressed ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
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
							ModKeyReadOnlyInfo seededInfo = captureIsCombo
								? ComputeComboModKeyReadOnlyInfo(modKeyCaptureScriptName, modKeyCaptureFunctionName)
								: ComputeModKeyReadOnlyInfo(modKeyCaptureScriptName, modKeyCaptureFunctionName);
							capturedToggleMode = false;
							capturedLongPressMode = seededInfo.anyLongPress;
							capturedHoldSeconds = (float)seededInfo.longPressSeconds;
							capturedInstantMode = seededInfo.anyInstant || !seededInfo.anyLongPress;
							capturedInstantTriggerType = seededInfo.instantType;
							capturedRepeatAccelMult = ModKeyBindings::GetTriggerConfig(modKeyCaptureScriptName, modKeyCaptureFunctionName).repeatAccelMult;
							capturedInstantUserSet = false;
							requestCaptureFocus = true;
							showCapturePrompt = true;
						};

						std::string groupLabel;
						for (size_t gi = 0; gi < groupNames.size(); gi++) {
							if (gi) groupLabel += UI_LBL_KEY_GROUP_SEPARATOR;
							groupLabel += groupNames[gi];
						}
						for (const LuaKeyState::TrackedComboKeyInfo& mergedCombo : row.mergedCombos) {
							groupLabel += "\n";
							groupLabel += ComboKeysDisplayName(mergedCombo.activeKeys);
						}
						float groupButtonWidth = ImGui::CalcTextSize(groupLabel.c_str()).x + 24.0f;
						if (groupButtonWidth < 130.0f) groupButtonWidth = 130.0f;
						if (row.mergedCombos.empty() && groupButtonWidth > 210.0f) groupButtonWidth = 210.0f;
						float groupButtonHeight = buttonHeight + (float)row.mergedCombos.size() * (ImGui::GetTextLineHeight() + 2.0f);

						ImGui::PushStyleColor(ImGuiCol_Text, keyNameColor);
						if (row.info.hasDescription) {
							if (ImGui::Button(groupLabel.c_str(), ImVec2(groupButtonWidth, groupButtonHeight))) {
								openReassignPrompt();
							}
							if (ImGui::IsItemHovered()) {
								ImGui::SetTooltip(UI_TIP_REASSIGN_KEY);
							}
						}
						else {
							ImGui::BeginDisabled();
							ImGui::Button(groupLabel.c_str(), ImVec2(groupButtonWidth, groupButtonHeight));
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
				ImGui::PopStyleColor();

				if (resetConfirmPopupRequested) {
					ImGui::OpenPopup(UI_POPUP_RESET_MOD_KEY);
					resetConfirmPopupRequested = false;
				}
				ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
				bool resetConfirmOpen = ImGui::BeginPopupModal(UI_POPUP_RESET_MOD_KEY, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
				if (resetConfirmOpen) {
					ImGui::Text(UI_FMT_RESET_CONFIRM, pendingResetScriptName.c_str(), pendingResetFunctionName.c_str());
					ImGui::TextDisabled(UI_TXT_CLEARS_REASSIGNMENT);
					ImGui::Spacing();
					if (ImGui::Button(UI_BTN_YES, ImVec2(80, 0))) {
						ModKeyBindings::SetOverride(pendingResetScriptName, pendingResetFunctionName, "");
						for (auto& b : bindings) {
							if (!b.isInject || !b.scriptDescribed || b.injectScriptName != pendingResetScriptName || b.injectFunctionName != pendingResetFunctionName) continue;
							b.isInstant = false;
							b.instantTriggerType = 0;
							b.holdSeconds = 0.0f;
							b.repeatAccelMult = 1.0f;
						}
						for (auto& b : bindings) {
							if (!b.isInject || !b.scriptDescribed || b.injectScriptName != pendingResetScriptName || b.injectFunctionName != pendingResetFunctionName) continue;
							USHORT bindNative = b.nativeVKey;
							if (bindNative == 0) {
								std::string storedNative = ModKeyBindings::GetNativeKey(b.injectScriptName, b.injectFunctionName);
								int resolvedNative = storedNative.empty() ? -1 : ResolveKeyNameForSlot(storedNative, b.vKey);
								if (resolvedNative > 0 && SlotOfVKey((USHORT)resolvedNative) == SlotOfVKey(b.vKey)) bindNative = (USHORT)resolvedNative;
							}
							if (bindNative != 0 && bindNative != b.vKey) {
								RemoveDispatcherIfUnused(b.vKey);
								b.vKey = bindNative;
								b.keyName = NameForVKey(bindNative);
								EnsureDispatcherRegistered(b.vKey);
							}
						}
						MarkDisplayCacheDirty();
						for (const auto& info : LuaKeyState::GetTrackedKeyInfo()) {
							if (info.scriptName != pendingResetScriptName || info.functionName != pendingResetFunctionName) {
								continue;
							}
							USHORT resetNativeVKey = 0;
							{
								std::string storedNative = ModKeyBindings::GetNativeKey(pendingResetScriptName, pendingResetFunctionName);
								int resolvedNative = storedNative.empty() ? -1 : ResolveKeyNameForSlot(storedNative, info.vKey);
								if (resolvedNative > 0 && SlotOfVKey((USHORT)resolvedNative) == SlotOfVKey(info.vKey)) resetNativeVKey = (USHORT)resolvedNative;
							}
							if (resetNativeVKey == 0) {
								for (const auto& b : bindings) {
									if (!b.isInject || !b.scriptDescribed || b.injectScriptName != pendingResetScriptName || b.injectFunctionName != pendingResetFunctionName) continue;
									if (SlotOfVKey(b.vKey) == SlotOfVKey(info.vKey) && b.nativeVKey != 0) { resetNativeVKey = b.nativeVKey; break; }
								}
							}
							if (resetNativeVKey == 0) resetNativeVKey = LuaKeyState::FindRedirectSource(info.vKey);
							if (resetNativeVKey != 0 && resetNativeVKey != info.vKey) {
								LuaKeyState::ReassignBinding(info.vKey, resetNativeVKey, pendingResetScriptName, pendingResetFunctionName);
							}
						}
						for (const auto& cinfo : LuaKeyState::GetTrackedComboKeyInfo()) {
							if (cinfo.scriptName == pendingResetScriptName && cinfo.functionName == pendingResetFunctionName) {
								LuaKeyState::ClearComboRedirect(cinfo.nativeKeys);
								break;
							}
						}
						ModKeyBindings::SetDisabled(pendingResetScriptName, pendingResetFunctionName, false);
						LogActivity("KeyBindMenu: Reset mod key to default: " + pendingResetScriptName + " [" + pendingResetFunctionName + "]");
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
				ImGui::PopStyleColor();
			}
			float bottomControlPanelY = ImGui::GetWindowHeight() - paddingY - 35.0f; 
			ImGui::SetCursorPosY(bottomControlPanelY);
			
			static std::chrono::steady_clock::time_point clearAllHoldStart;
			static bool clearAllHoldActive = false;
			static bool clearAllConfirmPending = false;
			static bool clearAllConfirmPopupRequested = false;

			bool anyTrackedModKeys = false;
			for (const auto& info : LuaKeyState::GetTrackedKeyInfo()) {
				if (info.hasDescription) { anyTrackedModKeys = true; break; }
			}
			if (!anyTrackedModKeys) {
				for (const auto& c : LuaKeyState::GetTrackedComboKeyInfo()) { (void)c; anyTrackedModKeys = true; break; }
			}
			bool clearAllDisabled = bindings.empty() && !anyTrackedModKeys;

			if (clearAllDisabled) ImGui::BeginDisabled();
			bool clearAllClicked = ImGui::Button(UI_BTN_CLEAR_ALL_HOTKEYS, ImVec2(145, 24));
			ImVec2 clearAllBtnMin = ImGui::GetItemRectMin();
			ImVec2 clearAllBtnMax = ImGui::GetItemRectMax();
			if (ImGui::IsItemActive()) {
				if (!clearAllHoldActive) {
					clearAllHoldActive = true;
					clearAllHoldStart = std::chrono::steady_clock::now();
				} else if (!clearAllConfirmPending) {
					double heldSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - clearAllHoldStart).count();
					float holdProgress = (float)((std::min)(1.0, heldSeconds / kRemoveHoldSeconds));
					float barHeight = 3.0f;
					ImVec2 barMin(clearAllBtnMin.x, clearAllBtnMax.y - barHeight);
					ImVec2 barMax(clearAllBtnMin.x + (clearAllBtnMax.x - clearAllBtnMin.x) * holdProgress, clearAllBtnMax.y);
					ImGui::GetWindowDrawList()->AddRectFilled(barMin, barMax, IM_COL32(255, 70, 70, 255));
					if (heldSeconds >= kRemoveHoldSeconds) {
						clearAllConfirmPending = true;
						clearAllConfirmPopupRequested = true;
					}
				}
			} else {
				clearAllHoldActive = false;
			}

			if (clearAllClicked && !clearAllConfirmPending) {
				DisableAllBindingsAndModKeys();
			}

			if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
				ImGui::SetTooltip("%s", UI_TIP_CLICK_HOLD_CLEAR_ALL);
			}
			if (clearAllDisabled) ImGui::EndDisabled();

			if (clearAllConfirmPopupRequested) {
				ImGui::OpenPopup(UI_POPUP_CLEAR_ALL_CONFIRM);
				clearAllConfirmPopupRequested = false;
			}
			ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			bool clearAllConfirmOpen = ImGui::BeginPopupModal(UI_POPUP_CLEAR_ALL_CONFIRM, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
			if (clearAllConfirmOpen) {
				ImGui::Text("%s", UI_TXT_CLEAR_ALL_CONFIRM);
				ImGui::TextDisabled(UI_TXT_CANNOT_BE_UNDONE);
				ImGui::Spacing();
				if (ImGui::Button(UI_BTN_YES, ImVec2(80, 0))) {
					ResetAndRemoveAllBindingsAndModKeys();
					clearAllConfirmPending = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(UI_BTN_NO, ImVec2(80, 0))) {
					clearAllConfirmPending = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			ImGui::PopStyleColor();
			
			ImGui::SameLine(ImGui::GetContentRegionMax().x - 165.0f);
			if (ImGui::Button(UI_BTN_ADD_NEW_BINDING, ImVec2(165, 24))) {
				editingBindingIndex = -1;
				captureIsCombo = false;
				captureIsInject = false;
				capturedInjectLineStart = 1;
				capturedInjectLineEnd = 1;
				ResetComboCaptureState();
				capturedVKey = 0;
				capturedInstantMode = false;
				capturedInstantTriggerType = 0;
				capturedInstantUserSet = false;
				isAssigningMenuToggleKey = false; isAssigningModKey = false; showCapturePrompt = true;
				requestCaptureFocus = true;
				LogActivity(LOG_KEY_ASSIGNMENT_BINDING_PROMPT_OPENED);
			}
			ImGui::End();

			if (showCapturePrompt) DrawKeyCapturePrompt();
		}
	}
}
