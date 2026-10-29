#include "game/impulses.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <strings.h>

#include "game/runtime.h"
#include "game/session.h"

namespace game {

namespace {

// DE_CKeyboardEvent::ASCII: the character a virtual key types (US layout), 0 for keys that type
// none (Esc, F-keys, arrows), which then match by their VK.
int ascii(int vk, bool shift) {
	if (vk >= 0x41 && vk <= 0x5a) return shift ? vk : vk + 0x20;
	if (vk >= 0x30 && vk <= 0x39) return shift ? ")!@#$%^&*("[vk - 0x30] : vk;
	if (vk >= 0x60 && vk <= 0x6f) return vk - 0x30;
	if (vk == 0x20 || vk == 9) return vk;
	static const struct {
		int vk;
		char plain, shifted;
	} oem[] = {{0xba, ';', ':'}, {0xbb, '=', '+'}, {0xbc, ',', '<'}, {0xbd, '-', '_'}, {0xbe, '.', '>'}, {0xbf, '/', '?'},
	           {0xc0, '`', '~'}, {0xdb, '[', '{'}, {0xdc, '\\', '|'}, {0xdd, ']', '}'}, {0xde, '\'', '"'}};
	for (auto &o : oem) {
		if (o.vk == vk) return shift ? o.shifted : o.plain;
	}
	return 0;
}

}  // namespace

ImpulseRobot::Impulse &ImpulseRobot::find(const std::string &name) {
	for (auto it = impulses_.rbegin(); it != impulses_.rend(); ++it) {
		if (!strcasecmp(it->name.c_str(), name.c_str())) return *it;
	}
	impulses_.push_back({});
	impulses_.back().name = name;
	return impulses_.back();
}

void ImpulseRobot::assure(const std::string &name, const std::string &desc) {
	bool fresh = std::none_of(impulses_.begin(), impulses_.end(), [&](const Impulse &i) { return !strcasecmp(i.name.c_str(), name.c_str()); });
	Impulse &i = find(name);
	if (fresh) i.desc = desc;
}

void ImpulseRobot::set_default_hotkey(const std::string &name, uint8_t key, bool alt, bool ctrl, bool shift) {
	Impulse &i = find(name);
	if (i.key) return;
	i.key = key, i.alt = alt, i.ctrl = ctrl, i.shift = shift;
}

void ImpulseRobot::set_default_mousekey(const std::string &name, int button, bool alt, bool ctrl, bool shift) {
	Impulse &i = find(name);
	if (i.mouse >= 0) return;
	i.mouse = button, i.alt = alt, i.ctrl = ctrl, i.shift = shift;
}

void ImpulseRobot::attach(const std::string &name, GameObject *target) {
	static const bool debug = getenv("OPENBG_DEBUG_IMPULSES") != nullptr;
	if (debug) fprintf(stderr, "[impulse] attach %s -> %s\n", name.c_str(), target ? target->name.c_str() : "-");
	Impulse &i = find(name);
	if (target && std::find(i.targets.begin(), i.targets.end(), target) == i.targets.end()) i.targets.push_back(target);
}

void ImpulseRobot::attach(const std::string &name, std::function<void(bool)> handler) { find(name).handlers.push_back(std::move(handler)); }

void ImpulseRobot::set_focus(const std::string &name, GameObject *control) { find(name).focus = control; }

void ImpulseRobot::key(int vk, bool down, bool shift, bool ctrl) {
	const int ch = ascii(vk, shift);
	for (size_t k = 0; k < impulses_.size(); ++k) {
		const Impulse &i = impulses_[k];
		if (!i.key) continue;
		bool hit = vk >= 0x41 && vk <= 0x5a ? ch == i.key || ch == i.key + 0x20 : ch ? ch == i.key : vk == int(int8_t(i.key));
		// Alt is never recorded by the keyboard hook, so an Alt hotkey never fires; modifiers not
		// required are ignored.
		if (!hit || i.alt || (i.shift && !shift) || (i.ctrl && !ctrl)) continue;
		pending_.push_back({k, !down});
	}
}

void ImpulseRobot::mouse_down(int button, bool shift, bool ctrl) {
	for (size_t k = 0; k < impulses_.size(); ++k) {
		const Impulse &i = impulses_[k];
		if (i.mouse != button || (i.shift && !shift) || (i.ctrl && !ctrl)) continue;
		pending_.push_back({k, false});
	}
}

void ImpulseRobot::flush() {
	auto pending = std::move(pending_);
	pending_.clear();
	for (auto [k, up] : pending) {
		if (k < impulses_.size()) pulse(impulses_[k], up);
	}
}

void ImpulseRobot::pulse(Impulse &imp, bool up) {
	// A focus control requires its branch to have the focus; the only one set is
	// Session().GameDialog(), which is null in the original too.
	if (imp.focus) return;
	static const bool debug = getenv("OPENBG_DEBUG_IMPULSES") != nullptr;
	if (debug) fprintf(stderr, "[impulse %u] %s %s to %zu targets\n", w_.frame(), imp.name.c_str(), up ? "up" : "down", imp.targets.size());
	for (auto &h : imp.handlers) h(up);
	auto targets = imp.targets;
	for (GameObject *t : targets) {
		if (!t || w_.object(t->handle) != t) continue;
		uint32_t e = new_event(w_, "<ImpulsePulse>");
		w_.memory().set<uint32_t>(e + 44, w_.string(imp.name));  // Text
		w_.memory().set<int32_t>(e + 4, up ? 1 : 0);            // INT1
		w_.send_event(t, e);
	}
}

GameObject *ImpulseRobot::object() {
	if (!object_ || w_.object(object_->handle) != object_) object_ = w_.create("ImpulseRobot", "ImpulseRobot");
	return object_;
}

}  // namespace game
