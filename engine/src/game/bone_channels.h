// Which actor of a fighter's tree an animation channel drives (World::pose_skeleton), without game
// data: any node type with `parent`, `children`, `is_actor`, `name` and `channel` (-2 not looked up).
#pragma once

namespace game {

// The channel a bone actor `o` takes: the channel of its name (channel_of), unless another actor of
// the same name is the channel's owner, in which case -1 (o keeps its own place). A channel belongs
// to the actor of its name whose parent is the channel's parent bone, or the fighter itself for a
// root bone or one under `body`. The destruction debris (DestructionExplosion\DebrisParent's Head,
// Pelvis, limbs) would otherwise be posed as the bone and stand in for it when the pose is read
// back, so the body's extra rotation and scripted turns would miss those bones. A name only one
// actor has keeps its channel wherever it hangs (Gargoyle's wing bones under WingBase).
// channel_parent(c) is channel c's parent channel (-1 for a root).
template <class Node, class ChannelOf, class ChannelParent>
int bone_channel(const Node &fighter, const Node &o, int body, ChannelOf channel_of, ChannelParent channel_parent) {
	const int c = channel_of(o.name);
	if (c < 0) return c;
	auto parent_channel = [&](const Node &n) -> int {
		if (&n == &fighter) return -1;
		return n.channel == -2 ? channel_of(n.name) : n.channel;
	};
	auto owns = [&](const Node &n) {
		const int bp = channel_parent(c);
		if (!n.parent) return false;
		if (bp < 0 || bp == body) return n.parent == &fighter;
		return parent_channel(*n.parent) == bp;
	};
	if (owns(o)) return c;
	struct Rival {
		const Node &self;
		decltype(owns) &owns_it;
		bool in(const Node &n) const {
			for (const Node *x : n.children) {
				if (!x->is_actor) continue;
				if (x != &self && x->name == self.name && owns_it(*x)) return true;
				if (in(*x)) return true;
			}
			return false;
		}
	};
	return Rival{o, owns}.in(fighter) ? -1 : c;
}

}  // namespace game
