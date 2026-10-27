// Throws and grabs (OMF DE_COMF2Fighter::AnimHeadFollow 0x35026310, AnimCenterOfGravityFollow
// 0x35026590, AnimPrepareHeadAnchorForPreludeToFollow 0x35026240; Core DE_CActor::RotateToFace
// 0x70091900).
//
// A thrower's script poses its victim every frame: it sets the victim's reaction animation and
// time, slides and turns the victim's actor towards the release point, and then makes the victim
// "follow" the thrower's ThrowFollowTarget (an animated channel of the thrower's skeleton). The
// follow moves the victim's Pelvis actor -- the root of its skeleton -- so that the victim's
// HeadAnchor (another animated channel, the head) or the Pelvis itself lands on that target,
// matching its orientation; every bone, hit sphere and anchor under it moves with it until the
// next AnimSetTime poses the skeleton again.
#include <cmath>
#include <functional>

#include "game/runtime.h"

namespace game {
namespace {

GameObject *child_named(GameObject *o, const std::string &name) {
	for (GameObject *c : o->children) {
		if (c->name == name) return c;
	}
	return nullptr;
}

GameObject *descendant_named(GameObject *o, const std::string &name) {
	for (GameObject *c : o->children) {
		if (c->name == name) return c;
		if (GameObject *d = descendant_named(c, name)) return d;
	}
	return nullptr;
}

GameObject *fighter_of(GameObject *o) {
	for (; o; o = o->parent) {
		if (o->fighter) return o;
	}
	return nullptr;
}

// The pose as it stands now (the world update the original asks for validates a stale pose).
void repose(World &w, GameObject *o) {
	if (GameObject *f = fighter_of(o)) w.validate_animation(*f);
}

void set_pelvis_world(GameObject &f, GameObject &pelvis, const de::Transform &world) {
	pelvis.local = f.world().inverse().then(world);
}

}  // namespace

void register_grapple_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };

	// AnimPrepareHeadAnchorForPreludeToFollow: the HeadAnchor is put at the target, so that the
	// AnimHeadFollow that follows leaves the body where it is (the prelude starts from there).
	reg("COMF2Fighter::AnimPrepareHeadAnchorForPreludeToFollow(class CActor)", [](const NativeContext &c) {
		GameObject *f = c.self(), *t = c.arg_object(0);
		if (!f || !t) return;
		repose(c.world, t);
		c.world.validate_animation(*f);
		GameObject *ha = descendant_named(f, "HeadAnchor");
		if (!ha || !ha->parent) return;
		auto p = ha->parent->world().inverse().apply(t->world().t);
		ha->local.t = p;
		f->anchor_prepared_frame = c.world.frame();
	});
	// AnimHeadFollow: the Pelvis moves (and turns) so that the HeadAnchor, carried along at its
	// pose offset from the Pelvis, takes the target's place and orientation.
	reg("COMF2Fighter::AnimHeadFollow(class CActor)", [](const NativeContext &c) {
		GameObject *f = c.self(), *t = c.arg_object(0);
		if (!f || !t) return;
		repose(c.world, t);
		c.world.validate_animation(*f);
		GameObject *pelvis = child_named(f, "Pelvis"), *ha = descendant_named(f, "HeadAnchor");
		if (!pelvis || !ha) return;
		de::Transform rel = pelvis->world().inverse().then(ha->world());  // head in pelvis space
		set_pelvis_world(*f, *pelvis, t->world().then(rel.inverse()));
		static const bool debug = getenv("OPENBG_DEBUG_FOLLOW") != nullptr;
		if (debug) {
			auto tw = t->world().t, fw = f->world().t, pw = pelvis->world().t, hw = ha->world().t;
			fprintf(stderr, "[follow %u] %s target %s at %.1f %.1f %.1f; fighter %.1f %.1f %.1f; pelvis now %.1f %.1f %.1f (local %.1f %.1f %.1f); head %.1f %.1f %.1f\n",
			        c.world.frame(), f->name.c_str(), t->name.c_str(), tw[0], tw[1], tw[2], fw[0], fw[1], fw[2], pw[0], pw[1], pw[2],
			        pelvis->local.t[0], pelvis->local.t[1], pelvis->local.t[2], hw[0], hw[1], hw[2]);
		}
	});
	// AnimCenterOfGravityFollow: the Pelvis takes the target's place and orientation.
	reg("COMF2Fighter::AnimCenterOfGravityFollow(class CActor)", [](const NativeContext &c) {
		GameObject *f = c.self(), *t = c.arg_object(0);
		if (!f || !t) return;
		repose(c.world, t);
		c.world.validate_animation(*f);
		GameObject *pelvis = child_named(f, "Pelvis");
		if (!pelvis) return;
		set_pelvis_world(*f, *pelvis, t->world());
		static const bool debug = getenv("OPENBG_DEBUG_FOLLOW") != nullptr;
		if (debug) {
			auto tw = t->world().t, pw = pelvis->world().t;
			fprintf(stderr, "[follow %u] %s centre of gravity on %s at %.1f %.1f %.1f; pelvis now %.1f %.1f %.1f\n", c.world.frame(), f->name.c_str(),
			        t->name.c_str(), tw[0], tw[1], tw[2], pw[0], pw[1], pw[2]);
		}
	});
	// DE_CActor::RotateToFace: turn about Y to face the target, 1/f of the way (all of it for
	// f < 1.5).
	reg("CActor::RotateToFace(class CActor,float)", [](const NativeContext &c) {
		GameObject *o = c.self(), *t = c.arg_object(0);
		if (!o || !t) return;
		auto a = o->world().t, b = t->world().t;
		float yaw = std::atan2(b[0] - a[0], b[2] - a[2]);
		float s = std::sin(yaw), k = std::cos(yaw);
		std::array<float, 9> to{k, 0, -s, 0, 1, 0, s, 0, k};
		float f = c.arg<float>(1);
		o->local.r = f < 1.5f ? to : slerp_rotation(o->local.r, to, 1.0f / f);
	});
}

}  // namespace game
