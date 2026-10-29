// Hotkeys (Core DE_CImpulseRobot 0x7001f650..0x700209d0): named impulses with a default key or
// mouse button, the first default registered for a name winning; a matching key down, autorepeat
// or up pulses a CGenericEvent (Text = the name, INT1 = 1 on up) to every attached target.
// Nothing saves them: the user cannot rebind hotkeys.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace game {

class World;
struct GameObject;

class ImpulseRobot {
public:
	explicit ImpulseRobot(World &w) : w_(w) {}

	void assure(const std::string &name, const std::string &desc = {});
	void set_default_hotkey(const std::string &name, uint8_t key, bool alt, bool ctrl, bool shift);
	void set_default_mousekey(const std::string &name, int button, bool alt, bool ctrl, bool shift);
	void attach(const std::string &name, GameObject *target);
	// A C++ receiver (an engine factory's ProcessEvent), called with the down/up flag.
	void attach(const std::string &name, std::function<void(bool up)> handler);
	void set_focus(const std::string &name, GameObject *control);

	// TestKeyboardEvent (0x70020020): a Win32 virtual key going down (or repeating) or up.
	void key(int vk, bool down, bool shift, bool ctrl);
	// TestMouseEvent (0x70020340): button 0 left, 1 right, 2 middle pressed.
	void mouse_down(int button, bool shift, bool ctrl);
	// Pulses wait for the next World tick, as the robot raises them in its own update.
	void flush();

	// The script-visible robot object (CObject::GlobalImpulseRobot).
	GameObject *object();

private:
	struct Impulse {
		std::string name, desc;
		uint8_t key = 0;
		int mouse = -1;
		bool alt = false, shift = false, ctrl = false;
		std::vector<GameObject *> targets;
		std::vector<std::function<void(bool)>> handlers;
		GameObject *focus = nullptr;
	};
	Impulse &find(const std::string &name);
	void pulse(Impulse &imp, bool up);

	World &w_;
	std::vector<Impulse> impulses_;
	std::vector<std::pair<size_t, bool>> pending_;
	GameObject *object_ = nullptr;
};

}  // namespace game
