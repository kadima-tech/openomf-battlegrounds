// Pure computations behind natives (game/native_math.h), without game data.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "game/bone_channels.h"
#include "game/native_math.h"

namespace {

int failures = 0;
void check(bool ok, const char *what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++failures;
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

// A node of a synthetic actor tree for game::bone_channel.
struct Node {
	Node *parent = nullptr;
	std::vector<Node *> children;
	bool is_actor = true;
	std::string name;
	int channel = -2;
};

// The vertices order `order` visits over `count` spawns from a fresh instance, -1 where a pass ends.
std::vector<int> walk(int order, int n, int count) {
	std::vector<int> out;
	int next = 0;
	for (int i = 0; i < count; ++i) {
		game::MeshStep s = game::mesh_vertex_step(order, n, next);
		out.push_back(s.pass_done ? -1 : s.vertex);
	}
	return out;
}

}  // namespace

int main() {
	// RaiseToPower through __ftol.
	check(game::ftol(std::pow(2.0, 3.0)) == 8, "ftol: 2^3 = 8");
	check(game::ftol(-2.7) == -2 && game::ftol(2.7) == 2, "ftol truncates towards zero");
	check(game::ftol(std::pow(2.0, 31.0)) == INT32_MIN, "ftol: 2^31 is out of range, 0x80000000");
	check(game::ftol(-2147483648.0) == INT32_MIN && game::ftol(2147483647.0) == INT32_MAX, "ftol: the int32 limits");
	check(game::ftol(std::pow(10.0, 40.0)) == INT32_MIN && game::ftol(NAN) == INT32_MIN, "ftol: huge and NaN give 0x80000000");
	check(game::ftol(std::pow(2.0, -1.0)) == 0, "ftol: 2^-1 = 0");

	// DampenMomentum subtracts from the length.
	auto m = game::dampen_momentum({-3.01f, 0, 0}, 0.3f);
	check(near(m[0], -2.71f) && m[1] == 0 && m[2] == 0, "DampenMomentum: 3.01 - 0.3 = 2.71, same direction");
	m = game::dampen_momentum({3, 4, 0}, 1);
	check(near(m[0], 2.4f) && near(m[1], 3.2f), "DampenMomentum: (3,4,0) by 1 = length 4");
	m = game::dampen_momentum({0.1f, 0.1f, 0}, 0.3f);
	check(m[0] == 0 && m[1] == 0 && m[2] == 0, "DampenMomentum: shorter than the amount stops");
	m = game::dampen_momentum({0, 0, 0}, 0.3f);
	check(m[0] == 0 && m[1] == 0 && m[2] == 0, "DampenMomentum: zero stays zero");

	// Mesh spawner vertex orders.
	check(walk(1, 3, 7) == std::vector<int>{0, 1, 2, 0, 1, 2, 0}, "order 1 wraps forwards");
	check(walk(2, 3, 7) == std::vector<int>{0, 2, 1, 0, 2, 1, 0}, "order 2 wraps backwards");
	check(walk(3, 3, 7) == std::vector<int>{0, 1, -1, 0, 1, -1, 0}, "order 3: one pass, the last vertex's particle dropped");
	check(walk(4, 3, 7) == std::vector<int>{-1, 2, 2, 1, -1, 2, 2}, "order 4: ends at once from a fresh instance, then backwards (vertex n clamped)");
	int next = 0;
	check(game::mesh_vertex_step(0, 3, next).vertex == -1 && next == 0, "order 0 leaves the vertex to the caller");
	check(game::mesh_vertex_step(7, 3, next).vertex == 0 && next == 0, "an unknown order stays on the current vertex");


	// Bone channel ownership (World::pose_skeleton, game/bone_channels.h). Channels: Body, Pelvis
	// (under Body), LeftThigh (under Pelvis), Head (a root), Chest (under Body), LeftWing1 (under Chest).
	{
		const std::vector<std::pair<std::string, int>> channels = {{"Body", -1}, {"Pelvis", 0}, {"LeftThigh", 1}, {"Head", -1}, {"Chest", 0}, {"LeftWing1", 4}};
		auto channel_of = [&](const std::string &n) {
			for (size_t i = 0; i < channels.size(); ++i) {
				if (channels[i].first == n) return int(i);
			}
			return -1;
		};
		auto channel_parent = [&](int c) { return channels[size_t(c)].second; };
		std::vector<std::unique_ptr<Node>> nodes;
		auto add = [&](Node *parent, const char *name) {
			nodes.push_back(std::make_unique<Node>());
			Node *n = nodes.back().get();
			n->name = name;
			n->parent = parent;
			if (parent) parent->children.push_back(n);
			return n;
		};
		Node *fighter = add(nullptr, "Pyros");
		Node *head = add(fighter, "Head");
		Node *pelvis = add(fighter, "Pelvis");
		Node *thigh = add(pelvis, "LeftThigh");
		Node *chest = add(fighter, "Chest");
		Node *wing_base = add(chest, "WingBase");
		Node *wing = add(wing_base, "LeftWing1");
		Node *debris = add(add(fighter, "DestructionExplosion"), "DebrisParent");
		Node *debris_head = add(debris, "Head");
		Node *debris_pelvis = add(debris, "Pelvis");
		Node *debris_thigh = add(debris, "LeftThigh");
		add(fighter, "Effects")->is_actor = false;
		auto ch = [&](Node *n) { return game::bone_channel(*fighter, *n, 0, channel_of, channel_parent); };
		check(ch(head) == 3 && ch(debris_head) == -1, "bones: a root bone keeps its channel, debris named like it gets none");
		check(ch(thigh) == 2 && ch(debris_thigh) == -1, "bones: a child bone under its parent channel's bone keeps it, a debris copy gets none");
		check(ch(wing) == 5, "bones: a name only one actor has keeps its channel under a non-bone parent (WingBase)");
		check(ch(pelvis) == 1 && ch(debris_pelvis) == -1, "bones: a bone under Body belongs to the actor under the fighter");
		check(ch(chest) == 4 && ch(wing_base) == -1, "bones: a name with no channel gets none");
		thigh->parent = nullptr;
		check(ch(debris_thigh) == 2, "bones: with no owner left, a copy keeps the channel");
	}
	printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
	return failures ? 1 : 0;
}
