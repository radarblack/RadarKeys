#include "LuaKeyState.h"
#include "RawInput.h"
#include "spdlog/spdlog.h"
#include <chrono>
#include <algorithm>

namespace RadarKeys {
	namespace LuaKeyState {
		using clock = std::chrono::steady_clock;

		constexpr double kHoldTimeSeconds = 0.9;
		constexpr double kRepeatRateSeconds = 0.85;
		constexpr double kIncrementMultIncrementMult = 1.5;
		constexpr double kMaxIncrementMult = 50.0;

		struct KeyDescription {
			std::string scriptName;
			std::string functionName;
			bool hasToggleState = false;
			bool toggleEnabled = false;
			bool touchedSinceSweep = true;
		};

		struct KeyPollState {
			bool registered = false;
			bool isPressed = false;
			bool downEdgePending = false;
			bool upEdgePending = false;

			bool heldStartSet = false;
			bool onHoldStartSet = false;
			bool repeatStartSet = false;

			clock::time_point heldStart{};
			clock::time_point onHoldStart{};
			clock::time_point repeatStart{};

			double currentIncrementMult = 1.0;
			std::vector<KeyDescription> descriptions;
		};

		KeyPollState states[256];

		bool ValidVKey(USHORT vKey) {
			return vKey < 256;
		}

		void OnRawEvent(USHORT vKey, RawInput::BUTTONEVENT ev) {
			if (!ValidVKey(vKey)) {
				return;
			}
			KeyPollState& s = states[vKey];
			if (ev == RawInput::BUTTONEVENT::ONDOWN) {
				s.isPressed = true;
				s.downEdgePending = true;
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
				s.upEdgePending = true;
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
			if (s.registered) {
				return;
			}
			s.isPressed = RawInput::IsKeyHeldReal(vKey);
			RawInput::RegisterAction(vKey, [vKey](RawInput::BUTTONEVENT ev) { OnRawEvent(vKey, ev); });
			s.registered = true;
			spdlog::debug("LuaKeyState EnsureTracked: now tracking vKey:{}", vKey);
		}

		bool ButtonDown(USHORT vKey) {
			if (!ValidVKey(vKey)) {
				return false;
			}
			EnsureTracked(vKey);
			return RawInput::IsKeyHeldReal(vKey);
		}

		bool OnButtonDown(USHORT vKey) {
			if (!ValidVKey(vKey)) {
				return false;
			}
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			if (s.downEdgePending) {
				s.downEdgePending = false;
				return true;
			}
			return false;
		}

		bool OnButtonUp(USHORT vKey) {
			if (!ValidVKey(vKey)) {
				return false;
			}
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			if (s.upEdgePending) {
				s.upEdgePending = false;
				return true;
			}
			return false;
		}

		bool ButtonHeld(USHORT vKey, double holdSecondsOverride) {
			if (!ValidVKey(vKey)) {
				return false;
			}
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			double holdTime = (holdSecondsOverride > 0.0) ? holdSecondsOverride : kHoldTimeSeconds;
			if (s.isPressed && s.heldStartSet) {
				double elapsed = std::chrono::duration<double>(clock::now() - s.heldStart).count();
				return elapsed > holdTime;
			}
			return false;
		}

		bool OnButtonHoldTime(USHORT vKey, double holdSecondsOverride) {
			if (!ValidVKey(vKey)) {
				return false;
			}
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			double holdTime = (holdSecondsOverride > 0.0) ? holdSecondsOverride : kHoldTimeSeconds;
			if (s.isPressed && s.onHoldStartSet) {
				double elapsed = std::chrono::duration<double>(clock::now() - s.onHoldStart).count();
				if (elapsed > holdTime) {
					s.onHoldStartSet = false;
					return true;
				}
			}
			return false;
		}

		bool OnButtonRepeat(USHORT vKey) {
			if (!ValidVKey(vKey)) {
				return false;
			}
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];
			if (!s.isPressed) {
				s.currentIncrementMult = 1.0;
				return false;
			}
			if (s.repeatStartSet) {
				double elapsed = std::chrono::duration<double>(clock::now() - s.repeatStart).count();
				if (elapsed > kRepeatRateSeconds) {
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
			if (!ValidVKey(vKey)) {
				return 1.0;
			}
			return states[vKey].currentIncrementMult;
		}

		void ResetRepeat(USHORT vKey) {
			if (!ValidVKey(vKey)) {
				return;
			}
			KeyPollState& s = states[vKey];
			s.heldStartSet = false;
			s.onHoldStartSet = false;
			s.repeatStartSet = false;
		}

		void DescribeKey(USHORT vKey, const std::string& scriptName, const std::string& functionName, const std::string& toggleState) {
			if (!ValidVKey(vKey)) {
				return;
			}
			EnsureTracked(vKey);
			KeyPollState& s = states[vKey];

			bool hasToggleState = (toggleState == "on" || toggleState == "off");
			bool toggleEnabled = (toggleState == "on");

			for (KeyDescription& d : s.descriptions) {
				if (d.scriptName == scriptName && d.functionName == functionName) {
					d.hasToggleState = hasToggleState;
					d.toggleEnabled = toggleEnabled;
					d.touchedSinceSweep = true;
					return;
				}
			}

			KeyDescription d;
			d.scriptName = scriptName;
			d.functionName = functionName;
			d.hasToggleState = hasToggleState;
			d.toggleEnabled = toggleEnabled;
			d.touchedSinceSweep = true;
			s.descriptions.push_back(std::move(d));
		}

		void SweepStaleDescriptions() {
			for (int vKeyInt = 0; vKeyInt < 256; ++vKeyInt) {
				std::vector<KeyDescription>& descs = states[vKeyInt].descriptions;
				if (descs.empty()) {
					continue;
				}
				descs.erase(
					std::remove_if(descs.begin(), descs.end(), [](const KeyDescription& d) { return !d.touchedSinceSweep; }),
					descs.end()
				);
				for (KeyDescription& d : descs) {
					d.touchedSinceSweep = false;
				}
			}
		}

		void ReassignBinding(USHORT oldVKey, USHORT newVKey, const std::string& scriptName, const std::string& functionName) {
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
				movedDesc.touchedSinceSweep = true;
				newState.descriptions.push_back(std::move(movedDesc));
				oldState.descriptions.erase(it);
			}
		}

		std::vector<TrackedKeyInfo> GetTrackedKeyInfo() {
			std::vector<TrackedKeyInfo> result;
			for (int vKeyInt = 0; vKeyInt < 256; ++vKeyInt) {
				const KeyPollState& s = states[vKeyInt];
				if (!s.registered) {
					continue;
				}
				USHORT vKey = static_cast<USHORT>(vKeyInt);
				bool isPressed = RawInput::IsKeyHeldReal(vKey);

				if (s.descriptions.empty()) {
					TrackedKeyInfo info;
					info.vKey = vKey;
					info.isPressed = isPressed;
					info.hasDescription = false;
					result.push_back(std::move(info));
					continue;
				}

				bool conflicted = s.descriptions.size() > 1;
				for (const KeyDescription& d : s.descriptions) {
					TrackedKeyInfo info;
					info.vKey = vKey;
					info.isPressed = isPressed;
					info.hasDescription = true;
					info.scriptName = d.scriptName;
					info.functionName = d.functionName;
					info.hasToggleState = d.hasToggleState;
					info.toggleEnabled = d.toggleEnabled;
					info.isConflicted = conflicted;
					result.push_back(std::move(info));
				}
			}
			return result;
		}
	}
}
