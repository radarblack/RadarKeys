#include "LuaKeyState.h"
#include "RawInput.h"
#include "ModKeyBindings.h"
#include "KeyBindMenu.h"
#include "spdlog/spdlog.h"
#include <chrono>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace RadarKeys {
	namespace LuaKeyState {
		using clock = std::chrono::steady_clock;
		namespace {
			std::recursive_mutex g_keyStateMutex;
		}
		using KeyStateLock = std::lock_guard<std::recursive_mutex>;
		static const char* LOG_LUAKEYSTATE_ENSURETRACKED_NOW_TRACKING_VKEY_FMT = "LuaKeyState: EnsureTracked: now tracking vKey:{}";
		static const char* LOG_LUAKEYSTATE_RETIREIFUNDESCRIBED_RELEASED_VKEY_FMT = "LuaKeyState: RetireIfUndescribed: released vKey:{}";

		constexpr double kHoldTimeSeconds = 0.9;
		constexpr double kRepeatRateSeconds = 0.85;
		constexpr double kIncrementMultIncrementMult = 1.5;
		constexpr double kMaxIncrementMult = 50.0;
		constexpr int kMaxQueuedEdges = 8;
		constexpr double kDescriptionStaleSeconds = 5.0;

		struct KeyDescription {
			bool hasToggleState = false;
			bool toggleEnabled = false;
			clock::time_point lastTouched = clock::now();
			bool usesOnPress = false;
			bool usesHoldTime = false;
			bool usesRepeat = false;
			bool usesOnRelease = false;
			double lastHoldSeconds = 0.0;
			std::string scriptName;
			std::string functionName;
		};

		struct KeyPollState {
			bool registered = false;
			bool isPressed = false;
			int downEdgePending = 0;
			int upEdgePending = 0;
			int physicalDownEdgePending = 0;
			int physicalUpEdgePending = 0;
			bool heldStartSet = false;
			bool onHoldStartSet = false;
			bool repeatStartSet = false;
			bool pendingUsesOnPress = false;
			bool pendingUsesHoldTime = false;
			bool pendingUsesRepeat = false;
			bool pendingUsesOnRelease = false;
			double pendingLastHoldSeconds = 0.0;
			double currentIncrementMult = 1.0;
			double configuredIncrementMult = 1.0;
			clock::time_point heldStart{};
			clock::time_point onHoldStart{};
			clock::time_point repeatStart{};
			std::vector<KeyDescription> descriptions;
			RawInput::ActionHandle actionHandle = 0;
			clock::time_point lastTrackedUse{};
		};

		KeyPollState states[RawInput::kMaxVKey];
		USHORT redirectTarget[RawInput::kMaxVKey] = {};

		struct ComboPollState {
			bool active = false;
			bool holdStartSet = false;
			bool repeatStartSet = false;
			bool pendingUsesOnPress = false;
			bool pendingUsesOnRelease = false;
			bool pendingUsesHoldTime = false;
			bool pendingUsesRepeat = false;
			double pendingLastHoldSeconds = 0.0;
			clock::time_point pressTime{};
			clock::time_point repeatStart{};
			double currentIncrementMult = 1.0;
			double configuredIncrementMult = 1.0;
		};
		std::map<std::string, ComboPollState> comboStates;
		bool ValidVKey(USHORT vKey) {
			return vKey < RawInput::kMaxVKey;
		}

		bool suppressed[RawInput::kMaxVKey] = {};
		std::map<USHORT, double> g_holdSecondsOverrides;
		bool IsSuppressed(USHORT vKey) {
			return ValidVKey(vKey) && suppressed[vKey];
		}

		void SetSuppressedVKeys(const std::vector<USHORT>& vKeys) {
			KeyStateLock lock(g_keyStateMutex);
			for (int i = 0; i < RawInput::kMaxVKey; ++i) {
				suppressed[i] = false;
			}
			for (USHORT vKey : vKeys) {
				if (ValidVKey(vKey)) {
					suppressed[vKey] = true;
				}
			}
		}

		bool disabledVKey[RawInput::kMaxVKey] = {};

		bool IsDisabledVKey(USHORT vKey) {
			return ValidVKey(vKey) && disabledVKey[vKey];
		}

		void SetHoldSecondsOverrides(const std::map<USHORT, double>& overrides) {
			KeyStateLock lock(g_keyStateMutex);
			g_holdSecondsOverrides = overrides;
		}

		void SetDisabledVKeys(const std::vector<USHORT>& vKeys) {
			KeyStateLock lock(g_keyStateMutex);
			for (int i = 0; i < RawInput::kMaxVKey; ++i) {
				disabledVKey[i] = false;
			}
			for (USHORT vKey : vKeys) {
				if (ValidVKey(vKey)) {
					disabledVKey[vKey] = true;
				}
			}
		}

		USHORT ResolveActive(USHORT vKey) {
			if (!ValidVKey(vKey)) {
				return vKey;
			}
			USHORT current = vKey;
			for (int hops = 0; hops < 8; ++hops) {
				USHORT next = redirectTarget[current];
				if (next == 0 || next == current || !ValidVKey(next)) {
					break;
				}
				current = next;
			}
			return current;
		}

		void UpdateRedirectForIdentity(USHORT nativeVKey, const std::string& scriptName, const std::string& functionName) {
			if (!ValidVKey(nativeVKey)) {
				return;
			}
			USHORT desired = nativeVKey;
			std::string overrideName = ModKeyBindings::GetSlotOverride(scriptName, functionName, KeyBindMenu::SlotOfVKey(nativeVKey));
			if (!overrideName.empty()) {
				int resolved = KeyBindMenu::VKeyForName(overrideName);
				if (resolved > 0 && ValidVKey((USHORT)resolved) && KeyBindMenu::SlotOfVKey((USHORT)resolved) == KeyBindMenu::SlotOfVKey(nativeVKey)) {
					desired = (USHORT)resolved;
				}
			}
			redirectTarget[nativeVKey] = (desired == nativeVKey) ? 0 : desired;
		}

		void OnRawEvent(USHORT vKey, RawInput::BUTTONEVENT ev) {
			if (!ValidVKey(vKey)) {
				return;
			}
			KeyStateLock lock(g_keyStateMutex);
			KeyPollState& s = states[vKey];
			if (ev == RawInput::BUTTONEVENT::ONDOWN) {
				s.isPressed = true;
				if (s.downEdgePending < kMaxQueuedEdges) s.downEdgePending++;
				if (s.physicalDownEdgePending < kMaxQueuedEdges) s.physicalDownEdgePending++;
				clock::time_point now = clock::now();
				s.heldStart = now;
				s.heldStartSet = true;
				s.onHoldStart = now;
				s.onHoldStartSet = true;
				s.repeatStart = now;
				s.repeatStartSet = true;
				s.currentIncrementMult = 1.0;
			}
			else if (ev == RawInput::BUTTONEVENT::ONUP) {
				s.isPressed = false;
				if (s.upEdgePending < kMaxQueuedEdges) s.upEdgePending++;
				if (s.physicalUpEdgePending < kMaxQueuedEdges) s.physicalUpEdgePending++;
				s.heldStartSet = false;
				s.onHoldStartSet = false;
				s.repeatStartSet = false;
				s.currentIncrementMult = 1.0;
			}
		}

		void EnsureTracked(USHORT vKey) {
			if (!ValidVKey(vKey)) {
				return;
			}
			KeyPollState& s = states[vKey];
			s.lastTrackedUse = clock::now();
			if (s.registered) {
				return;
			}
			s.isPressed = RawInput::IsKeyHeldReal(vKey);
			if (s.isPressed) {
				clock::time_point now = clock::now();
				s.heldStart = now;
				s.heldStartSet = true;
				s.onHoldStart = now;
				s.onHoldStartSet = true;
				s.repeatStart = now;
				s.repeatStartSet = true;
			}
			s.actionHandle = RawInput::RegisterAction(vKey, [vKey](RawInput::BUTTONEVENT ev) { OnRawEvent(vKey, ev); });
			s.registered = true;
			spdlog::debug(LOG_LUAKEYSTATE_ENSURETRACKED_NOW_TRACKING_VKEY_FMT, vKey);
		}

		void RetireIfUndescribed(USHORT vKey) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return;
			}
			KeyPollState& s = states[vKey];
			if (!s.registered || !s.descriptions.empty()) {
				return;
			}
			if (s.isPressed || s.downEdgePending > 0 || s.upEdgePending > 0 ||
				s.physicalDownEdgePending > 0 || s.physicalUpEdgePending > 0) {
				return;
			}
			if (std::chrono::duration<double>(clock::now() - s.lastTrackedUse).count() < kDescriptionStaleSeconds) {
				return;
			}
			RawInput::UnRegisterAction(vKey, s.actionHandle);
			s = KeyPollState{};
			spdlog::debug(LOG_LUAKEYSTATE_RETIREIFUNDESCRIBED_RELEASED_VKEY_FMT, vKey);
		}

		bool ButtonDown(USHORT vKey) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return false;
			}
			vKey = ResolveActive(vKey);
			EnsureTracked(vKey);
			states[vKey].pendingUsesOnPress = true;
			if (IsSuppressed(vKey) || IsDisabledVKey(vKey) || showCapturePrompt) {
				return false;
			}
			return RawInput::IsKeyHeldReal(vKey);
		}

		bool PhysicalOnButtonDown(USHORT vKey) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return false;
			}
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			if (IsSuppressed(vKey)) {
				s.physicalDownEdgePending = 0;
				return false;
			}
			if (s.physicalDownEdgePending > 0) {
				s.physicalDownEdgePending--;
				return true;
			}
			return false;
		}

		bool OnButtonDown(USHORT vKey) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return false;
			}
			vKey = ResolveActive(vKey);
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			s.pendingUsesOnPress = true;
			if (IsSuppressed(vKey) || IsDisabledVKey(vKey) || showCapturePrompt) {
				s.downEdgePending = 0;
				return false;
			}
			if (s.downEdgePending > 0) {
				s.downEdgePending--;
				return true;
			}
			return false;
		}

		bool PhysicalOnButtonUp(USHORT vKey) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return false;
			}
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			if (IsSuppressed(vKey)) {
				s.physicalUpEdgePending = 0;
				return false;
			}
			if (s.physicalUpEdgePending > 0) {
				s.physicalUpEdgePending--;
				return true;
			}
			return false;
		}

		bool OnButtonUp(USHORT vKey) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return false;
			}
			vKey = ResolveActive(vKey);
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			s.pendingUsesOnRelease = true;
			if (IsSuppressed(vKey) || IsDisabledVKey(vKey) || showCapturePrompt) {
				s.upEdgePending = 0;
				return false;
			}
			if (s.upEdgePending > 0) {
				s.upEdgePending--;
				return true;
			}
			return false;
		}

		bool ButtonHeld(USHORT vKey, double holdSecondsOverride) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return false;
			}
			vKey = ResolveActive(vKey);
			EnsureTracked(vKey);
			double heldHoldTime = (holdSecondsOverride >= 0.0) ? holdSecondsOverride : kHoldTimeSeconds;
			states[vKey].pendingUsesHoldTime = true;
			states[vKey].pendingLastHoldSeconds = heldHoldTime;
			if (IsSuppressed(vKey) || IsDisabledVKey(vKey) || showCapturePrompt) {
				return false;
			}
			KeyPollState& s = states[vKey];
			if (s.isPressed && s.heldStartSet) {
				double elapsed = std::chrono::duration<double>(clock::now() - s.heldStart).count();
				return elapsed >= heldHoldTime;
			}
			return false;
		}

		bool PhysicalOnButtonHoldTime(USHORT vKey, double holdSecondsOverride) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return false;
			}
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			double holdTime = (holdSecondsOverride >= 0.0) ? holdSecondsOverride : kHoldTimeSeconds;
			if (IsSuppressed(vKey)) {
				return false;
			}
			if (s.isPressed && s.onHoldStartSet) {
				double elapsed = std::chrono::duration<double>(clock::now() - s.onHoldStart).count();
				if (elapsed >= holdTime) {
					s.onHoldStartSet = false;
					return true;
				}
			}
			return false;
		}

		bool OnButtonHoldTime(USHORT vKey, double holdSecondsOverride) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return false;
			}
			vKey = ResolveActive(vKey);
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			double holdTime = (holdSecondsOverride >= 0.0) ? holdSecondsOverride : kHoldTimeSeconds;
			auto holdOverrideIt = g_holdSecondsOverrides.find(vKey);
			if (holdOverrideIt != g_holdSecondsOverrides.end()) {
				holdTime = holdOverrideIt->second;
			}
			s.pendingUsesHoldTime = true;
			s.pendingLastHoldSeconds = holdTime;
			if (IsSuppressed(vKey) || IsDisabledVKey(vKey) || showCapturePrompt) {
				return false;
			}
			if (s.isPressed && s.onHoldStartSet) {
				double elapsed = std::chrono::duration<double>(clock::now() - s.onHoldStart).count();
				if (elapsed >= holdTime) {
					s.onHoldStartSet = false;
					return true;
				}
			}
			return false;
		}

		bool OnButtonRepeat(USHORT vKey) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return false;
			}
			vKey = ResolveActive(vKey);
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			s.pendingUsesRepeat = true;
			if (IsSuppressed(vKey) || IsDisabledVKey(vKey) || showCapturePrompt) {
				return false;
			}
			if (!s.isPressed) {
				s.currentIncrementMult = 1.0;
				return false;
			}
			if (s.repeatStartSet) {
				double elapsed = std::chrono::duration<double>(clock::now() - s.repeatStart).count();
				if (elapsed >= kRepeatRateSeconds / s.currentIncrementMult) {
					s.repeatStart = clock::now();
					s.currentIncrementMult *= kIncrementMultIncrementMult;
					if (s.currentIncrementMult > kMaxIncrementMult) {
						s.currentIncrementMult = kMaxIncrementMult;
					}
					return true;
				}
			}
			return false;
		}

		double GetRepeatMult(USHORT vKey) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return 1.0;
			}
			vKey = ResolveActive(vKey);
			if (IsSuppressed(vKey) || IsDisabledVKey(vKey) || showCapturePrompt) {
				return 1.0;
			}
			return states[vKey].currentIncrementMult;
		}

		void SetRepeatMult(USHORT vKey, double mult) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey) || !(mult > 0.0)) {
				return;
			}
			vKey = ResolveActive(vKey);
			if (mult > kMaxIncrementMult) {
				mult = kMaxIncrementMult;
			}
			if (mult < 1.0 / kMaxIncrementMult) {
				mult = 1.0 / kMaxIncrementMult;
			}
			states[vKey].currentIncrementMult = mult;
			states[vKey].configuredIncrementMult = mult;
		}

		std::string ComboStateKey(const std::vector<USHORT>& vKeys) {
			std::vector<USHORT> keys = vKeys;
			std::sort(keys.begin(), keys.end());
			keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
			std::string key;
			for (USHORT vKey : keys) {
				if (!key.empty()) key += ',';
				key += std::to_string(vKey);
			}
			return key;
		}

		bool ValidCombo(const std::vector<USHORT>& vKeys) {
			if (vKeys.size() < 2 || vKeys.size() > 3) return false;
			std::vector<USHORT> keys = vKeys;
			std::sort(keys.begin(), keys.end());
			return std::all_of(keys.begin(), keys.end(), ValidVKey) &&
				std::adjacent_find(keys.begin(), keys.end()) == keys.end();
		}

		std::set<std::string> disabledComboKeys;

		bool IsComboDisabled(const std::vector<USHORT>& vKeys) {
			return disabledComboKeys.count(ComboStateKey(vKeys)) > 0;
		}

		void SetDisabledCombos(const std::vector<std::vector<USHORT>>& combos) {
			KeyStateLock lock(g_keyStateMutex);
			disabledComboKeys.clear();
			for (const auto& combo : combos) {
				if (ValidCombo(combo)) {
					disabledComboKeys.insert(ComboStateKey(combo));
				}
			}
		}

		std::map<std::string, std::vector<USHORT>> comboRedirectTarget;
		std::vector<USHORT> ResolveActiveCombo(const std::vector<USHORT>& vKeys) {
			KeyStateLock lock(g_keyStateMutex);
			auto it = comboRedirectTarget.find(ComboStateKey(vKeys));
			if (it != comboRedirectTarget.end() && ValidCombo(it->second)) {
				return it->second;
			}
			return vKeys;
		}

		void UpdateRedirectForComboIdentity(const std::vector<USHORT>& nativeVKeys, const std::string& scriptName, const std::string& functionName) {
			std::string nativeKey = ComboStateKey(nativeVKeys);
			std::string overrideName = ModKeyBindings::GetOverride(scriptName, functionName);
			if (overrideName.empty()) {
				comboRedirectTarget.erase(nativeKey);
				return;
			}
			std::vector<USHORT> resolved = KeyBindMenu::ParseComboKeyNames(overrideName);
			if (resolved.empty() || !ValidCombo(resolved) || ComboStateKey(resolved) == nativeKey) {
				comboRedirectTarget.erase(nativeKey);
				return;
			}
			comboRedirectTarget[nativeKey] = resolved;
		}

		bool RawComboAllHeld(const std::vector<USHORT>& vKeys) {
			for (USHORT vKey : vKeys) {
				if (!RawInput::IsKeyHeldReal(vKey)) return false;
			}
			return true;
		}

		bool ComboButtonDown(const std::vector<USHORT>& vKeys) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidCombo(vKeys)) return false;
			std::vector<USHORT> active = ResolveActiveCombo(vKeys);
			if (!ValidCombo(active) || IsComboDisabled(active) || showCapturePrompt) return false;
			return RawComboAllHeld(active);
		}

		bool OnComboButtonDown(const std::vector<USHORT>& vKeys) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidCombo(vKeys)) return false;
			std::vector<USHORT> active = ResolveActiveCombo(vKeys);
			if (!ValidCombo(active)) return false;
			std::string stateKey = ComboStateKey(active);
			ComboPollState& state = comboStates[stateKey];
			state.pendingUsesOnPress = true;
			if (IsComboDisabled(active) || showCapturePrompt) return false;
			bool allHeld = RawComboAllHeld(active);
			if (allHeld && !state.active) {
				state.active = true;
				state.holdStartSet = true;
				state.repeatStartSet = true;
				state.pressTime = clock::now();
				state.repeatStart = state.pressTime;
				state.currentIncrementMult = 1.0;
				return true;
			}
			if (!allHeld) {
				state.active = false;
				state.holdStartSet = false;
				state.repeatStartSet = false;
				state.currentIncrementMult = 1.0;
			}
			return false;
		}

		bool OnComboButtonUp(const std::vector<USHORT>& vKeys) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidCombo(vKeys)) return false;
			std::vector<USHORT> active = ResolveActiveCombo(vKeys);
			if (!ValidCombo(active)) return false;
			std::string stateKey = ComboStateKey(active);
			ComboPollState& state = comboStates[stateKey];
			state.pendingUsesOnRelease = true;
			if (IsComboDisabled(active) || showCapturePrompt) return false;
			bool allHeld = RawComboAllHeld(active);
			if (allHeld && !state.active) {
				state.active = true;
				state.pressTime = clock::now();
				state.repeatStart = state.pressTime;
				state.holdStartSet = true;
				state.repeatStartSet = true;
				state.currentIncrementMult = 1.0;
			}
			if (state.active && !allHeld) {
				state.active = false;
				state.holdStartSet = false;
				state.repeatStartSet = false;
				state.currentIncrementMult = 1.0;
				return true;
			}
			return false;
		}

		bool ComboButtonHeld(const std::vector<USHORT>& vKeys, double holdSecondsOverride) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidCombo(vKeys)) return false;
			std::vector<USHORT> active = ResolveActiveCombo(vKeys);
			if (!ValidCombo(active)) return false;
			std::string stateKey = ComboStateKey(active);
			ComboPollState& state = comboStates[stateKey];
			state.pendingUsesHoldTime = true;
			if (IsComboDisabled(active) || showCapturePrompt || !RawComboAllHeld(active)) return false;
			if (!state.active) {
				state.active = true;
				state.pressTime = clock::now();
				state.holdStartSet = true;
			}
			double holdTime = (holdSecondsOverride >= 0.0) ? holdSecondsOverride : kHoldTimeSeconds; // L46: explicit 0 = immediate
			state.pendingLastHoldSeconds = holdTime;
			return state.holdStartSet && std::chrono::duration<double>(clock::now() - state.pressTime).count() >= holdTime;
		}

		bool OnComboButtonHoldTime(const std::vector<USHORT>& vKeys, double holdSecondsOverride) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidCombo(vKeys)) return false;
			std::vector<USHORT> active = ResolveActiveCombo(vKeys);
			if (!ValidCombo(active)) return false;
			std::string stateKey = ComboStateKey(active);
			ComboPollState& state = comboStates[stateKey];
			state.pendingUsesHoldTime = true;
			if (IsComboDisabled(active) || showCapturePrompt || !RawComboAllHeld(active)) return false;
			if (!state.active) {
				state.active = true;
				state.pressTime = clock::now();
				state.holdStartSet = true;
			}
			double holdTime = (holdSecondsOverride >= 0.0) ? holdSecondsOverride : kHoldTimeSeconds; // L46: explicit 0 = immediate
			state.pendingLastHoldSeconds = holdTime;
			if (state.holdStartSet && std::chrono::duration<double>(clock::now() - state.pressTime).count() >= holdTime) {
				state.holdStartSet = false;
				return true;
			}
			return false;
		}

		bool OnComboButtonRepeat(const std::vector<USHORT>& vKeys) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidCombo(vKeys)) return false;
			std::vector<USHORT> active = ResolveActiveCombo(vKeys);
			if (!ValidCombo(active)) return false;
			std::string stateKey = ComboStateKey(active);
			ComboPollState& state = comboStates[stateKey];
			state.pendingUsesRepeat = true;
			if (IsComboDisabled(active) || showCapturePrompt || !RawComboAllHeld(active)) return false;
			if (!state.active) {
				state.active = true;
				state.pressTime = clock::now();
				state.repeatStart = state.pressTime;
				state.holdStartSet = true;
				state.repeatStartSet = true;
				state.currentIncrementMult = 1.0;
			}
			if (state.repeatStartSet && std::chrono::duration<double>(clock::now() - state.repeatStart).count() >= kRepeatRateSeconds / state.currentIncrementMult) {
				state.repeatStart = clock::now();
				state.currentIncrementMult *= kIncrementMultIncrementMult;
				if (state.currentIncrementMult > kMaxIncrementMult) state.currentIncrementMult = kMaxIncrementMult;
				return true;
			}
			return false;
		}

		double GetComboRepeatMult(const std::vector<USHORT>& vKeys) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidCombo(vKeys)) return 1.0;
			std::vector<USHORT> active = ResolveActiveCombo(vKeys);
			if (!ValidCombo(active) || IsComboDisabled(active) || showCapturePrompt) return 1.0;
			auto it = comboStates.find(ComboStateKey(active));
			return it == comboStates.end() ? 1.0 : it->second.currentIncrementMult;
		}

		double GetRepeatIntervalSeconds(USHORT vKey) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return kRepeatRateSeconds;
			}
			vKey = ResolveActive(vKey);
			double displayMult = states[vKey].configuredIncrementMult != 1.0 ? states[vKey].configuredIncrementMult : states[vKey].currentIncrementMult;
			return kRepeatRateSeconds / displayMult;
		}

		double GetComboRepeatIntervalSeconds(const std::vector<USHORT>& vKeys) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidCombo(vKeys)) return kRepeatRateSeconds;
			std::vector<USHORT> active = ResolveActiveCombo(vKeys);
			if (!ValidCombo(active)) return kRepeatRateSeconds;
			auto it = comboStates.find(ComboStateKey(active));
			if (it == comboStates.end()) return kRepeatRateSeconds;
			double displayMult = it->second.configuredIncrementMult != 1.0 ? it->second.configuredIncrementMult : it->second.currentIncrementMult;
			return kRepeatRateSeconds / displayMult;
		}

		void ResetComboRepeat(const std::vector<USHORT>& vKeys) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidCombo(vKeys)) return;
			std::vector<USHORT> active = ResolveActiveCombo(vKeys);
			if (!ValidCombo(active)) return;
			ComboPollState& state = comboStates[ComboStateKey(active)];
			state.holdStartSet = false;
			state.repeatStartSet = false;
			state.currentIncrementMult = 1.0;
		}

		void SetComboRepeatMult(const std::vector<USHORT>& vKeys, double mult) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidCombo(vKeys) || !(mult > 0.0)) return;
			std::vector<USHORT> active = ResolveActiveCombo(vKeys);
			if (!ValidCombo(active)) return;
			if (mult > kMaxIncrementMult) mult = kMaxIncrementMult;
			if (mult < 1.0 / kMaxIncrementMult) mult = 1.0 / kMaxIncrementMult;
			comboStates[ComboStateKey(active)].currentIncrementMult = mult;
			comboStates[ComboStateKey(active)].configuredIncrementMult = mult;
		}

		struct ComboKeyDescription {
			bool hasToggleState = false;
			bool toggleEnabled = false;
			clock::time_point lastTouched = clock::now();
			bool usesOnPress = false;
			bool usesHoldTime = false;
			bool usesRepeat = false;
			bool usesOnRelease = false;
			double lastHoldSeconds = 0.0;
			std::vector<USHORT> nativeKeys;
			std::string scriptName;
			std::string functionName;
		};
		std::map<std::string, ComboKeyDescription> comboDescriptions;

		void DescribeComboKey(const std::vector<USHORT>& vKeys, const std::string& scriptName, const std::string& functionName, const std::string& toggleState) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidCombo(vKeys)) {
				return;
			}
			UpdateRedirectForComboIdentity(vKeys, scriptName, functionName);
			std::vector<USHORT> activeKeys = ResolveActiveCombo(vKeys);
			std::string stateKey = ComboStateKey(activeKeys);
			ComboPollState& state = comboStates[stateKey];
			bool obsOnPress = state.pendingUsesOnPress;
			bool obsHoldTime = state.pendingUsesHoldTime;
			bool obsRepeat = state.pendingUsesRepeat;
			bool obsOnRelease = state.pendingUsesOnRelease;
			double obsHoldSeconds = state.pendingLastHoldSeconds;
			state.pendingUsesOnPress = false;
			state.pendingUsesHoldTime = false;
			state.pendingUsesRepeat = false;
			state.pendingUsesOnRelease = false;
			state.pendingLastHoldSeconds = 0.0;
			std::string identity = scriptName + "\x1f" + functionName;
			auto existingDesc = comboDescriptions.find(identity);
			bool keysChanged = existingDesc != comboDescriptions.end() && existingDesc->second.nativeKeys != vKeys;
			if (keysChanged) {
				obsOnPress = obsOnPress || existingDesc->second.usesOnPress;
				obsHoldTime = obsHoldTime || existingDesc->second.usesHoldTime;
				obsRepeat = obsRepeat || existingDesc->second.usesRepeat;
				obsOnRelease = obsOnRelease || existingDesc->second.usesOnRelease;
				if (obsHoldSeconds == 0.0) obsHoldSeconds = existingDesc->second.lastHoldSeconds;
				comboRedirectTarget.erase(ComboStateKey(existingDesc->second.nativeKeys));
			}
			ComboKeyDescription& d = comboDescriptions[identity];
			d.nativeKeys = vKeys;
			d.scriptName = scriptName;
			d.functionName = functionName;
			d.hasToggleState = (toggleState == "on" || toggleState == "off");
			d.toggleEnabled = (toggleState == "on");
			d.usesOnPress = obsOnPress;
			d.usesHoldTime = obsHoldTime;
			d.usesRepeat = obsRepeat;
			d.usesOnRelease = obsOnRelease;
			d.lastHoldSeconds = obsHoldSeconds;
			d.lastTouched = clock::now();
		}

		void SweepStaleComboDescriptions() {
			KeyStateLock lock(g_keyStateMutex);
			const clock::time_point now = clock::now();
			for (auto it = comboDescriptions.begin(); it != comboDescriptions.end(); ) {
				const double ageSeconds = std::chrono::duration<double>(now - it->second.lastTouched).count();
				if (ageSeconds > kDescriptionStaleSeconds) {
				std::string eraseActiveKey = ComboStateKey(ResolveActiveCombo(it->second.nativeKeys));
				auto eraseState = comboStates.find(eraseActiveKey);
				if (eraseState != comboStates.end()) {
					eraseState->second.pendingUsesOnPress = false;
					eraseState->second.pendingUsesHoldTime = false;
					eraseState->second.pendingUsesRepeat = false;
					eraseState->second.pendingUsesOnRelease = false;
				}
				comboRedirectTarget.erase(ComboStateKey(it->second.nativeKeys));
				it = comboDescriptions.erase(it);
				} else {
					++it;
				}
			}
		}

		std::vector<TrackedComboKeyInfo> GetTrackedComboKeyInfo() {
			KeyStateLock lock(g_keyStateMutex);
			std::vector<TrackedComboKeyInfo> result;
			for (const auto& entry : comboDescriptions) {
				const ComboKeyDescription& d = entry.second;
				TrackedComboKeyInfo info;
				info.nativeKeys = d.nativeKeys;
				info.activeKeys = ResolveActiveCombo(d.nativeKeys);
				info.isPressed = ValidCombo(info.activeKeys) && RawComboAllHeld(info.activeKeys);
				info.scriptName = d.scriptName;
				info.functionName = d.functionName;
				info.hasToggleState = d.hasToggleState;
				info.toggleEnabled = d.toggleEnabled;
				info.usesOnPress = d.usesOnPress;
				info.usesHoldTime = d.usesHoldTime;
				info.usesRepeat = d.usesRepeat;
				info.usesOnRelease = d.usesOnRelease;
				info.lastHoldSeconds = d.lastHoldSeconds;
				result.push_back(std::move(info));
			}
			return result;
		}

		void ResetRepeat(USHORT vKey) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return;
			}
			vKey = ResolveActive(vKey);
			KeyPollState& s = states[vKey];
			s.heldStartSet = false;
			s.onHoldStartSet = false;
			s.repeatStartSet = false;
			s.currentIncrementMult = 1.0;
		}

		void DescribeKey(USHORT vKey, const std::string& scriptName, const std::string& functionName, const std::string& toggleState) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(vKey)) {
				return;
			}
			UpdateRedirectForIdentity(vKey, scriptName, functionName);
			vKey = ResolveActive(vKey);
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			bool hasToggleState = (toggleState == "on" || toggleState == "off");
			bool toggleEnabled = (toggleState == "on");
			bool obsOnPress = s.pendingUsesOnPress;
			bool obsHoldTime = s.pendingUsesHoldTime;
			double obsHoldSeconds = s.pendingLastHoldSeconds;
			bool obsRepeat = s.pendingUsesRepeat;
			bool obsOnRelease = s.pendingUsesOnRelease;
			s.pendingUsesOnPress = false;
			s.pendingUsesHoldTime = false;
			s.pendingLastHoldSeconds = 0.0;
			s.pendingUsesRepeat = false;
			s.pendingUsesOnRelease = false;

			for (KeyDescription& d : s.descriptions) {
				if (d.scriptName == scriptName && d.functionName == functionName) {
					d.hasToggleState = hasToggleState;
					d.toggleEnabled = toggleEnabled;
					d.lastTouched = clock::now();
					d.usesOnPress = obsOnPress;
					d.usesHoldTime = obsHoldTime;
					d.lastHoldSeconds = obsHoldSeconds;
					d.usesRepeat = obsRepeat;
					d.usesOnRelease = obsOnRelease;
					return;
				}
			}

			KeyDescription d;
			d.scriptName = scriptName;
			d.functionName = functionName;
			d.hasToggleState = hasToggleState;
			d.toggleEnabled = toggleEnabled;
			d.lastTouched = clock::now();
			d.usesOnPress = obsOnPress;
			d.usesHoldTime = obsHoldTime;
			d.lastHoldSeconds = obsHoldSeconds;
			d.usesRepeat = obsRepeat;
			d.usesOnRelease = obsOnRelease;
			s.descriptions.push_back(std::move(d));
		}

		void SweepStaleDescriptions() {
			KeyStateLock lock(g_keyStateMutex);
			const clock::time_point now = clock::now();
			for (int vKeyInt = 0; vKeyInt < RawInput::kMaxVKey; ++vKeyInt) {
				std::vector<KeyDescription>& descs = states[vKeyInt].descriptions;
				if (!descs.empty()) {
					descs.erase(
						std::remove_if(descs.begin(), descs.end(), [&](const KeyDescription& d) {
							return std::chrono::duration<double>(now - d.lastTouched).count() > kDescriptionStaleSeconds;
						}),
						descs.end()
					);
				}
				RetireIfUndescribed(static_cast<USHORT>(vKeyInt));
			}
		}

		void ReassignBinding(USHORT oldVKey, USHORT newVKey, const std::string& scriptName, const std::string& functionName) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(oldVKey) || !ValidVKey(newVKey) || oldVKey == newVKey) {
				return;
			}
			KeyPollState& oldState = states[oldVKey];
			auto it = std::find_if(oldState.descriptions.begin(), oldState.descriptions.end(),
				[&](const KeyDescription& d) {
					return d.scriptName == scriptName && d.functionName == functionName;
				});

			if (it != oldState.descriptions.end()) {
				EnsureTracked(newVKey);
				KeyPollState& newState = states[newVKey];
				KeyDescription movedDesc = *it;
				movedDesc.lastTouched = clock::now();
				newState.descriptions.push_back(std::move(movedDesc));
				oldState.descriptions.erase(it);
			oldState.downEdgePending = 0;
			oldState.upEdgePending = 0;
			oldState.physicalDownEdgePending = 0;
			oldState.physicalUpEdgePending = 0;
				RetireIfUndescribed(oldVKey);
			}
		}

		std::vector<TrackedKeyInfo> GetTrackedKeyInfo() {
			KeyStateLock lock(g_keyStateMutex);
			std::vector<TrackedKeyInfo> result;
			for (int vKeyInt = 0; vKeyInt < RawInput::kMaxVKey; ++vKeyInt) {
				const KeyPollState& s = states[vKeyInt];
				if (!s.registered) {
					continue;
				}
				USHORT vKey = static_cast<USHORT>(vKeyInt);
				bool isPressed = RawInput::IsKeyHeldReal(vKey);
				if (s.descriptions.empty()) {
					continue;
				}

				for (const KeyDescription& d : s.descriptions) {
					TrackedKeyInfo info;
					info.vKey = vKey;
					info.isPressed = isPressed;
					info.hasDescription = true;
					info.scriptName = d.scriptName;
					info.functionName = d.functionName;
					info.hasToggleState = d.hasToggleState;
					info.toggleEnabled = d.toggleEnabled;
					info.isConflicted = false;
					unsigned dMask = 0;
					if (d.usesOnPress) dMask |= 1u << 0;
					if (d.usesOnRelease) dMask |= 1u << 1;
					if (d.usesHoldTime) dMask |= 1u << 2;
					if (d.usesRepeat) dMask |= 1u << 3;
					for (const KeyDescription& other : s.descriptions) {
						if (&other == &d) continue;
						unsigned otherMask = 0;
						if (other.usesOnPress) otherMask |= 1u << 0;
						if (other.usesOnRelease) otherMask |= 1u << 1;
						if (other.usesHoldTime) otherMask |= 1u << 2;
						if (other.usesRepeat) otherMask |= 1u << 3;
						if (dMask != 0 && otherMask != 0 && (dMask & otherMask) != 0) {
							info.isConflicted = true;
							break;
						}
					}
					info.usesOnPress = d.usesOnPress;
					info.usesHoldTime = d.usesHoldTime;
					info.lastHoldSeconds = d.lastHoldSeconds;
					info.usesRepeat = d.usesRepeat;
					info.usesOnRelease = d.usesOnRelease;
					result.push_back(std::move(info));
				}
			}
			return result;
		}

		void OnFocusLost() {
			KeyStateLock lock(g_keyStateMutex);
			for (int i = 0; i < RawInput::kMaxVKey; ++i) {
				KeyPollState& s = states[i];
				s.isPressed = false;
				s.downEdgePending = 0;
				s.upEdgePending = 0;
				s.physicalDownEdgePending = 0;
				s.physicalUpEdgePending = 0;
				s.heldStartSet = false;
				s.onHoldStartSet = false;
				s.repeatStartSet = false;
				s.currentIncrementMult = 1.0;
			}
			for (auto& entry : comboStates) {
				entry.second.active = false;
				entry.second.holdStartSet = false;
				entry.second.repeatStartSet = false;
				entry.second.currentIncrementMult = 1.0;
			}
		}


		USHORT FindRedirectSource(USHORT activeVKey) {
			KeyStateLock lock(g_keyStateMutex);
			if (!ValidVKey(activeVKey)) return 0;
			for (int i = 1; i < RawInput::kMaxVKey; ++i) {
				if (redirectTarget[i] == activeVKey) return (USHORT)i;
			}
			return activeVKey;
		}

		void ClearComboRedirect(const std::vector<USHORT>& nativeVKeys) {
			KeyStateLock lock(g_keyStateMutex);
			comboRedirectTarget.erase(ComboStateKey(nativeVKeys));
		}

	}
}
