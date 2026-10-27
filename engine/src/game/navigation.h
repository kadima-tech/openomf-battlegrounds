// Navigation-link paths for move-to commands (see navigation.cpp).
#pragma once

#include <array>
#include <map>
#include <vector>

#include "game/runtime.h"

namespace game {

class Navigation {
public:
	explicit Navigation(World &w) : w_(w) {}
	// DE_CMoveToCommand::Update: the position a creature moving to `dest` should head for now
	// (the next navigation area on its path, or `dest`) and the distance left along the path.
	// False when the arena has no navigation areas.
	bool steer(GameObject &creature, const std::array<float, 3> &dest, std::array<float, 3> *waypoint, float *distance);
	// Areas are re-read when the scene changes (a new arena).
	void reset() { built_ = false; paths_.clear(); }

private:
	struct Link {
		GameObject *object = nullptr;
		GameObject *from = nullptr, *to = nullptr;  // +0x20 (its actor), +0x54 (destination)
		bool open = true;                           // +0x64
		float walked = 0, cost = 0;                 // +0x60, +0x5c
		Link *parent = nullptr;                     // +0x68
	};
	struct Path {
		std::vector<GameObject *> areas;  // CPath +0xa4: [destination area, ..., next area]
		std::array<float, 3> dest{1e30f, 0, 0};
		float calculated = -1e9f;         // +0x280 on the creature
	};
	void build();
	GameObject *area_at(const std::array<float, 3> &p);
	void calculate(Path &path, GameObject &creature, const std::array<float, 3> &dest);
	std::array<float, 3> move_to(Path &path, GameObject &creature, float *distance);
	float scene_ms() const;
	World &w_;
	bool built_ = false;
	std::vector<Link> links_;
	std::vector<GameObject *> areas_;
	std::map<const GameObject *, Path> paths_;
};

}  // namespace game
