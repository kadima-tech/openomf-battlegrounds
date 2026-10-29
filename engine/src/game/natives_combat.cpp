// Combat: attack descriptions (DE_COMF2Manager::DE_CAttackInfo, OMF module), collision testers
// (DE_CCollisionTester, Core) over affector volumes, and danger (DE_CDanger, Creature).
//
// An attack state prepares its CAttackInfo, then each frame of its hit window calls
// AttackerTryAttack(AttackInfo, <limb tester>). TestForConnectedAttack runs the tester -- the
// limb's hit-detector spheres against every affector in the filter's groups (the victims'
// get-hit spheres, group 0x100000) -- and accepts the first new victim. The attacker's script
// then fills in damage, reaction and momentum and hands the CAttackInfo to the victim's
// BAttackable::AttackableReceiveAttack through a behavior interface.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <tuple>

#include "de/image.h"
#include "game/replay.h"
#include "game/forces.h"
#include "game/particles.h"
#include "game/runtime.h"
#include "game/session.h"
#include "game/simple_physics.h"

namespace game {

namespace {

// Script-visible CAttackInfo fields (DE_COMF2ManagerFactory::Init member registrations).
constexpr uint32_t kReactionType = 0x44, kEffectType = 0x48, kAttackType = 0x4c, kUser = 0x54;
constexpr uint32_t kBlockPenetration = 0x58, kDamage = 0x5c, kStunDamage = 0x60, kEnergyDamage = 0x64;
constexpr uint32_t kRecoveryTime = 0x68, kDiminishDamage = 0x6c, kRetainDamage = 0x70, kMomentumAdjust = 0x74;
constexpr uint32_t kGuideToLastHit = 0x78, kConnectPoint = 0x7c, kEffectDirection = 0x88;
constexpr uint32_t kEffectDirection2 = 0x94, kTransferedMomentum = 0xa0, kReelDirection = 0xac;
constexpr uint32_t kMaxNumberOfHits = 0xc0, kFlags = 0xc4;  // flags: 1 blocked, 4 super, 8 melee, 0x10 no evade, 0x20 force combo

struct AttackState {
	GameObject *owner = nullptr;  // +0x14
	uint32_t id = 0;              // +0x50
	std::string name;             // +0xb8: the preparing method, owner and Prepare's argument
	std::vector<GameObject *> victims;  // +0x24
};

struct HitRecord {
	uint32_t id;
	GameObject *owner, *victim;
};

struct Collision {
	GameObject *source = nullptr;  // the tester's affector that touched
	GameObject *touched = nullptr; // the affector it touched
	GameObject *owner = nullptr;   // that affector's owning actor (the victim)
	std::array<float, 3> point{0, 0, 0}, normal{0, 1, 0};
	std::array<float, 3> face_normal{0, 1, 0};  // the touched surface's own normal (+0x18)
	float distance = 0;
};

struct Tester {
	uint32_t groups = 0;
	bool no_repeat_owners = false;
	bool active = false;                                // Activate(): tested every tick
	std::vector<std::pair<Brain *, uint32_t>> notify;   // AddNotify(instance, method)
	GameObject *force = nullptr;                        // SetAttachedForce: applied to what it touches
	uint32_t event = 0;                                 // the CCollisionEvent handed to them
	std::vector<GameObject *> sources;  // branch roots (AddSourceBranch)
	std::vector<GameObject *> singles;  // lone affectors (AddSource)
	std::vector<Collision> results;
	uint32_t results_handle = 0;
};

struct DangerTiming {
	float delay_ms = 0;
	uint32_t set_at = 0;  // scene time of SetDangerDelay
};

std::map<uint32_t, AttackState> g_attacks;           // by CAttackInfo block
std::vector<HitRecord> g_hits;                        // DE_CAttackInfo::m_HitRecords
std::map<uint32_t, Tester> g_testers;                 // by tester handle
std::map<uint32_t, std::pair<uint32_t, size_t>> g_collision_infos;  // info handle -> (tester, index)
std::map<const GameObject *, std::vector<std::string>> g_hit_names;  // fighter +0x490
std::map<const GameObject *, DangerTiming> g_danger_timing;
uint32_t g_next_attack_id = 1;
struct PackedHit {
	uint8_t damage = 0, flags = 0;
	uint32_t effect = 0, reaction = 0, attack = 0;
	std::array<float, 3> point{}, dir{}, dir2{};
};
std::map<uint32_t, PackedHit> g_packed_hits;  // by CData block

float length(const std::array<float, 3> &v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

GameObject *top_fighter(GameObject *o) {
	for (GameObject *p = o; p; p = p->parent) {
		if (p->fighter) {
			return p;
		}
	}
	return nullptr;
}


// DE_CAttackInfo::TestForFighterBlock (OMF 0x35017560): the attack direction in the defender's
// frame picks one of 16 sectors (22.5 degrees each, sector 0 = coming straight at its face); the
// attack's required zones, rotated to that sector, must all be zones the defender can block
// (its +0x44c mask). A defender that isn't a fighter always blocks.
bool fighter_blocks(GameObject *defender, GameObject *owner, std::array<float, 3> dir, uint16_t zones) {
	if (!owner || !defender || !defender->fighter) {
		return true;
	}
	auto l = defender->world().inverse_rotate(dir);
	float a = -float(std::atan2(l[0], l[2]));
	a = (a + 3.14159f + 0.19634938f) / 3.14159f * 8.0f;
	int n = int(a);  // __ftol truncates
	if (n < 0) n += (n + 15) & ~15;
	n &= 15;
	uint16_t z = n ? uint16_t((zones << (16 - n)) | (zones >> n)) : zones;
	return (defender->stats.block_mask & z) == z;
}

float radius(const GameObject &a) { return a.falloff > 0 ? a.falloff : a.hotspot; }

void collect_affectors(GameObject *o, std::vector<GameObject *> &out) {
	if (o->shape != GameObject::Shape::None) {
		out.push_back(o);
	}
	for (GameObject *c : o->children) {
		collect_affectors(c, out);
	}
}

// DE_CCollisionTester::Test: every sphere in the source branches against every active affector
// in the tester's groups, in scene order.
void run_tester(World &w, Tester &t) {
	t.results.clear();
	std::vector<GameObject *> sources;
	for (GameObject *b : t.sources) {
		collect_affectors(b, sources);
	}
	for (GameObject *a : t.singles) {
		if (a->shape != GameObject::Shape::None) sources.push_back(a);
	}
	if (sources.empty()) {
		return;
	}
	GameObject *self = top_fighter(sources[0]);
	for (GameObject *target : w.affectors()) {
		if (!in_scene(*target) || !(target->groups & t.groups) || target->shape == GameObject::Shape::Cone) {
			continue;
		}
		if (self && top_fighter(target) == self) {
			continue;
		}
		for (GameObject *s : sources) {
			if (s == target || !in_scene(*s)) continue;
			auto ct = touch(w, *s, *target);
			if (!ct) continue;
			static const bool dbg = getenv("OPENBG_DEBUG_TESTERS") != nullptr;
			if (dbg && s->shape == GameObject::Shape::Mesh) {
				auto sw = s->world(), tw = target->world();
				fprintf(stderr, "[tester] %s (%s) at %.0f %.0f %.0f touches %s under %s at %.0f %.0f %.0f, point %.0f %.0f %.0f\n", s->name.c_str(), s->mesh ? s->mesh.info().name.c_str() : "-", sw.t[0], sw.t[1], sw.t[2], target->name.c_str(), target->parent ? target->parent->name.c_str() : "-", tw.t[0], tw.t[1], tw.t[2], ct->point[0], ct->point[1], ct->point[2]);
			}
			Collision c;
			c.source = s;
			c.touched = target;
			c.owner = w.affector_owner(target);
			c.point = ct->point;
			c.normal = ct->normal;
			c.face_normal = ct->face_normal;
			c.distance = ct->distance;
			t.results.push_back(c);
			break;
		}
	}
}

// DE_CAttackInfo::AddPotentialVictim (OMF 0x35016420): one hit per victim per prepared attack,
// and (unless m_MaxNumberOfHits allows more) one hit per attack name until the victim resets
// its hit list.
bool add_potential_victim(World &w, uint32_t info, AttackState &a, GameObject *victim) {
	for (auto &h : g_hits) {
		if (h.id == a.id && h.owner == a.owner && h.victim == victim) {
			return false;
		}
	}
	if (victim->fighter) {
		auto &names = g_hit_names[victim];
		int32_t max_hits = w.memory().get<int32_t>(info + kMaxNumberOfHits);
		if (max_hits == 0) {
			if (std::find(names.begin(), names.end(), a.name) != names.end()) {
				return false;
			}
		} else {
			bool slot = false;
			for (int32_t i = 0; i < max_hits && !slot; ++i) {
				std::string n = a.name + std::to_string(i);
				if (std::find(names.begin(), names.end(), n) == names.end()) {
					names.push_back(n);
					a.victims.push_back(victim);
					slot = true;
				}
			}
			if (!slot) {
				return false;
			}
		}
		if (std::find(names.begin(), names.end(), a.name) == names.end()) {
			names.push_back(a.name);
		}
	}
	g_hits.insert(g_hits.begin(), {a.id, a.owner, victim});
	if (g_hits.size() > 256) {
		g_hits.pop_back();
	}
	return true;
}

// Sphere / cone strength at a point: 1 inside the hotspot (umbra), falling off linearly to 0 at
// the falloff (penumbra; near/far attenuation along the axis).
float affector_strength(const GameObject &a, const std::array<float, 3> &p);

// A box's intensity value (DE_CBoxAffector +0x180, references chunk 3 list 0). Only
// Value\FLOAT\ImageMap is read: contents [CValueFLOAT v1: scale +0x2c][v1: u32 plane mode +0x3c],
// references chunk 3: the image (+0x34).
struct ImageMapValue {
	bool present = false;
	de::Image image;
	uint32_t mode = 0;
	float scale = 1;
};

const ImageMapValue &box_intensity(World &w, const GameObject &a) {
	static std::map<std::pair<const de::ObjectsFile *, uint32_t>, ImageMapValue> cache;
	auto key = std::make_pair(a.file, a.index);
	auto it = cache.find(key);
	if (it != cache.end()) return it->second;
	ImageMapValue &v = cache[key];
	if (!a.file) return v;
	try {
		auto lists = reference_lists(w.data(), {a.file, a.index});
		if (lists.size() < 4 || lists[3].empty() || lists[3][0].empty() || !lists[3][0][0]) return v;
		de::ObjectRef map = lists[3][0][0];
		if (map.info().class_path != "Value\\FLOAT\\ImageMap") return v;
		de::Contents c(map.file->object_data(map.info()).contents);
		std::vector<std::pair<size_t, int>> chunks;
		while (c.remaining() > 1) {
			de::Chunk ch = de::begin_chunk(c);
			if (ch.end > c.size()) break;
			chunks.push_back({c.pos(), ch.version});
			c.seek(ch.end);
		}
		if (chunks.size() < 3) return v;
		c.seek(chunks[1].first);
		v.scale = c.f32();
		c.seek(chunks[2].first);
		v.mode = c.u32();
		auto mlists = reference_lists(w.data(), map);
		if (mlists.size() < 4 || mlists[3].empty() || mlists[3][0].empty() || !mlists[3][0][0]) return v;
		de::ObjectRef img = mlists[3][0][0];
		v.image = de::read_image(*img.file, img.info());
		v.present = v.image.width > 0 && v.image.height > 0;
	} catch (const std::exception &) {
		v.present = false;
	}
	return v;
}

// DE_CImage::GetNormalizedPixel (Core 0x7018e4b0): the pixel at (u x width, v x height),
// clamped, of that channel / 255. Channel 4 is the luminance.
float normalized_pixel(const de::Image &img, int channel, float u, float v) {
	auto it = img.channels.find(channel);
	if (it == img.channels.end()) return 0;
	int x = std::clamp(int(u * float(img.width)), 0, img.width - 1);
	int y = std::clamp(int(v * float(img.height)), 0, img.height - 1);
	return float(it->second[size_t(y) * size_t(img.width) + size_t(x)]) / 255.0f;
}

float affector_strength(World &w, const GameObject &a, const std::array<float, 3> &p) {
	if (a.shape == GameObject::Shape::Box) {
		// DE_CBoxAffector::IsTouching(point, &intensity) (Core 0x7007e520): inside the box, the
		// intensity value at the box-normalised position (DE_CValueFLOATImageMap::GetValue: plane
		// mode 0 uses x, y; 1 x, z; 2 y, z), else 1.
		de::Transform t = a.world();
		auto l = t.inverse_rotate({p[0] - t.t[0], p[1] - t.t[1], p[2] - t.t[2]});
		std::array<float, 3> n{};
		for (int i = 0; i < 3; ++i) {
			if (l[i] < a.box_min[i] || l[i] > a.box_max[i]) return 0;
			float span = a.box_max[i] - a.box_min[i];
			n[i] = span > 0 ? (l[i] - a.box_min[i]) / span : 0;
		}
		const ImageMapValue &m = box_intensity(w, a);
		if (!m.present) return 1;
		float u = m.mode == 2 ? n[1] : n[0], v = m.mode == 0 ? n[1] : n[2];
		return normalized_pixel(m.image, 4, u, v) * m.scale;
	}
	return affector_strength(a, p);
}

float affector_strength(const GameObject &a, const std::array<float, 3> &p) {
	de::Transform t = a.world();
	std::array<float, 3> d{p[0] - t.t[0], p[1] - t.t[1], p[2] - t.t[2]};
	float dist = length(d);
	if (a.shape == GameObject::Shape::Sphere) {
		if (dist <= a.hotspot) return 1;
		if (dist >= a.falloff || a.falloff <= a.hotspot) return 0;
		return 1 - (dist - a.hotspot) / (a.falloff - a.hotspot);
	}
	if (a.shape == GameObject::Shape::Cone) {
		auto axis = t.rotate({0, 0, 1});
		float along = d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2];
		if (along <= 0 || dist < 1e-4f) return 0;
		float angle = std::acos(std::clamp(along / dist, -1.0f, 1.0f)) * 57.29578f;
		float s = angle <= a.umbra ? 1 : (angle >= a.penumbra || a.penumbra <= a.umbra ? 0 : 1 - (angle - a.umbra) / (a.penumbra - a.umbra));
		float far_end = a.far_end > 0 ? a.far_end : a.far_begin;
		if (along > far_end) return 0;
		if (along > a.far_begin && far_end > a.far_begin) s *= 1 - (along - a.far_begin) / (far_end - a.far_begin);
		return s;
	}
	return 0;
}

// The CDanger property of a danger affector (a child UniqueActorProperty\Danger\*).
GameObject *danger_property(GameObject &a) {
	for (GameObject *c : a.children) {
		if (c->is("UniqueActorProperty\\Danger")) return c;
	}
	return nullptr;
}

}  // namespace

// DE_CCollisionTester::Tick: an active tester tests every scene tick and sends its notify methods
// a collision event when something touched.
// DE_CCollisionTester's temporal tick (16 ms, CCollisionTester::Activate): test, hand the
// contacts to the attached force, then the notify methods.
void tick_collision_tester(World &w, uint32_t h) {
	auto it = g_testers.find(h);
	if (it == g_testers.end() || !it->second.active || (it->second.notify.empty() && !it->second.force)) return;
	Tester &t = it->second;
	run_tester(w, t);
	if (t.results.empty()) return;
	if (t.force) {
		for (auto &r : t.results) {
			Contact ct;
			ct.source = r.source;
			ct.touched = r.touched;
			ct.point = r.point;
			ct.normal = ct.face_normal = r.normal;
			ct.distance = r.distance;
			w.forces->deliver(*t.force, ct);
		}
	}
	if (!t.event) t.event = w.memory().alloc(16);
	auto notify = t.notify;
	for (auto &[b, m] : notify) {
		if (!b || b->inst.shut_down) continue;
		std::vector<uint8_t> args(4);
		memcpy(args.data(), &t.event, 4);
		b->call_handle(w, m, args);
	}
}

void World::reset_attack_info(uint32_t a) {
	// DE_CAttackInfo::Reset / ResetAttackStats defaults.
	for (uint32_t off = kMaxNumberOfHits; off < kMaxNumberOfHits + 4; ++off) mem_.set<uint8_t>(a + off, 0);
	for (uint32_t off : {kDamage, kBlockPenetration, kStunDamage, kEnergyDamage, kRecoveryTime, kRetainDamage}) {
		mem_.set<float>(a + off, 0);
	}
	for (uint32_t off = kEffectDirection; off < kReelDirection + 12; off += 4) mem_.set<float>(a + off, 0);
	mem_.set<uint32_t>(a + kReactionType, 2);
	mem_.set<uint32_t>(a + kEffectType, 1);
	mem_.set<uint32_t>(a + kAttackType, 2);
	mem_.set<float>(a + kMomentumAdjust, -99999.0f);
	mem_.set<float>(a + kGuideToLastHit, 1.0f);
	mem_.set<float>(a + kDiminishDamage, 0.2f);
	mem_.set<uint8_t>(a + kFlags, 0);
}

void World::parented(GameObject *o, bool was_in_scene) {
	if (!o || was_in_scene || !in_scene(*o)) return;
	std::function<void(GameObject *)> clear = [&](GameObject *x) {
		x->affector_seq = 0;
		for (GameObject *c : x->children) clear(c);
	};
	clear(o);
	number_affectors(o);
}

void World::number_affectors(GameObject *root) {
	if (!root) return;
	for (auto it = root->children.rbegin(); it != root->children.rend(); ++it) {
		if (!parked(**it)) number_affectors(*it);
	}
	if (root->shape != GameObject::Shape::None && !root->affector_seq) root->affector_seq = ++affector_seq_;
}

std::vector<GameObject *> World::affectors() {
	std::vector<GameObject *> out;
	for (auto &o : objects_) {
		if (o.shape == GameObject::Shape::None) continue;
		if (!in_scene(o)) {
			o.affector_seq = 0;  // out of the list; joins at the end when added again
			continue;
		}
		// In the scene by another way than enter_scene (parented under a scene actor): it joins now.
		if (!o.affector_seq) o.affector_seq = ++affector_seq_;
		out.push_back(&o);
	}
	std::stable_sort(out.begin(), out.end(), [](const GameObject *a, const GameObject *b) { return a->affector_seq < b->affector_seq; });
	return out;
}

float World::danger_at(const std::array<float, 3> &p, const GameObject *ignore_owner, GameObject **greatest, float *greatest_level) {
	float total = 0, best = 0;
	GameObject *best_prop = nullptr;
	for (auto &o : objects_) {
		if (o.shape == GameObject::Shape::None || !(o.groups & 0x1000) || !in_scene(o)) continue;
		GameObject *prop = danger_property(o);
		if (!prop || prop->danger_level <= 0) continue;
		if (ignore_owner && (o.owner_actor == ignore_owner || top_fighter(&o) == ignore_owner)) continue;
		float v = affector_strength(*this, o, p) * prop->danger_level;
		total += v;
		if (v > best) {
			best = v;
			best_prop = prop;
		}
	}
	if (greatest) *greatest = best_prop;
	if (greatest_level) *greatest_level = best;
	return total;
}

GameObject *World::best_melee_target(GameObject &o) {
	GameObject *best = nullptr;
	float best_d = 200;
	de::Transform t = o.world();
	for (auto &x : objects_) {
		if (&x == &o || !x.fighter || !x.spawned) continue;
		auto p = x.world().t;
		auto l = t.inverse_rotate({p[0] - t.t[0], p[1] - t.t[1], p[2] - t.t[2]});
		float d = length(l);
		if (l[2] >= 0 && d < best_d) {
			best_d = d;
			best = &x;
		}
	}
	return best;
}

// DE_COMF2Fighter::Guide (OMF 0x35029010), ground and air cases: within 200 units of a target
// in front, slide towards it (unless closer than 11.75) and turn towards it by up to f degrees,
// f = frontness x (1 - distance / 100 on the ground, / 200 in the air) x power.
void World::guide(GameObject &o) {
	if (!o.guide_mode) return;
	GameObject *target = o.guide_mode == 1 ? o.last_hit : o.guide_mode == 2 ? o.last_hit_by : best_melee_target(o);
	bool move = o.guide_mode == 1 || (o.guide_mode == 3 && !o.on_floor);
	if (!target || o.guide_frames <= 0) {
		if (o.guide_frames <= 0) o.guide_mode = 0;
		return;
	}
	--o.guide_frames;
	de::Transform t = o.world();
	auto tp = target->world().t;
	auto l = t.inverse_rotate({tp[0] - t.t[0], tp[1] - t.t[1], tp[2] - t.t[2]});
	float dist = length(l);
	if (dist > 200 || dist < 1e-4f) return;
	std::array<float, 3> step = dist <= 11.75f ? std::array<float, 3>{0, 0, 0} : l;
	std::array<float, 3> n{l[0] / dist, l[1] / dist, l[2] / dist};
	if (n[2] < 0) return;
	float f;
	if (!o.on_floor) {
		f = n[2] * (1 - dist / 200) * o.guide_power;
		float k = f * 0.7f;
		step = {step[0] * k, step[1] * k * 1.3f, step[2] * k};
	} else {
		f = n[2] * (1 - dist / 100) * o.guide_power;
		float k = f * 0.75f;
		step = {step[0] * k, step[1] * k, step[2] * k};
	}
	if (move) {
		auto w = t.rotate(step);
		for (int k = 0; k < 3; ++k) o.local.t[size_t(k)] += w[size_t(k)];
	}
	if (f > 0) {
		// PointZByRotationDeg: turn about Y towards the target by at most f degrees.
		float want = std::atan2(n[0], n[2]) * 57.29578f;
		float turn = std::clamp(want, -f, f) * 0.017453292f, cs = std::cos(turn), sn = std::sin(turn);
		de::Transform yaw;
		yaw.r = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
		o.local = de::Transform{yaw.then(de::Transform{o.local.r, {0, 0, 0}}).r, o.local.t};
	}
}

void register_combat_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };
	auto info_of = [](const NativeContext &c) { return c.self_handle(); };
	auto vec_at = [](const NativeContext &c, uint32_t addr) { return read_vec(c.mem(), addr); };

	// --- CAttackInfo ------------------------------------------------------------------------------
	reg("CObject::CAttackInfo::SetOwner(class CActor)", [=](const NativeContext &c) {
		g_attacks[info_of(c)].owner = c.arg_object(0);
	});
	reg("CObject::CAttackInfo::class CActor Owner()", [=](const NativeContext &c) {
		auto it = g_attacks.find(info_of(c));
		c.ret_object(it == g_attacks.end() ? nullptr : it->second.owner);
	});
	// DE_CAttackInfo::Prepare (0x35016a80): new attack id; name from the preparing method.
	auto prepare = [=](const NativeContext &c, uint32_t arg) {
		uint32_t a = info_of(c);
		AttackState &st = g_attacks[a];
		c.mem().set<float>(a + kMomentumAdjust, -99999.0f);
		c.mem().set<float>(a + kGuideToLastHit, 1.0f);
		c.mem().set<uint8_t>(a + kFlags, 0);
		c.mem().set<int32_t>(a + kMaxNumberOfHits, 0);
		st.id = g_next_attack_id++;
		auto *m = c.ex.current_method();
		st.name = std::to_string(m ? m->index : 0) + std::to_string(st.owner ? st.owner->handle : 0) +
		          (arg ? std::to_string(arg) : std::string());
		st.victims.clear();
	};
	reg("CObject::CAttackInfo::Prepare()", [=](const NativeContext &c) { prepare(c, 0); });
	reg("CObject::CAttackInfo::Prepare(dword)", [=](const NativeContext &c) { prepare(c, c.arg<uint32_t>(0)); });
	reg("CObject::CAttackInfo::class CActor TestForConnectedAttack(class CCollisionTester)", [=](const NativeContext &c) {
		uint32_t a = info_of(c);
		AttackState &st = g_attacks[a];
		auto it = g_testers.find(c.arg<uint32_t>(0));
		if (it == g_testers.end()) {
			c.ret_u32(0);
			return;
		}
		run_tester(c.world, it->second);
		for (auto &col : it->second.results) {
			if (col.owner == st.owner) continue;
			if (add_potential_victim(c.world, a, st, col.owner)) {
				write_vec(c.mem(), a + kConnectPoint, col.point);
				if (getenv("OPENBG_DEBUG_HITS")) {
					fprintf(stderr, "hit frame %u: %s -> %s (%s touched %s)\n", c.world.frame(),
					        st.owner ? st.owner->name.c_str() : "?", col.owner->name.c_str(), col.source->name.c_str(),
					        col.touched->name.c_str());
				}
				c.ret_object(col.owner);
				return;
			}
		}
		c.ret_u32(0);
	});
	reg("CObject::CAttackInfo::bool AddPotentialVictim(class CActor)", [=](const NativeContext &c) {
		uint32_t a = info_of(c);
		auto *v = c.arg_object(0);
		c.ret_bool(v && add_potential_victim(c.world, a, g_attacks[a], v));
	});
	// AddPotentialVictimFromHitStreamCaller (OMF 0x350127e0): the stream's hit count goes into
	// m_MaxNumberOfHits (+0xc0), then AddPotentialVictim.
	reg("CObject::CAttackInfo::bool AddPotentialVictimFromHitStream(class CActor,int)", [=](const NativeContext &c) {
		uint32_t a = info_of(c);
		auto *v = c.arg_object(0);
		if (a) c.world.memory().set<int32_t>(a + kMaxNumberOfHits, c.arg<int32_t>(1));
		c.ret_bool(v && add_potential_victim(c.world, a, g_attacks[a], v));
	});
	auto connected_victim = [](uint32_t a) -> GameObject * {
		auto it = g_attacks.find(a);
		if (it == g_attacks.end()) return nullptr;
		for (auto &h : g_hits) {
			if (h.id == it->second.id && h.owner == it->second.owner) return h.victim;
		}
		return nullptr;
	};
	reg("CObject::CAttackInfo::bool AttackConnected()", [=](const NativeContext &c) { c.ret_bool(connected_victim(info_of(c)) != nullptr); });
	reg("CObject::CAttackInfo::class CActor AttackConnectedVictim()", [=](const NativeContext &c) { c.ret_object(connected_victim(info_of(c))); });
	reg("CObject::CAttackInfo::bool AttackConnectedWithEnemy()", [=](const NativeContext &c) {
		auto it = g_attacks.find(info_of(c));
		GameObject *v = connected_victim(info_of(c));
		c.ret_bool(v && it != g_attacks.end() && v != it->second.owner && v->fighter);
	});
	reg("CObject::CAttackInfo::ClearHit(class CActor)", [=](const NativeContext &c) {
		auto it = g_attacks.find(info_of(c));
		auto *v = c.arg_object(0);
		if (it == g_attacks.end() || !v) return;
		auto &names = g_hit_names[v];
		names.erase(std::remove(names.begin(), names.end(), it->second.name), names.end());
	});
	// DE_CAttackInfo::CloseHitStream(n) (0x350168a0): every fighter victim gets this attack's hit
	// names 0..n-1, so the rest of a multi-hit stream misses it until it resets its hit list.
	reg("CObject::CAttackInfo::CloseHitStream(dword)", [=](const NativeContext &c) {
		auto it = g_attacks.find(info_of(c));
		if (it == g_attacks.end()) return;
		int32_t n = c.arg<int32_t>(0);
		for (GameObject *v : it->second.victims) {
			if (!v || !v->fighter) continue;
			auto &names = g_hit_names[v];
			for (int32_t i = 0; i < n; ++i) {
				std::string name = it->second.name + std::to_string(i);
				if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
			}
		}
	});
	reg("COMF2Fighter::ResetCurrentHitList()", [](const NativeContext &c) {
		if (auto *o = c.self()) g_hit_names[o].clear();
	});
	// Flag bits at +0xc4 (Set*/Is*Caller).
	const std::vector<std::pair<const char *, uint8_t>> flags = {{"Melee", 8}, {"Super", 4}, {"ForceComboHit", 0x20}};
	for (auto &[n, bit] : flags) {
		uint8_t b = bit;
		reg(std::string("CObject::CAttackInfo::Set") + n + "()", [=](const NativeContext &c) {
			uint32_t a = info_of(c);
			c.mem().set<uint8_t>(a + kFlags, c.mem().get<uint8_t>(a + kFlags) | b);
		});
		reg(std::string("CObject::CAttackInfo::bool Is") + n + "()", [=](const NativeContext &c) {
			c.ret_bool((c.mem().get<uint8_t>(info_of(c) + kFlags) & b) != 0);
		});
	}
	reg("CObject::CAttackInfo::SetNoEvade()", [=](const NativeContext &c) {  // SetNoEvadeCaller 0x35013070
		uint32_t a = info_of(c);
		c.mem().set<uint8_t>(a + kFlags, c.mem().get<uint8_t>(a + kFlags) | 0x10);
	});
	reg("CObject::CAttackInfo::bool IsBlocked()", [=](const NativeContext &c) { c.ret_bool(c.mem().get<uint8_t>(info_of(c) + kFlags) & 1); });
	reg("CObject::CAttackInfo::SetBlocked(bool)", [=](const NativeContext &c) {
		uint32_t f = info_of(c) + kFlags;
		c.mem().set<uint8_t>(f, uint8_t((c.mem().get<uint8_t>(f) & ~1u) | (c.arg<uint32_t>(0) ? 1 : 0)));
	});
	// TestForBlockCaller: anything that is not a fighter blocks whatever it's asked about.
	reg("CObject::CAttackInfo::bool TestForBlock(class CObject)", [=](const NativeContext &c) {
		uint32_t f = info_of(c) + kFlags;
		c.mem().set<uint8_t>(f, c.mem().get<uint8_t>(f) | 1);
		c.ret_bool(true);
	});
	// TestForFighterBlock (OMF 0x35017560) by attack direction: the owner's facing (world Z axis),
	// toward the defender (Radiate), an explicit angle, or the transferred momentum. Sets the
	// blocked flag and returns it.
	auto block_test = [=](const NativeContext &c, std::array<float, 3> dir, uint16_t zones) {
		uint32_t a = info_of(c);
		auto it = g_attacks.find(a);
		bool blocked = fighter_blocks(c.arg_object(0), it == g_attacks.end() ? nullptr : it->second.owner, dir, zones);
		uint32_t f = a + kFlags;
		c.mem().set<uint8_t>(f, uint8_t((c.mem().get<uint8_t>(f) & ~1u) | (blocked ? 1 : 0)));
		return blocked;
	};
	auto owner_of = [=](const NativeContext &c) -> GameObject * {
		auto it = g_attacks.find(info_of(c));
		return it == g_attacks.end() ? nullptr : it->second.owner;
	};
	reg("CObject::CAttackInfo::bool TestForFighterBlock(class CObject,word)", [=](const NativeContext &c) {
		GameObject *o = owner_of(c);
		c.ret_bool(block_test(c, o ? o->world().rotate({0, 0, 1}) : std::array<float, 3>{0, 0, 1}, uint16_t(c.arg<uint32_t>(1))));
	});
	reg("CObject::CAttackInfo::bool TestForFighterBlockRadiate(class CObject,word)", [=](const NativeContext &c) {
		GameObject *o = owner_of(c), *d = c.arg_object(0);
		std::array<float, 3> dir{0, 0, 1};
		if (o && d) {
			auto p = o->world().t, q = d->world().t;
			dir = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
		}
		c.ret_bool(block_test(c, dir, uint16_t(c.arg<uint32_t>(1))));
	});
	reg("CObject::CAttackInfo::bool TestForFighterBlock(class CObject,class CVector,word)", [=](const NativeContext &c) {
		c.ret_bool(block_test(c, vec_at(c, c.arg<uint32_t>(1)), uint16_t(c.arg<uint32_t>(2))));
	});
	reg("CObject::CAttackInfo::bool TestForFighterBlockByTransferedMomentum(class CObject,word)", [=](const NativeContext &c) {
		c.ret_bool(block_test(c, vec_at(c, info_of(c) + kTransferedMomentum), uint16_t(c.arg<uint32_t>(1))));
	});
	reg("CObject::CAttackInfo::bool IsNoEvade()", [=](const NativeContext &c) { c.ret_bool(c.mem().get<uint8_t>(info_of(c) + kFlags) & 0x10); });
	reg("CObject::CAttackInfo::bool IsNoReplicate()", [](const NativeContext &c) { c.ret_bool(false); });
	// IsPopupCaller: reaction 100/101, or a reel direction rising more than 0.04.
	reg("CObject::CAttackInfo::bool IsPopup()", [=](const NativeContext &c) {
		uint32_t a = info_of(c);
		uint32_t r = c.mem().get<uint32_t>(a + kReactionType);
		c.ret_bool(r == 0x65 || r == 100 || c.mem().get<float>(a + kReelDirection + 4) > 0.04f);
	});
	// AddAttackType*Caller / IsAttackType*Caller (OMF 0x350130a0..0x35013210): bits of +0x4c;
	// ClearAttackTypeCaller zeroes it.
	for (auto &[n, bit] : std::vector<std::pair<const char *, uint32_t>>{{"Standard", 2}, {"Projectile", 4}, {"Cold", 8}, {"Heat", 0x10},
	                                                                     {"Electrical", 0x20}, {"Gravitational", 0x40}, {"TimeStop", 0x80},
	                                                                     {"NoReplicate", 0x80000000u}}) {
		uint32_t b = bit;
		reg(std::string("CObject::CAttackInfo::bool IsAttackType") + n + "()", [=](const NativeContext &c) {
			c.ret_bool((c.mem().get<uint32_t>(info_of(c) + kAttackType) & b) != 0);
		});
		reg(std::string("CObject::CAttackInfo::AddAttackType") + n + "()", [=](const NativeContext &c) {
			uint32_t a = info_of(c);
			c.mem().set<uint32_t>(a + kAttackType, c.mem().get<uint32_t>(a + kAttackType) | b);
		});
	}
	reg("CObject::CAttackInfo::ClearAttackType()", [=](const NativeContext &c) { c.mem().set<uint32_t>(info_of(c) + kAttackType, 0); });
	reg("CObject::CAttackInfo::SetReaction(dword)", [=](const NativeContext &c) { c.mem().set<uint32_t>(info_of(c) + kReactionType, c.arg<uint32_t>(0)); });
	reg("CObject::CAttackInfo::SetEffectType(dword)", [=](const NativeContext &c) { c.mem().set<uint32_t>(info_of(c) + kEffectType, c.arg<uint32_t>(0)); });
	reg("CObject::CAttackInfo::GainEnergy(float)", [=](const NativeContext &c) { c.mem().set<float>(info_of(c) + kEnergyDamage, -c.arg<float>(0)); });
	// StartSimpleAttackCaller: reset the stats, then damage = stun = arg0, recovery = arg1.
	reg("CObject::CAttackInfo::StartSimpleAttack(float,float)", [=](const NativeContext &c) {
		uint32_t a = info_of(c);
		c.world.reset_attack_info(a);
		c.mem().set<float>(a + kDamage, c.arg<float>(0));
		c.mem().set<float>(a + kStunDamage, c.arg<float>(0));
		c.mem().set<float>(a + kRecoveryTime, c.arg<float>(1));
	});
	(void)vec_at;
	(void)kUser;

	// --- Collision testers -----------------------------------------------------------------------
	reg("CScene::class CCollisionTester NewCollisionTester()", [](const NativeContext &c) {
		uint32_t h = c.mem().alloc(16);
		g_testers[h];
		c.ret_u32(h);
	});
	reg("CCollisionTester::SetGroupFilter(dword)", [](const NativeContext &c) { g_testers[c.self_handle()].groups = c.arg<uint32_t>(0); });
	reg("CCollisionTester::NeedTouchPoint()", [](const NativeContext &) {});
	reg("CCollisionTester::AddSourceBranch(class CActor)", [](const NativeContext &c) {
		if (auto *a = c.arg_object(0)) g_testers[c.self_handle()].sources.push_back(a);
	});
	// DE_CCollisionTester::AddSource (Core 0x7006a090): that affector alone, not its branch.
	reg("CCollisionTester::AddSource(class CAffector)", [](const NativeContext &c) {
		if (auto *a = c.arg_object(0)) g_testers[c.self_handle()].singles.push_back(a);
	});
	reg("CCollisionTester::AddNotify(class CClassExecutorInstance,Method(class CCollisionEvent))", [](const NativeContext &c) {
		g_testers[c.self_handle()].notify.push_back({c.world.brain_of(c.ex), c.arg<uint32_t>(1)});
	});
	reg("CCollisionTester::Activate()", [](const NativeContext &c) {
		g_testers[c.self_handle()].active = true;
		c.world.activate_tester(c.self_handle());
	});
	reg("CCollisionTester::SetAttachedForce(class CForce)", [](const NativeContext &c) { g_testers[c.self_handle()].force = c.arg_object(0); });
	reg("CCollisionTester::NeedSurfaceNormal()", [](const NativeContext &) {});
	reg("CCollisionTester::Deactivate()", [](const NativeContext &c) { g_testers[c.self_handle()].active = false; });
	// SetupArenaSurfaceHitCaller (OMF): an arena-surface-hit event (type 0xb at +0x1c) carrying
	// the results (+0x44) and the power (+0x48). The projectiles hand it to the surface's owner
	// with ProcessEvent; nothing in the DLLs or scripts handles type 0xb.
	reg("COMF2Event::SetupArenaSurfaceHit(class CCollisionResults,float)", [](const NativeContext &c) {
		if (uint32_t e = c.self_handle()) {
			c.mem().set<uint32_t>(e + 0x44, c.arg<uint32_t>(0));
			c.mem().set<float>(e + 0x48, c.arg<float>(1));
			c.mem().set<uint32_t>(e + 0x1c, 0xb);
		}
	});
	// CCollisionEvent (the active tester's results, handed to its notify methods).
	auto event_tester = [](const NativeContext &c) -> Tester * {
		for (auto &[h, t] : g_testers) {
			if (t.event && t.event == c.self_handle()) return &t;
		}
		return nullptr;
	};
	reg("CCollisionEvent::int NumCollisions()", [=](const NativeContext &c) {
		Tester *t = event_tester(c);
		c.ret_u32(t ? uint32_t(t->results.size()) : 0);
	});
	reg("CCollisionEvent::class CAffector TouchAffector(int)", [=](const NativeContext &c) {
		Tester *t = event_tester(c);
		int32_t i = c.arg<int32_t>(0);
		c.ret_object(t && i >= 0 && size_t(i) < t->results.size() ? t->results[size_t(i)].touched : nullptr);
	});
	// AddTestAffectorsCaller (Core): the other tester's affectors are tested by this one too.
	reg("CCollisionTester::AddTestAffectors(class CCollisionTester)", [](const NativeContext &c) {
		uint32_t o = c.arg<uint32_t>(0);
		if (!o || !g_testers.count(o)) return;
		Tester &t = g_testers[c.self_handle()];
		for (GameObject *b : g_testers[o].sources) t.sources.push_back(b);
		for (GameObject *a : g_testers[o].singles) t.singles.push_back(a);
	});
	// NeedPushBackNormal / NeedSurfaceNormal only ask for what every test here computes anyway.
	reg("CCollisionTester::NeedPushBackNormal()", [](const NativeContext &) {});
	// CollisionResultsCaller (+0x44): the results the event reports.
	reg("CCollisionEvent::class CCollisionResults CollisionResults()", [=](const NativeContext &c) {
		Tester *t = event_tester(c);
		if (!t) {
			c.ret_u32(0);
			return;
		}
		if (!t->results_handle) {
			t->results_handle = c.mem().alloc(16);
			for (auto &[h, tt] : g_testers) {
				if (&tt == t) c.mem().set<uint32_t>(t->results_handle, h);
			}
		}
		c.ret_u32(t->results_handle);
	});
	reg("CCollisionTester::bool Test()", [](const NativeContext &c) {
		Tester &t = g_testers[c.self_handle()];
		run_tester(c.world, t);
		static const bool debug = getenv("OPENBG_DEBUG_TESTER") != nullptr;
		if (debug) {
			GameObject *who = c.owner();
			fprintf(stderr, "[tester %u] %s: %zu source branch(es) %s, groups %#x -> %zu\n", c.world.frame(), who ? who->name.c_str() : "?", t.sources.size() + t.singles.size(),
			        !t.sources.empty() ? t.sources[0]->name.c_str() : !t.singles.empty() ? t.singles[0]->name.c_str() : "-", t.groups, t.results.size());
			for (auto &r : t.results) {
				fprintf(stderr, "[tester %u]   touched %s (owner %s, parent %s)\n", c.world.frame(), r.touched ? r.touched->name.c_str() : "-",
				        r.owner ? r.owner->name.c_str() : "-", r.owner && r.owner->parent ? r.owner->parent->name.c_str() : "-");
			}
		}
		c.ret_bool(!t.results.empty());
	});
	reg("CCollisionTester::class CCollisionResults Results()", [](const NativeContext &c) {
		Tester &t = g_testers[c.self_handle()];
		if (!t.results_handle) {
			t.results_handle = c.mem().alloc(16);
			c.mem().set<uint32_t>(t.results_handle, c.self_handle());
		}
		c.ret_u32(t.results_handle);
	});
	auto tester_of_results = [](const NativeContext &c) -> Tester * {
		uint32_t r = c.self_handle();
		if (!r) return nullptr;
		auto it = g_testers.find(c.mem().get<uint32_t>(r));
		return it == g_testers.end() ? nullptr : &it->second;
	};
	// ApplyForceCollisionResultsCaller (Core 0x7007abb0) -> DE_CCollisionResults::ApplyForce
	// (0x7007bde0): a collision event carrying the force to collision i's touched affector (its
	// materials process it), or to every collision's with i = -1.
	reg("CAffector::CCollisionResults::ApplyForce(class CForce,int)", [=](const NativeContext &c) {
		Tester *t = tester_of_results(c);
		GameObject *force = c.arg_object(0);
		int32_t i = c.arg<int32_t>(1);
		if (!t || !force) return;
		auto results = t->results;  // a material may run scripts that test again
		for (size_t k = 0; k < results.size(); ++k) {
			if (i != -1 && size_t(i) != k) continue;
			const Collision &r = results[k];
			Contact ct;
			ct.source = r.source;
			ct.touched = r.touched;
			ct.point = r.point;
			ct.normal = ct.face_normal = r.normal;
			ct.distance = r.distance;
			c.world.forces->deliver(*force, ct);
		}
	});
	reg("CAffector::CCollisionResults::int NumCollisions()", [=](const NativeContext &c) {
		Tester *t = tester_of_results(c);
		c.ret_u32(t ? uint32_t(t->results.size()) : 0);
	});
	reg("CAffector::CCollisionResults::class CSingleCollisionInfo Collision(int)", [=](const NativeContext &c) {
		Tester *t = tester_of_results(c);
		int32_t i = c.arg<int32_t>(0);
		if (!t || i < 0 || size_t(i) >= t->results.size()) {
			c.ret_u32(0);
			return;
		}
		uint32_t h = c.mem().alloc(32);
		g_collision_infos[h] = {c.mem().get<uint32_t>(c.self_handle()), size_t(i)};
		c.ret_u32(h);
	});
	auto collision_of = [](const NativeContext &c) -> Collision * {
		auto it = g_collision_infos.find(c.self_handle());
		if (it == g_collision_infos.end()) return nullptr;
		auto t = g_testers.find(it->second.first);
		if (t == g_testers.end() || it->second.second >= t->second.results.size()) return nullptr;
		return &t->second.results[it->second.second];
	};
	reg("CAffector::CCollisionResults::CSingleCollisionInfo::class CActor OwnerActor()", [=](const NativeContext &c) {
		Collision *col = collision_of(c);
		c.ret_object(col ? col->owner : nullptr);
	});
	reg("CAffector::CCollisionResults::CSingleCollisionInfo::class CActor TouchedOwner()", [=](const NativeContext &c) {
		Collision *col = collision_of(c);
		c.ret_object(col ? col->owner : nullptr);
	});
	reg("CAffector::CCollisionResults::CSingleCollisionInfo::class CAffector TouchAffector()", [=](const NativeContext &c) {
		Collision *col = collision_of(c);
		c.ret_object(col ? col->touched : nullptr);
	});
	for (const char *k : {"CAffector::CCollisionResults::CSingleCollisionInfo::class CVector TouchPoint()",
	                      "CAffector::CCollisionResults::CSingleCollisionInfo::class CVector SurfaceNormal()"}) {
		bool point = strstr(k, "TouchPoint") != nullptr;
		reg(k, [=](const NativeContext &c) {
			Collision *col = collision_of(c);
			uint32_t v = c.mem().alloc(12);
			write_vec(c.mem(), v, col ? (point ? col->point : col->normal) : std::array<float, 3>{0, 0, 0});
			c.ret_u32(v);
		});
	}
	// CollisionResultsGetAverageSurfaceNormalCaller: the touched surfaces' normals summed and
	// normalised; false (vector untouched) with no collisions.
	reg("CAffector::CCollisionResults::bool GetAverageSurfaceNormal(class CVector)", [=](const NativeContext &c) {
		Tester *t = tester_of_results(c);
		uint32_t v = c.arg<uint32_t>(0);
		if (!v || !t || t->results.empty()) {
			c.ret_bool(false);
			return;
		}
		std::array<float, 3> n{0, 0, 0};
		for (int i = int(t->results.size()) - 1; i >= 0; --i) {
			for (int k = 0; k < 3; ++k) n[size_t(k)] += t->results[size_t(i)].face_normal[size_t(k)];
		}
		float l = length(n);
		if (l > 0) n = {n[0] / l, n[1] / l, n[2] / l};
		write_vec(c.mem(), v, n);
		c.ret_bool(true);
	});
	// CSimplePhysics::CollisionResults: the last collision tests' results, as a results block.
	reg("CSimplePhysics::class CCollisionResults CollisionResults()", [](const NativeContext &c) {
		GameObject *p = c.self();
		if (!p) {
			c.ret_u32(0);
			return;
		}
		static std::map<const GameObject *, uint32_t> keys;
		uint32_t &key = keys[p];
		if (!key) key = c.mem().alloc(4);
		Tester &t = g_testers[key];
		t.results.clear();
		for (auto &ct : c.world.simple_physics->state(*p).contacts) {
			Collision col;
			col.source = ct.source;
			col.touched = ct.touched;
			col.owner = c.world.affector_owner(ct.touched);
			col.point = ct.point;
			col.normal = ct.normal;
			col.face_normal = ct.face_normal;
			col.distance = ct.distance;
			t.results.push_back(col);
		}
		if (!t.results_handle) {
			t.results_handle = c.mem().alloc(16);
			c.mem().set<uint32_t>(t.results_handle, key);
		}
		c.ret_u32(t.results_handle);
	});
	for (const char *k : {"CAffector::CCollisionResults::NeedSurfaceNormal()", "CAffector::CCollisionResults::NeedTouchPoint()"}) {
		reg(k, [](const NativeContext &) {});
	}
	// DE_CCollisionResults::SortByTouchDistance (Core): a bubble sort by the touch points' distance
	// from the point, swapping a pair only when the first is more than 0.005 farther.
	reg("CAffector::CCollisionResults::SortByTouchDistance(class CVector)", [=](const NativeContext &c) {
		Tester *t = tester_of_results(c);
		uint32_t v = c.arg<uint32_t>(0);
		if (!t || !v) return;
		auto p = read_vec(c.mem(), v);
		auto d = [&](const Collision &x) { return length({p[0] - x.point[0], p[1] - x.point[1], p[2] - x.point[2]}); };
		for (bool swapped = true; swapped && t->results.size() > 1;) {
			swapped = false;
			for (size_t i = 1; i < t->results.size(); ++i) {
				if (d(t->results[i]) + 0.005f < d(t->results[i - 1])) {
					std::swap(t->results[i], t->results[i - 1]);
					swapped = true;
				}
			}
		}
	});
	// A script's own results block (instance CCollisionResults) names a private tester at +0.
	auto own_results = [](const NativeContext &c, uint32_t results) -> Tester * {
		if (!results) return nullptr;
		uint32_t key = c.mem().get<uint32_t>(results);
		if (!key || !g_testers.count(key)) {
			key = c.mem().alloc(4);
			c.mem().set<uint32_t>(results, key);
		}
		return &g_testers[key];
	};
	reg("CAffector::CCollisionResults::SetGroupsToTest(dword)", [=](const NativeContext &c) {
		if (Tester *t = own_results(c, c.self_handle())) t->groups = c.arg<uint32_t>(0);
	});
	reg("CAffector::CCollisionResults::AddGroupsToTest(dword)", [=](const NativeContext &c) {  // AddGroupsToTestCaller: +0x34 |=
		if (Tester *t = own_results(c, c.self_handle())) t->groups |= c.arg<uint32_t>(0);
	});
	reg("CAffector::CCollisionResults::NoRepeatOwners()", [=](const NativeContext &c) {
		if (Tester *t = own_results(c, c.self_handle())) t->no_repeat_owners = true;
	});
	reg("CAffector::CCollisionResults::ResetTouching()", [=](const NativeContext &c) {
		if (Tester *t = own_results(c, c.self_handle())) t->results.clear();
	});
	// DE_CAffector::IsTouching(results) (vtable +0x168): this affector against the results' groups.
	auto is_touching = [=](const NativeContext &c, GameObject *a, uint32_t results) {
		Tester *t = own_results(c, results);
		if (!a || !t) return false;
		t->results.clear();
		std::vector<const GameObject *> owners;
		for (auto &ct : touches(c.world, *a, t->groups)) {
			GameObject *owner = c.world.affector_owner(ct.touched);
			if (t->no_repeat_owners) {
				if (std::find(owners.begin(), owners.end(), owner) != owners.end()) continue;
				owners.push_back(owner);
			}
			Collision col;
			col.source = a;
			col.touched = ct.touched;
			col.owner = owner;
			col.point = ct.point;
			col.normal = ct.normal;
			col.face_normal = ct.face_normal;
			col.distance = ct.distance;
			t->results.push_back(col);
		}
		return !t->results.empty();
	};
	reg("CAffector::bool IsTouching(class CCollisionResults)", [=](const NativeContext &c) {
		c.ret_bool(is_touching(c, c.self(), c.arg<uint32_t>(0)));
	});

	// The scene's own test affectors (DE_CScene +0x150 sphere, +0x154 cone), placed and sized for
	// each query and then asked IsTouching(results). They are not in the scene themselves.
	auto test_sphere = [](World &w) {
		GameObject *g = w.create("SceneTestSphere", "Actor\\Affector\\Sphere");
		g->shape = GameObject::Shape::Sphere;
		return g;
	};
	auto test_cone = [](World &w) {
		GameObject *g = w.create("SceneTestCone", "Actor\\Affector\\Cone");
		g->shape = GameObject::Shape::Cone;
		return g;
	};
	// TestSphereCaller (Core 0x7006e290): the sphere at the position with that falloff.
	reg("CScene::bool TestSphere(class CCollisionResults,class CVector,float)", [=](const NativeContext &c) {
		GameObject *s = test_sphere(c.world);
		s->local = {};
		s->local.t = read_vec(c.mem(), c.arg<uint32_t>(1));
		s->falloff = c.arg<float>(2);
		c.ret_bool(is_touching(c, s, c.arg<uint32_t>(0)));
	});
	// DE_CScene::TestSpherePath (0x70072f80): the sphere stepped radius/2 at a time along the
	// direction (backwards for a negative distance), from one step out until the distance is used.
	reg("CScene::bool TestSpherePath(class CCollisionResults,class CVector,float,class CVector,float)", [=](const NativeContext &c) {
		GameObject *s = test_sphere(c.world);
		float r = c.arg<float>(2), dist = c.arg<float>(4), step = r * 0.5f;
		std::array<float, 3> d = read_vec(c.mem(), c.arg<uint32_t>(3));
		float l = length(d);
		if (l > 0) d = {d[0] / l, d[1] / l, d[2] / l};
		if (dist < 0) dist = -dist, step = -step;
		d = {d[0] * step, d[1] * step, d[2] * step};
		step = std::fabs(step);
		s->local = {};
		s->local.t = read_vec(c.mem(), c.arg<uint32_t>(1));
		s->falloff = r;
		do {
			for (int k = 0; k < 3; ++k) s->local.t[size_t(k)] += d[size_t(k)];
			if (is_touching(c, s, c.arg<uint32_t>(0))) {
				c.ret_bool(true);
				return;
			}
			dist -= step;
		} while (dist > 0);
		c.ret_bool(false);
	});
	// TestConeCaller (0x7006e3d0): the cone at the position and orientation, far end = length.
	reg("CScene::bool TestCone(class CCollisionResults,class CVector,class CCoordinateSystem,float,float)", [=](const NativeContext &c) {
		GameObject *k = test_cone(c.world);
		k->local.t = read_vec(c.mem(), c.arg<uint32_t>(1));
		if (uint32_t o = c.arg<uint32_t>(2)) {
			for (int i = 0; i < 9; ++i) k->local.r[size_t(i)] = c.mem().get<float>(o + uint32_t(i) * 4);
		}
		k->penumbra = c.arg<float>(3);
		k->far_end = c.arg<float>(4);
		c.ret_bool(is_touching(c, k, c.arg<uint32_t>(0)));
	});

	// DE_CScene::TestLineSegment(results, from, to) (Core 0x700731b0): the scene's cone at `from`
	// pointing at `to`, penumbra 0 and far end the length, asked IsTouching(results): every
	// affector in the results' groups the segment meets, in scene order.
	reg("CScene::bool TestLineSegment(class CCollisionResults,class CVector,class CVector)", [=](const NativeContext &c) {
		uint32_t results = c.arg<uint32_t>(0);
		auto a = read_vec(c.mem(), c.arg<uint32_t>(1)), b = read_vec(c.mem(), c.arg<uint32_t>(2));
		Tester *t = own_results(c, results);
		if (!t) {
			c.ret_bool(false);
			return;
		}
		t->results.clear();
		for (auto &ct : segment_hits(c.world, a, b, t->groups)) {
			Collision col;
			col.touched = ct.touched;
			col.owner = c.world.affector_owner(ct.touched);
			col.point = ct.point;
			col.normal = ct.normal;
			col.face_normal = ct.face_normal;
			col.distance = ct.distance;
			t->results.push_back(col);
		}
		c.ret_bool(!t->results.empty());
	});


	// --- Affectors --------------------------------------------------------------------------------
	// ActivateCaller (Core 0x7008e3e0) -> DE_CActor::SetActive (vtable +0x124, 0x700902f0): a parked
	// actor rejoins its parent's children and the scene (Scene().Add: prepared, scheduled, its
	// affectors at the list's end). Deactivate -> SetInnactive (0x700903e0): parked, out of the
	// scene (Scene().Remove), keeping its own executors' state.
	reg("CActor::Activate()", [](const NativeContext &c) {
		GameObject *o = c.self();
		if (!o || o->active) return;
		bool was = in_scene(*o);
		o->active = true;
		if (!was && in_scene(*o)) c.world.prepare_branch(o);
	});
	reg("CActor::Deactivate()", [](const NativeContext &c) { if (auto *o = c.self()) o->active = false; });
	reg("CAffector::SetOwnerActor(class CActor)", [](const NativeContext &c) { if (auto *o = c.self()) o->owner_actor = c.arg_object(0); });
	reg("CAffector::class CActor OwnerActor()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_object(o ? c.world.affector_owner(o) : nullptr);
	});
	reg("CSphereAffector::SetHotspot(float)", [](const NativeContext &c) { if (auto *o = c.self()) o->hotspot = c.arg<float>(0); });
	reg("CSphereAffector::SetFalloff(float)", [](const NativeContext &c) { if (auto *o = c.self()) o->falloff = c.arg<float>(0); });
	// DE_CBoxAffector Set{Lower,Upper}{X,Y,Z}Caller (Core 0x7007df60..): plain stores to the
	// corners (+0x188.. lower, +0x194.. upper); Katana's projectile grows its hit box with them.
	for (int k = 0; k < 3; ++k) {
		std::string axis(1, char('X' + k));
		reg("CBoxAffector::SetLower" + axis + "(float)", [k](const NativeContext &c) { if (auto *o = c.self()) o->box_min[size_t(k)] = c.arg<float>(0); });
		reg("CBoxAffector::SetUpper" + axis + "(float)", [k](const NativeContext &c) { if (auto *o = c.self()) o->box_max[size_t(k)] = c.arg<float>(0); });
	}
	reg("CConeAffector::SetUmbra(float)", [](const NativeContext &c) { if (auto *o = c.self()) o->umbra = c.arg<float>(0); });
	reg("CConeAffector::SetPenumbra(float)", [](const NativeContext &c) { if (auto *o = c.self()) o->penumbra = c.arg<float>(0); });
	reg("CConeAffector::SetNear(float)", [](const NativeContext &c) { if (auto *o = c.self()) o->near_begin = o->near_end = c.arg<float>(0), o->far_begin = std::max(o->far_begin, o->near_end); });
	reg("CConeAffector::SetFar(float)", [](const NativeContext &c) { if (auto *o = c.self()) o->far_begin = o->far_end = c.arg<float>(0); });
	// FarCaller (Core 0x7007fb50): +0x180, the far end.
	reg("CConeAffector::float Far()", [](const NativeContext &c) { auto *o = c.self(); c.ret_f32(o ? o->far_end : 0); });

	// --- Get-hit support (OMF Dampen*Caller, Core orientation) ----------------------------------
	// IsStateAlterable folds into IsSimulating: true for a creature simulated here (the server).
	reg("CObject::bool IsStateAlterable()", [](const NativeContext &c) { c.ret_bool(!replay_driven(c.world, c.self())); });
	// IsAirStateCaller -> IsJumpingState (0x35029c60): the running state inherits the "Air" state.
	reg("COMF2Fighter::bool IsAirState()", [](const NativeContext &c) {
		auto *o = c.self();
		bool air = false;
		if (o && o->brain && o->brain->exec) {
			for (auto *m = o->brain->exec->current_state(); m && !air; m = m->parent) {
				air = m->cm->name == "Air()";
			}
		}
		c.ret_bool(air);
	});
	// Dampen*Momentum(k): scale the speed along those axes by (1 - speed x k), stopping at zero.
	reg("COMF2Fighter::DampenHorizontalMomentum(float)", [](const NativeContext &c) {
		auto *o = c.self();
		if (!o) return;
		float h = std::sqrt(o->momentum[0] * o->momentum[0] + o->momentum[2] * o->momentum[2]);
		float f = h * c.arg<float>(0);
		float k = f >= 1 ? 0 : 1 - f;
		o->momentum[0] *= k, o->momentum[2] *= k;
	});
	reg("COMF2Fighter::DampenVerticalMomentum(float)", [](const NativeContext &c) {  // 0x35022520
		auto *o = c.self();
		if (!o) return;
		float f = std::fabs(o->momentum[1]) * c.arg<float>(0);
		o->momentum[1] = f >= 1 ? 0 : (1 - f) * o->momentum[1];
	});
	reg("COMF2Fighter::DampenDownwardMomentum(float)", [](const NativeContext &c) {
		auto *o = c.self();
		if (!o || o->momentum[1] >= 0) return;
		float f = -o->momentum[1] * c.arg<float>(0);
		o->momentum[1] = f >= 1 ? 0 : (1 - f) * o->momentum[1];
	});
	reg("COMF2Fighter::DampenUpwardMomentum(float)", [](const NativeContext &c) {
		auto *o = c.self();
		if (!o || o->momentum[1] <= 0) return;
		float f = o->momentum[1] * c.arg<float>(0);
		o->momentum[1] = f >= 1 ? 0 : (1 - f) * o->momentum[1];
	});
	reg("COMF2Fighter::DampenMomentum(float)", [](const NativeContext &c) {
		auto *o = c.self();
		if (!o) return;
		float f = length(o->momentum) * c.arg<float>(0);
		float k = f >= 1 ? 0 : 1 - f;
		for (auto &v : o->momentum) v *= k;
	});
	reg("CActor::class CCoordinateSystem WorldOrientation()", [](const NativeContext &c) {
		auto *o = c.self();
		if (!o) {
			c.ret_u32(0);
			return;
		}
		if (!o->orientation_block) o->orientation_block = c.mem().alloc(36);
		auto r = o->world().r;
		for (int i = 0; i < 9; ++i) c.mem().set<float>(o->orientation_block + uint32_t(i) * 4, r[size_t(i)]);
		c.ret_u32(o->orientation_block);
	});

	// --- CAttackInfo directions (Setup*Caller: the vector in the given actor's frame) ---------------
	auto setup_dir = [](const char *key, uint32_t off, bool zero_y_if_zero) {
		return std::make_tuple(key, off, zero_y_if_zero);
	};
	for (auto [key, off, zy] : {setup_dir("CObject::CAttackInfo::SetupReelDirection(class CActor,float,float,float)", kReelDirection, false),
	                            setup_dir("CObject::CAttackInfo::SetupEffectDirection(class CActor,float,float,float)", kEffectDirection, false),
	                            setup_dir("CObject::CAttackInfo::SetupEffectDirection2(class CActor,float,float,float)", kEffectDirection2, false),
	                            setup_dir("CObject::CAttackInfo::SetupTransferedMomentum(class CActor,float,float,float)", kTransferedMomentum, true)}) {
		uint32_t o = off;
		bool z = zy;
		reg(key, [=](const NativeContext &c) {
			auto *a = c.arg_object(0);
			std::array<float, 3> v{c.arg<float>(1), c.arg<float>(2), c.arg<float>(3)};
			auto r = a ? a->world().rotate(v) : v;
			if (z && v[1] == 0) r[1] = 0;
			if (o == kTransferedMomentum && getenv("OPENBG_DEBUG_HITS"))
				fprintf(stderr, "[momentum-setup] transfered (%.2f %.2f %.2f) in %s\n", v[0], v[1], v[2], a ? a->name.c_str() : "-");
			write_vec(c.mem(), c.self_handle() + o, r);
		});
	}
	// SetupTransferedMomentumForVacuum (OMF 0x35013460): the transferred momentum pulls the
	// connected victim towards the creator. The direction from the creator to the victim, in the
	// creator's frame, is normalised, scaled per axis by (x, y, z), reversed and turned back to
	// the world; a y of 0 leaves no vertical part.
	reg("CObject::CAttackInfo::SetupTransferedMomentumForVacuum(class CActor,float,float,float)", [=](const NativeContext &c) {
		GameObject *creator = c.arg_object(0), *victim = connected_victim(c.self_handle());
		if (!creator || !victim) return;
		de::Transform cw = creator->world();
		auto d = cw.inverse_rotate({victim->world().t[0] - cw.t[0], victim->world().t[1] - cw.t[1], victim->world().t[2] - cw.t[2]});
		float l = length(d);
		if (l > 0) d = {d[0] / l, d[1] / l, d[2] / l};
		float y = c.arg<float>(2);
		auto r = cw.rotate({-d[0] * c.arg<float>(1), -d[1] * y, -d[2] * c.arg<float>(3)});
		if (y == 0) r[1] = 0;
		write_vec(c.mem(), c.self_handle() + kTransferedMomentum, r);
	});
	// SetupKnockToDestination (0x35017280): momentum (per frame: per ms x 16) that carries the victim
	// to a point given in the attacker's frame within `ms`, plus lift against gravity.
	reg("CObject::CAttackInfo::SetupKnockToDestination(class CActor,class CVector,float,float)", [](const NativeContext &c) {
		uint32_t a = c.self_handle();
		auto it = g_attacks.find(a);
		auto *victim = c.arg_object(0);
		float ms = c.arg<float>(2);
		if (it == g_attacks.end() || !it->second.owner || !victim || ms == 0) return;
		auto dest = it->second.owner->world().apply(read_vec(c.mem(), c.arg<uint32_t>(1)));
		auto vp = victim->world().t;
		std::array<float, 3> m{(dest[0] - vp[0]) / ms * 16, (dest[1] - vp[1]) / ms * 16, (dest[2] - vp[2]) / ms * 16};
		m[1] += ms / 16 * 0.037f;
		if (getenv("OPENBG_DEBUG_HITS")) fprintf(stderr, "[momentum-setup] knock-to (%.2f %.2f %.2f) over %.1f ms\n", m[0], m[1], m[2], ms);
		write_vec(c.mem(), a + kTransferedMomentum, m);
	});
	// AttackOrientation (0x35016190): where around the victim the reel direction points, 0..1.
	reg("CObject::CAttackInfo::float AttackOrientation()", [](const NativeContext &c) {
		uint32_t a = c.self_handle();
		float v = (std::atan2(c.mem().get<float>(a + kReelDirection + 8), c.mem().get<float>(a + kReelDirection)) - 1.5707964f) /
		              6.2831855f + 0.75f;
		if (v > 1) v -= 1;
		if (v < 0) v += 1;
		c.ret_f32(v);
	});
	// IsHighHit (0x35016230): the connect point more than 23 units up in the victim's frame.
	reg("CObject::CAttackInfo::bool IsHighHit(class CActor)", [](const NativeContext &c) {
		auto *v = c.arg_object(0);
		if (!v) {
			c.ret_bool(false);
			return;
		}
		auto p = read_vec(c.mem(), c.self_handle() + kConnectPoint);
		de::Transform t = v->world();
		auto l = t.inverse_rotate({p[0] - t.t[0], p[1] - t.t[1], p[2] - t.t[2]});
		c.ret_bool(l[1] > 23.0f);
	});

	// --- Hit bookkeeping and auto-guide (DE_COMF2Fighter) ------------------------------------------
	reg("COMF2Fighter::SetLastActorHit(class CActor)", [](const NativeContext &c) { if (auto *o = c.self()) o->last_hit = c.arg_object(0); });
	reg("COMF2Fighter::SetLastActorHitBy(class CActor)", [](const NativeContext &c) { if (auto *o = c.self()) o->last_hit_by = c.arg_object(0); });
	reg("COMF2Fighter::class CActor LastActorHit()", [](const NativeContext &c) { auto *o = c.self(); c.ret_object(o ? o->last_hit : nullptr); });
	reg("COMF2Fighter::class CActor LastActorHitBy()", [](const NativeContext &c) { auto *o = c.self(); c.ret_object(o ? o->last_hit_by : nullptr); });
	const std::vector<std::pair<const char *, int>> guides = {{"COMF2Fighter::GuideToLastActorHit(int,float)", 1},
	                                                          {"COMF2Fighter::GuideToLastActorHitBy(int,float)", 2},
	                                                          {"COMF2Fighter::GuideToBestMeleeTarget(int,float)", 3}};
	for (auto &[key, mode] : guides) {
		int m = mode;
		reg(key, [=](const NativeContext &c) {
			if (auto *o = c.self()) {
				o->guide_frames = c.arg<int32_t>(0);
				o->guide_power = c.arg<float>(1) != 0 ? c.arg<float>(1) : 0.01f;
				o->guide_mode = m;
			}
		});
	}
	// DE_COMF2Fighter::NewGetHitEffect (0x35026710, vtable +0x23c): unless the effect type is 0 or
	// 4, a copy of the fighter's "SparkEffect" child at the connect point. Effect type 1 points its
	// Z along the effect direction; type 3 takes X from the second direction and Z from the first
	// (Y = X x Z) and switches the sparks to the spray. With m = |effect direction|, the Sparks get
	// 11 - trunc(-4m) sparks when m > 1 (none otherwise) and the light, kept only for m >= 3, is
	// coloured (1, 0.7, 0.3) at min(1.5m, 1).
	// The shared part (also DE_COMF2Fighter::NewGetHitSpark 0x35026a60, which places the copy at a
	// position with Set(angle) and returns it).
	auto spawn_sparks = [](World &w, GameObject *f, const std::array<float, 3> &at, const std::array<float, 9> *rows, int spark_type,
	                       float power) -> GameObject * {
		GameObject *effect = nullptr;
		for (GameObject *ch : f->children) {
			if (ch->name == "SparkEffect" && ch->shape != GameObject::Shape::None) effect = ch;
		}
		if (!effect) return nullptr;
		GameObject *copy = w.copy_tree(*effect);
		copy->local = {};
		copy->local.t = at;
		if (rows) copy->local.r = *rows;
		SparkBurst b;
		b.actor = copy;
		b.type = spark_type;
		std::function<void(GameObject *)> scan = [&](GameObject *o) {
			for (auto *list : {&o->children, &o->components}) {
				for (GameObject *ch : *list) {
					if (ch->class_path == "Mesh\\Sparks" && !b.image && ch->file) b.image = de::read_mesh_image(w.data(), {ch->file, ch->index});
					if (ch->class_path == "Light" && !b.light) b.light = ch;
				}
			}
		};
		scan(copy);
		b.count = power > 1 ? 11 - int(power * -4) : 0;
		b.light_intensity = std::min(power * 1.5f, 1.0f);
		if (b.light && power < 3) {
			w.destroy(b.light);
			b.light = nullptr;
		}
		if (b.light) {
			auto &st = w.particles->light(b.light);
			st.color = std::array<float, 3>{1, 0.7f, 0.3f};
			st.intensity = b.light_intensity;
		}
		w.enter_scene(copy);
		w.particles->add_sparks(std::move(b));
		return copy;
	};
	auto unit = [](std::array<float, 3> v) {
		float l = length(v);
		return l > 0 ? std::array<float, 3>{v[0] / l, v[1] / l, v[2] / l} : v;
	};
	auto cross3 = [](const std::array<float, 3> &x, const std::array<float, 3> &y) {
		return std::array<float, 3>{x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
	};
	// DE_CCoordinateSystem::Set(direction) (Core 0x70025fe0): Z the direction, Y the world up made
	// perpendicular to it (world Z when parallel), X = Y x Z.
	auto rows_along = [=](const std::array<float, 3> &dir) {
		auto z = unit(dir);
		float d = z[1];
		std::array<float, 3> y = d >= 1 || d <= -1 ? std::array<float, 3>{0, 0, d} : unit({-z[0] * d, 1 - z[1] * d, -z[2] * d});
		auto x = cross3(y, z);
		return std::array<float, 9>{x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
	};
	// DE_COMF2Fighter::NewGetHitEffect (0x35026710, vtable +0x23c): unless the effect type is 0 or
	// 4, a copy of the fighter's "SparkEffect" child at the connect point. Effect type 1 points its
	// Z along the effect direction; type 3 takes X from the second direction and Z from the first
	// (Y = X x Z) and switches the sparks to the spray. With m = |effect direction|, the Sparks get
	// 11 - trunc(-4m) sparks when m > 1 (none otherwise) and the light, kept only for m >= 3, is
	// coloured (1, 0.7, 0.3) at min(1.5m, 1).
	reg("COMF2Fighter::NewGetHitEffect(class CAttackInfo)", [=](const NativeContext &c) {
		GameObject *f = c.self();
		uint32_t a = c.arg<uint32_t>(0);
		if (!f || !a) return;
		auto &m = c.mem();
		uint32_t type = m.get<uint32_t>(a + kEffectType);
		if (type == 0 || type == 4) return;
		std::array<float, 3> dir = read_vec(m, a + kEffectDirection), dir2 = read_vec(m, a + kEffectDirection2);
		std::array<float, 9> rows{1, 0, 0, 0, 1, 0, 0, 0, 1};
		if (type == 1) rows = rows_along(dir);
		if (type == 3) {
			auto x = unit(dir2), z = unit(dir), y = cross3(x, z);
			rows = {x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
		}
		spawn_sparks(c.world, f, read_vec(m, a + kConnectPoint), type == 1 || type == 3 ? &rows : nullptr, type == 3 ? 1 : 0, length(dir));
	});
	reg("COMF2Fighter::NewGetHitSpark(class CVector,class CVector)", [=](const NativeContext &c) {
		GameObject *f = c.self();
		uint32_t p = c.arg<uint32_t>(0), ang = c.arg<uint32_t>(1);
		if (!f || !p || !ang) {
			c.ret_u32(0);
			return;
		}
		auto dir = read_vec(c.mem(), ang);
		auto rows = rows_along(dir);
		c.ret_object(spawn_sparks(c.world, f, read_vec(c.mem(), p), &rows, 0, length(dir)));
	});
	// PackForGetHitEffect / UnpackForGetHitEffect (OMF 0x35016e00, 0x35016f30): the fields the hit
	// effect needs, through a replicated CData. Pack writes the damage as the byte
	// trunc(clamp(damage, 0, 50) x 255); Unpack reads that byte but discards it, setting the
	// damage to clamp(its own damage, 0, 50) / 255 x 50 instead (as the original does).
	reg("CObject::CAttackInfo::PackForGetHitEffect(class CData)", [=](const NativeContext &c) {
		uint32_t a = info_of(c), d = c.arg<uint32_t>(0);
		if (!a || !d) return;
		auto &pk = g_packed_hits[d];
		float dmg = std::clamp(c.mem().get<float>(a + kDamage), 0.0f, 50.0f);
		pk.damage = uint8_t(int32_t(dmg * 255));
		pk.effect = c.mem().get<uint32_t>(a + kEffectType);
		pk.reaction = c.mem().get<uint32_t>(a + kReactionType);
		pk.attack = c.mem().get<uint32_t>(a + kAttackType);
		pk.point = read_vec(c.mem(), a + kConnectPoint);
		pk.dir = read_vec(c.mem(), a + kEffectDirection);
		pk.dir2 = read_vec(c.mem(), a + kEffectDirection2);
		pk.flags = c.mem().get<uint8_t>(a + kFlags);
	});
	reg("CObject::CAttackInfo::UnpackForGetHitEffect(class CData)", [=](const NativeContext &c) {
		uint32_t a = info_of(c), d = c.arg<uint32_t>(0);
		auto it = g_packed_hits.find(d);
		if (!a || it == g_packed_hits.end()) return;
		const auto &pk = it->second;
		c.mem().set<float>(a + kDamage, std::clamp(c.mem().get<float>(a + kDamage), 0.0f, 50.0f) / 255 * 50);
		c.mem().set<uint32_t>(a + kEffectType, pk.effect);
		c.mem().set<uint32_t>(a + kReactionType, pk.reaction);
		c.mem().set<uint32_t>(a + kAttackType, pk.attack);
		write_vec(c.mem(), a + kConnectPoint, pk.point);
		write_vec(c.mem(), a + kEffectDirection, pk.dir);
		write_vec(c.mem(), a + kEffectDirection2, pk.dir2);
		c.mem().set<uint8_t>(a + kFlags, pk.flags);
	});
	// RegisterStandardComboAttackCaller (0x35022880): GuideToLastActorHit(frames, power)
	// and RegisterStandardState (which only concerns replication here).
	reg("COMF2Fighter::RegisterStandardComboAttack(int,float)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->guide_frames = c.arg<int32_t>(0);
			o->guide_power = c.arg<float>(1) != 0 ? c.arg<float>(1) : 0.01f;
			o->guide_mode = 1;
		}
	});
	reg("COMF2Fighter::class CActor CurrentTarget()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_object(o ? c.world.current_target_of(*o) : nullptr);
	});
	reg("COMF2Fighter::SetInvulnerabilityFramesRemaining(int)", [](const NativeContext &c) { if (auto *o = c.self()) o->invulnerable_frames = c.arg<int32_t>(0); });
	reg("COMF2Fighter::IncrementInvulnerabilityFramesRemaining(int)", [](const NativeContext &c) { if (auto *o = c.self()) o->invulnerable_frames += c.arg<int32_t>(0); });
	reg("COMF2Fighter::int InvulnerabilityFramesRemaining()", [](const NativeContext &c) { auto *o = c.self(); c.ret_u32(o ? uint32_t(std::max(0, o->invulnerable_frames)) : 0); });
	reg("COMF2Fighter::bool IsInvulnerable()", [](const NativeContext &c) { auto *o = c.self(); c.ret_bool(o && o->invulnerable_frames > 0); });
	// Run-time generated moves: capture the pose now (initial) or after setting a sequence (final).
	reg("CCreature::AnimCaptureAsInitial()", [](const NativeContext &c) {
		if (auto *o = c.self(); o && o->fighter) c.world.validate_animation(*o);  // the Caller validates first
		if (auto *o = c.self()) o->capture_initial = {o->sequence, o->prelude ? o->prelude_time : o->anim_time};
	});
	reg("CCreature::AnimCaptureAsFinal()", [](const NativeContext &c) {
		if (auto *o = c.self(); o && o->fighter) c.world.validate_animation(*o);
		if (auto *o = c.self()) o->capture_final = {o->sequence, o->anim_time};
	});
	reg("CCreature::AnimActivateEmpty()", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->captured_blend = true;
			o->prelude = false;
			o->anim_time = 0;
			++o->activations;
		}
	});

	// ForceOffFloorCaller: physics +0x18c = 0 (the next frame starts airborne).
	reg("COMF2Fighter::ForceOffFloor()", [](const NativeContext &c) { if (auto *o = c.self()) o->on_floor = false; });

	// SetLeft/RightCarryable: single references at fighter +0x314 / +0x320 (what each hand holds,
	// read by the AI's pickup search).
	reg("COMF2Fighter::SetLeftCarryable(class CActor)", [](const NativeContext &c) { if (auto *o = c.self()) o->carry[0] = c.arg_object(0); });
	reg("COMF2Fighter::SetRightCarryable(class CActor)", [](const NativeContext &c) { if (auto *o = c.self()) o->carry[1] = c.arg_object(0); });

	// --- Danger -----------------------------------------------------------------------------------
	reg("CDanger::SetDangerLevel(float)", [](const NativeContext &c) { if (auto *o = c.self()) o->danger_level = c.arg<float>(0); });
	reg("CDanger::float DangerLevel()", [](const NativeContext &c) { auto *o = c.self(); c.ret_f32(o ? o->danger_level : 0); });
	reg("CDanger::SetDangerDelay(float)", [](const NativeContext &c) {
		if (auto *o = c.self()) g_danger_timing[o] = {c.arg<float>(0), uint32_t(c.world.scene_ms())};
	});
	// DE_CDanger::DangerDelay: the delay still to run since it was set (ms), never negative.
	reg("CDanger::float DangerDelay(class CActor)", [](const NativeContext &c) {
		auto *o = c.self();
		auto it = o ? g_danger_timing.find(o) : g_danger_timing.end();
		float left = 0;
		if (it != g_danger_timing.end()) {
			left = std::max(0.0f, it->second.delay_ms - float(uint32_t(c.world.scene_ms()) - it->second.set_at));
		}
		c.ret_f32(left);
	});
	// DE_CCreature::TotalDangerLevel / GetDangerLevel: summed danger at a point (danger group
	// 0x1000 affectors x their CDanger level), ignoring the creature's own danger areas.
	reg("CCreature::float TotalDangerLevel(class CVector)", [](const NativeContext &c) {
		c.ret_f32(c.world.danger_at(read_vec(c.mem(), c.arg<uint32_t>(0)), c.self(), nullptr, nullptr));
	});
	reg("CCreature::float TotalDangerLevel()", [](const NativeContext &c) {
		auto *o = c.self();
		float v = o ? c.world.danger_at(focal_point(*o), o, nullptr, nullptr) : 0;
		static const bool debug = getenv("OPENBG_DEBUG_DANGER") != nullptr;  // perceived danger
		if (debug && v > 0) fprintf(stderr, "[danger] frame %u %s total %.2f\n", c.world.frame(), o->name.c_str(), v);
		c.ret_f32(v);
	});
	reg("CCreature::float GreatestThreatLevel()", [](const NativeContext &c) {
		auto *o = c.self();
		float level = 0;
		if (o) c.world.danger_at(focal_point(*o), o, nullptr, &level);
		c.ret_f32(level);
	});
	reg("CCreature::class CDanger GreatestThreat()", [](const NativeContext &c) {
		auto *o = c.self();
		GameObject *g = nullptr;
		if (o) c.world.danger_at(focal_point(*o), o, &g, nullptr);
		c.ret_object(g);
	});
	// IsGreatestThreatBlockable (OMF 0x3502bb30): the greatest threat is an OMF danger whose
	// blockable flag (+0x5c, default TRUE) is set; fighters' melee danger areas are.
	reg("COMF2Fighter::bool IsGreatestThreatBlockable()", [](const NativeContext &c) {
		auto *o = c.self();
		GameObject *g = nullptr;
		if (o) c.world.danger_at(focal_point(*o), o, &g, nullptr);
		c.ret_bool(g && top_fighter(g) && top_fighter(g) != o);
	});
}

}  // namespace game
