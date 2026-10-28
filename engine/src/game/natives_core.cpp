// First tier of native intrinsics: object tree, strings, vectors, actor transforms, creature
// animation and simple fighter physics. Keys are the exact "CClass::signature" strings from
// the compiled code. Behaviour is reimplemented from the corresponding *Caller functions where
// noted; the rest is a minimal stand-in until the subsystem is decoded.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <tuple>

#include "game/forces.h"
#include "game/particles.h"
#include "game/runtime.h"

namespace game {

const std::map<std::string, std::string> &class_paths() {
	static const std::map<std::string, std::string> m = {
#include "game/class_paths.inc"
	};
	return m;
}

namespace {

GameObject *find_child(GameObject *o, const std::string &name) {
	if (!o) {
		return nullptr;
	}
	for (auto *c : o->children) {
		if (c->name == name) {
			return c;
		}
	}
	return nullptr;
}

GameObject *find_descendant(GameObject *o, const std::string &name) {
	if (!o) {
		return nullptr;
	}
	for (auto *c : o->children) {
		if (c->name == name) {
			return c;
		}
	}
	for (auto *c : o->children) {
		if (auto *d = find_descendant(c, name)) {
			return d;
		}
	}
	return nullptr;
}

size_t count_descendants(GameObject *o) {
	size_t n = 0;
	for (auto *c : o->children) {
		n += 1 + count_descendants(c);
	}
	return n;
}

// DE_CObject::ObjDescendant (Core 0x7003dc30): direct children first, then each child's
// descendants in turn.
GameObject *descendant(GameObject *o, int32_t i) {
	if (!o || i < 0) {
		return nullptr;
	}
	if (size_t(i) < o->children.size()) {
		return o->children[size_t(i)];
	}
	int32_t r = i - int32_t(o->children.size());
	for (auto *ch : o->children) {
		int32_t n = int32_t(count_descendants(ch));
		if (r < n) {
			return descendant(ch, r);
		}
		r -= n;
	}
	return nullptr;
}

// Singletons the scripts reach through Scene()/Session()/Environment()/Pilot().
GameObject *singleton(World &w, const std::string &name, const std::string &path) { return w.create(name, path); }

}  // namespace

std::array<float, 3> read_vec(vm::Memory &m, uint32_t a) {
	if (!a) {
		return {0, 0, 0};
	}
	return {m.get<float>(a), m.get<float>(a + 4), m.get<float>(a + 8)};
}

void write_vec(vm::Memory &m, uint32_t a, const std::array<float, 3> &v) {
	if (!a) {
		return;
	}
	m.set<float>(a, v[0]);
	m.set<float>(a + 4, v[1]);
	m.set<float>(a + 8, v[2]);
}

void register_core_natives(World &w) {
	auto reg = [&](const char *key, Native fn) { w.register_native(key, std::move(fn)); };
	auto stub = [&](const char *key) { w.register_native(key, [](const NativeContext &) {}); };

	// --- Object tree -----------------------------------------------------------------------
	reg("CObject::string Name()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_u32(o ? c.world.string(o->name) : 0);
	});
	// PathCaller -> DE_CObject::ObjPath (Core 0x7003a880): the names from the root down, joined by
	// a separator (taken to be '\\'; the scripts use it only to name spawned actors for debugging).
	reg("CObject::string Path()", [](const NativeContext &c) {
		auto *o = c.self();
		std::string p;
		for (; o; o = o->parent) p = p.empty() ? o->name : o->name + "\\" + p;
		c.ret_u32(c.world.string(p));
	});
	reg("CObject::int NumChildren()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_u32(o ? uint32_t(o->children.size()) : 0);
	});
	reg("CObject::class CObject Child(int)", [](const NativeContext &c) {
		auto *o = c.self();
		int32_t i = c.arg<int32_t>(0);
		c.ret_object(o && i >= 0 && size_t(i) < o->children.size() ? o->children[size_t(i)] : nullptr);
	});
	reg("CObject::class CObject FindChildByName(string)",
	    [](const NativeContext &c) { c.ret_object(find_child(c.self(), c.arg_string(0))); });
	reg("CObject::class CObject FindDescendantByName(string)",
	    [](const NativeContext &c) { c.ret_object(find_descendant(c.self(), c.arg_string(0))); });
	reg("CObject::class CObject Descendant(int)",
	    [](const NativeContext &c) { c.ret_object(descendant(c.self(), c.arg<int32_t>(0))); });
	reg("CObject::bool IsNamePrefix(string)", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_bool(o && o->name.rfind(c.arg_string(0), 0) == 0);
	});
	reg("CObject::float DifferenceBetween(float,float)",
	    [](const NativeContext &c) { c.ret_f32(std::fabs(c.arg<float>(0) - c.arg<float>(1))); });
	reg("CObject::int NumDescendants()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_u32(o ? uint32_t(count_descendants(o)) : 0);
	});
	reg("CActor::class CObject FindPropertyByName(string)", [](const NativeContext &c) {
		auto *o = c.self();
		auto name = c.arg_string(0);
		GameObject *hit = nullptr;
		if (o) {
			for (auto *p : o->components) {
				if (p->name == name) {
					hit = p;
				}
			}
			if (!hit) {
				hit = find_child(o, name);
			}
			// Copies (templates, description resources) keep their data identity: look at the
			// actor's property references (DE_CActor +0x20) themselves.
			if (!hit && o->file) {
				for (auto &r : c.world.properties_of(*o)) {
					if (r && r.info().name == name) {
						hit = c.world.global_object(r);
						break;
					}
				}
			}
			// A unique actor property kept elsewhere (Forces.DEObjects' footfall force) is the
			// actor's own copy: its Actor() is this actor.
			if (hit && hit->is("UniqueActorProperty") && !hit->is("UniqueActorProperty\\Danger")) {
				hit = c.world.forces->unique_property(*o, *hit);
			}
		}
		c.ret_object(hit);
	});
	reg("CObject::class CObject Parent()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_object(o ? o->parent : nullptr);
	});
	reg("CObject::SetParent(class CObject)", [](const NativeContext &c) {
		auto *o = c.self();
		auto *p = c.arg_object(0);
		if (!o || o == p) {
			return;
		}
		bool was = in_scene(*o);
		if (o->parent) {
			auto &v = o->parent->children;
			v.erase(std::remove(v.begin(), v.end(), o), v.end());
		}
		o->parent = p;
		if (p) {
			p->children.push_back(o);
		}
		c.world.parented(o, was);
	});
	reg("CObject::SetNoParent()", [](const NativeContext &c) {
		auto *o = c.self();
		if (!o || !o->parent) return;
		auto &v = o->parent->children;
		v.erase(std::remove(v.begin(), v.end(), o), v.end());
		o->parent = nullptr;
		if (std::find(c.world.scene_roots.begin(), c.world.scene_roots.end(), o) == c.world.scene_roots.end()) c.world.scene_roots.push_back(o);
	});
	reg("CObject::class CObject NewCopy()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_object(o ? c.world.copy_tree(*o) : nullptr);
	});

	// Generic type checks: CObject::bool Is<Class>(class CObject).
	for (auto &[name, path] : class_paths()) {
		std::string key = "CObject::bool Is" + name + "(class CObject)";
		std::string p = path;
		w.register_native(key, [p](const NativeContext &c) {
			auto *o = c.arg_object(0);
			c.ret_bool(o && o->is(p.c_str()));
		});
	}

	// --- Strings (CObject::CText holds a char* at +0) --------------------------------------
	reg("CObject::CText::(opp =)(string)", [](const NativeContext &c) {
		uint32_t t = c.self_handle();
		if (t) {
			c.mem().set<uint32_t>(t, c.world.string(c.arg_string(0)));
		}
	});
	reg("CObject::CText::(opp +=)(string)", [](const NativeContext &c) {
		uint32_t t = c.self_handle();
		if (t) {
			std::string s = c.world.read_string(c.mem().get<uint32_t>(t)) + c.arg_string(0);
			c.mem().set<uint32_t>(t, c.world.string(s));
		}
	});
	reg("CObject::CText::string Conversion()", [](const NativeContext &c) {
		uint32_t t = c.self_handle();
		c.ret_u32(t ? c.mem().get<uint32_t>(t) : 0);
	});
	reg("CGenericEvent::CNamedHandle::(opp =)(string)", [](const NativeContext &c) {
		uint32_t t = c.self_handle();
		if (t) {
			c.mem().set<uint32_t>(t, c.world.string(c.arg_string(0)));
		}
	});

	// --- Vectors (CObject::CVector is three floats) ----------------------------------------
	// VectorReflectBySurfaceNormalCaller (0x7014ac00): the direction reflected off the plane of
	// normal n, d - 2 (d . n) n, at the same length.
	reg("CObject::CVector::ReflectBySurfaceNormal(class CVector)", [](const NativeContext &c) {
		uint32_t v = c.self_handle(), nh = c.arg<uint32_t>(0);
		if (!v || !nh) return;
		auto a = read_vec(c.mem(), v), n = read_vec(c.mem(), nh);
		float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
		if (l == 0) return;
		float d = 2 * (a[0] / l * n[0] + a[1] / l * n[1] + a[2] / l * n[2]);
		write_vec(c.mem(), v, {(a[0] / l - n[0] * d) * l, (a[1] / l - n[1] * d) * l, (a[2] / l - n[2] * d) * l});
	});
	// CoordinateSystemReflectBySurfaceNormalCaller (0x7014b320): with t = normalize(n x X) and
	// the frame F = (n x t, t, n), each of the system's rows is taken into F, its n component
	// negated, and turned back: the frame mirrored across the surface (for a unit n,
	// r - 2 (r . n) n).
	reg("CObject::CCoordinateSystem::ReflectBySurfaceNormal(class CVector)", [](const NativeContext &c) {
		uint32_t cs = c.self_handle(), nh = c.arg<uint32_t>(0);
		if (!cs || !nh) return;
		using V = std::array<float, 3>;
		auto cross = [](const V &a, const V &b) { return V{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
		auto dot = [](const V &a, const V &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
		V n = read_vec(c.mem(), nh);
		V rows[3] = {read_vec(c.mem(), cs), read_vec(c.mem(), cs + 12), read_vec(c.mem(), cs + 24)};
		V t = cross(n, rows[0]);
		float tl = std::sqrt(dot(t, t));
		if (tl > 0) t = {t[0] / tl, t[1] / tl, t[2] / tl};
		V a = cross(n, t);
		for (int i = 0; i < 3; ++i) {
			float ka = dot(rows[i], a), kt = dot(rows[i], t), kn = -dot(rows[i], n);
			V r{};
			for (int j = 0; j < 3; ++j) r[size_t(j)] = ka * a[size_t(j)] + kt * t[size_t(j)] + kn * n[size_t(j)];
			write_vec(c.mem(), cs + uint32_t(i) * 12, r);
		}
	});
	reg("CObject::CVector::Reverse()", [](const NativeContext &c) {  // ReverseCaller 0x7014ab30
		if (uint32_t v = c.self_handle()) {
			auto a = read_vec(c.mem(), v);
			write_vec(c.mem(), v, {-a[0], -a[1], -a[2]});
		}
	});
	reg("CObject::CVector::(opp =)(class CVector)", [](const NativeContext &c) {
		write_vec(c.mem(), c.self_handle(), read_vec(c.mem(), c.arg<uint32_t>(0)));
	});
	reg("CObject::CVector::Set(class CVector)", [](const NativeContext &c) {
		write_vec(c.mem(), c.self_handle(), read_vec(c.mem(), c.arg<uint32_t>(0)));
	});
	reg("CObject::CVector::Set(float,float,float)", [](const NativeContext &c) {
		write_vec(c.mem(), c.self_handle(), {c.arg<float>(0), c.arg<float>(1), c.arg<float>(2)});
	});
	reg("CObject::CVector::(opp *=)(float)", [](const NativeContext &c) {
		auto v = read_vec(c.mem(), c.self_handle());
		float k = c.arg<float>(0);
		write_vec(c.mem(), c.self_handle(), {v[0] * k, v[1] * k, v[2] * k});
	});
	// EqualsMultiplyOpperatorCaller (Core 0x7014a830): per component; a null vector zeroes it.
	reg("CObject::CVector::(opp *=)(class CVector)", [](const NativeContext &c) {
		auto v = read_vec(c.mem(), c.self_handle());
		uint32_t a = c.arg<uint32_t>(0);
		auto u = a ? read_vec(c.mem(), a) : std::array<float, 3>{0, 0, 0};
		write_vec(c.mem(), c.self_handle(), {v[0] * u[0], v[1] * u[1], v[2] * u[2]});
	});
	reg("CObject::CVector::(opp /=)(float)", [](const NativeContext &c) {
		auto v = read_vec(c.mem(), c.self_handle());
		float k = c.arg<float>(0);
		if (k != 0) write_vec(c.mem(), c.self_handle(), {v[0] / k, v[1] / k, v[2] / k});
	});
	reg("CObject::CVector::(opp +=)(class CVector)", [](const NativeContext &c) {
		auto v = read_vec(c.mem(), c.self_handle()), u = read_vec(c.mem(), c.arg<uint32_t>(0));
		write_vec(c.mem(), c.self_handle(), {v[0] + u[0], v[1] + u[1], v[2] + u[2]});
	});
	reg("CObject::CVector::(opp -=)(class CVector)", [](const NativeContext &c) {
		auto v = read_vec(c.mem(), c.self_handle()), u = read_vec(c.mem(), c.arg<uint32_t>(0));
		write_vec(c.mem(), c.self_handle(), {v[0] - u[0], v[1] - u[1], v[2] - u[2]});
	});
	reg("CObject::CVector::Normalize()", [](const NativeContext &c) {
		auto v = read_vec(c.mem(), c.self_handle());
		float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (l > 0) {
			write_vec(c.mem(), c.self_handle(), {v[0] / l, v[1] / l, v[2] / l});
		}
	});
	reg("CObject::CVector::SetLength(float)", [](const NativeContext &c) {
		auto v = read_vec(c.mem(), c.self_handle());
		float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), k = c.arg<float>(0);
		if (l > 0) {
			write_vec(c.mem(), c.self_handle(), {v[0] / l * k, v[1] / l * k, v[2] / l * k});
		}
	});
	reg("CObject::CVector::float Length()", [](const NativeContext &c) {
		auto v = read_vec(c.mem(), c.self_handle());
		c.ret_f32(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
	});

	// --- Math helpers (CObject statics; SmoothFastToStopCaller etc.) -----------------------
	reg("CObject::float SmoothFastToStop(float)", [](const NativeContext &c) {
		float x = c.arg<float>(0);
		c.ret_f32(1.0f - (1.0f - x) * (1.0f - x));
	});
	reg("CObject::float SmoothStopToFast(float)", [](const NativeContext &c) {
		float x = c.arg<float>(0);
		c.ret_f32(x * x);
	});

	// --- Scene / session --------------------------------------------------------------------
	reg("CActor::class CScene Scene()", [](const NativeContext &c) { c.ret_object(singleton(c.world, "Scene", "Scene")); });
	reg("CScene::class CSession Session()", [](const NativeContext &c) { c.ret_object(singleton(c.world, "Session", "Session")); });
	reg("CCreature::class CCreature Environment()",
	    [](const NativeContext &c) { c.ret_object(singleton(c.world, "Environment", "Actor\\Creature\\Environment")); });
	// Scene time is in milliseconds (scripts wait e.g. 200 between navigation commands); 60 frames/s.
	reg("CScene::dword Time()", [](const NativeContext &c) { c.ret_u32(uint32_t(c.world.scene_ms())); });
	reg("CSession::bool IsSinglePlayer()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CSession::bool IsServer()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CActor::bool IsServer()", [](const NativeContext &c) { c.ret_bool(true); });
	reg("CCreature::bool IsLocallyControlled()", [](const NativeContext &c) { c.ret_bool(true); });
	// IsLowAuthoritySimulationCaller (Creature): false on the server.
	// InvalidateOrientationCaller: marks cached world orientations stale; ours are computed on use.
	reg("CCreature::InvalidateOrientation()", [](const NativeContext &) {});
	reg("CCreature::bool IsLowAuthoritySimulation()", [](const NativeContext &c) { c.ret_bool(false); });
	// SetInstanceNumberCaller appends the number to the creature's ID suffix (+0x1ec), which only
	// DE_CCreature::BuildID (replication) reads.
	reg("CCreature::SetInstanceNumber(int)", [](const NativeContext &) {});
	reg("CCreature::bool IsUserControlled()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_bool(o && o->user_controlled);
	});

	// --- Actor transforms ---------------------------------------------------------------------
	reg("CActor::class CVector WorldPosition()", [](const NativeContext &c) {
		auto *o = c.self();
		if (!o) {
			c.ret_u32(0);
			return;
		}
		write_vec(c.mem(), o->vec_world_pos, o->world().t);
		if (getenv("OPENBG_DEBUG_POS")) {
			auto *who = c.owner();
			fprintf(stderr, "WorldPosition of %s (asked by %s) = (%.1f %.1f %.1f) block %u\n", o->name.c_str(),
			        who ? who->name.c_str() : "?", o->world().t[0], o->world().t[1], o->world().t[2], o->vec_world_pos);
		}
		c.ret_u32(o->vec_world_pos);
	});
	reg("CActor::class CVector LocalPosition()", [](const NativeContext &c) {
		auto *o = c.self();
		if (!o) {
			c.ret_u32(0);
			return;
		}
		write_vec(c.mem(), o->vec_local_pos, o->local.t);
		c.ret_u32(o->vec_local_pos);
	});
	reg("CActor::SetLocalPosition(class CVector)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->local.t = read_vec(c.mem(), c.arg<uint32_t>(0));
			// A session fighter not yet in the scene (its slot normally adds it, OwnCreature) joins it
			// when BFighter::ObjectSpawn first places it.
			if (o->fighter && !o->spawned && c.world.session) {
				o->spawned = true;
				if (std::find(c.world.scene_roots.begin(), c.world.scene_roots.end(), o) == c.world.scene_roots.end()) {
					c.world.scene_roots.push_back(o);
				}
			}
		}
	});
	// SetPhaseCaller (Core 0x7018cc90, vtable +0x16c): DE_CImage::SetPhase stores the phase and
	// marks the image changed; DE_CAVIImage's picks its frame, a procedural image regenerates
	// (none of the scripted ones is procedural).
	reg("CImage::SetPhase(float)", [](const NativeContext &c) {
		if (GameObject *o = c.self(); o && o->file) c.world.image_phases[{o->file, o->index}] = c.arg<float>(0);
	});
	// DE_CActor::CopyWorldToLocal (Core 0x7008b120): this actor's local position and orientation
	// become the target's world ones.
	reg("CActor::CopyWorldToLocal(class CActor)", [](const NativeContext &c) {
		GameObject *o = c.self(), *t = c.arg_object(0);
		if (o && t) o->local = t->world();
	});
	reg("CActor::SetLocalPosition(float,float,float)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->local.t = {c.arg<float>(0), c.arg<float>(1), c.arg<float>(2)};
		}
	});
	reg("CActor::SetLocalOrientation(class CCoordinateSystem)", [](const NativeContext &c) {
		auto *o = c.self();
		uint32_t cs = c.arg<uint32_t>(0);
		if (o && cs) {
			for (int i = 0; i < 9; ++i) {
				o->local.r[size_t(i)] = c.mem().get<float>(cs + uint32_t(i) * 4);
			}
		}
	});
	// AddCopyToSceneCaller (Core): a copy of the property placed by this actor's world transform
	// (its own transform taken as relative to the actor), added to the actor's scene.
	reg("CActor::AddCopyToScene(class CActor)", [](const NativeContext &c) {
		GameObject *o = c.self(), *p = c.arg_object(0);
		if (!o || !p) return;
		GameObject *copy = c.world.copy_tree(*p);
		copy->local = o->world().then(p->local);
		c.world.enter_scene(copy);
	});
	// Enable/DisableAllMeshPropertiesCaller (0x7008f1b0, 0x7008f230): every mesh property of the
	// actor is enabled / disabled for it (vtable +0x150 / +0x154) and its particle instances
	// start / stop spawning (+0x58).
	for (bool on : {true, false}) {
		reg(on ? "CActor::EnableAllMeshProperties()" : "CActor::DisableAllMeshProperties()", [on](const NativeContext &c) {
			if (GameObject *o = c.self()) {
				o->meshes_disabled = !on;
				c.world.particles->set_spawning(o, on);
			}
		});
	}
	// AddFilterCaller (0x7018d300): the filter joins the image's list (+0x34).
	reg("CImage::AddFilter(class CImageFilter)", [](const NativeContext &c) {
		GameObject *img = c.self(), *f = c.arg_object(0);
		if (img && f && img->file) c.world.image_filters[{img->file, img->index}] = f;
	});
	// UpdateCaller (0x701d5120): the generator's instances step to the scene time; every live
	// effect steps each frame here anyway (ParticleSystem::step).
	reg("CParticleGenerator::Update()", [](const NativeContext &) {});
	// SetBranchUnrenderedCaller (0x7008e5d0): the actor and its whole tree stop rendering.
	reg("CActor::SetBranchUnrendered()", [](const NativeContext &c) {
		std::function<void(GameObject *)> hide = [&](GameObject *o) {
			o->visible = false;
			for (GameObject *ch : o->children) hide(ch);
		};
		if (GameObject *o = c.self()) hide(o);
	});
	reg("CActor::SetBranchRendered()", [](const NativeContext &c) {
		std::function<void(GameObject *)> show = [&](GameObject *o) {
			o->visible = true;
			for (GameObject *ch : o->children) show(ch);
		};
		if (GameObject *o = c.self()) show(o);
	});
	// LocalTimeScaleCaller (Creature 0x150130f0): SceneTicksPerUpdate (+0x1dc) as a float.
	reg("CCreature::float LocalTimeScale()", [](const NativeContext &c) { c.ret_f32(float(c.world.creature_rate(c.self()))); });
	// IncreaseGravityMultiplierToCaller (OMF 0x35021680): raises the multiplier, never lowers it.
	reg("COMF2Fighter::IncreaseGravityMultiplierTo(float)", [](const NativeContext &c) {
		if (auto *o = c.self()) o->gravity = std::max(o->gravity, c.arg<float>(0));
	});
	// IsHighDetailGeometryCaller (Core 0x70045a60): the detail level; ShouldBeInactiveCaller
	// (0x7008e700): the actor lacks its flag for that level (+0x110: 2 low, 4 high).
	reg("CObject::bool IsHighDetailGeometry()", [](const NativeContext &c) { c.ret_bool(de::kHighDetail); });
	reg("CActor::bool ShouldBeInactive()", [](const NativeContext &c) {
		GameObject *o = c.self();
		c.ret_bool(o && !de::shown_at_detail(o->actor_flags));
	});
	reg("CActor::SetRendered()", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->visible = true;
		}
	});
	reg("CActor::SetUnrendered()", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->visible = false;
		}
	});
	// SetRendered/SetUnrenderedCaller (Core 0x7008e420): for one renderer; there is only ours.
	reg("CActor::SetRendered(class CRenderer)", [](const NativeContext &c) {
		if (auto *o = c.self()) o->visible = true;
	});
	reg("CActor::SetUnrendered(class CRenderer)", [](const NativeContext &c) {
		if (auto *o = c.self()) o->visible = false;
	});
	// UnitRolloverCaller: above 1 the fraction (x - (int)x); below 0, x + (int)x + 1.
	reg("CObject::float UnitRollover(float)", [](const NativeContext &c) {
		float x = c.arg<float>(0);
		if (x > 1) x -= float(int32_t(x));
		else if (x < 0) x += float(int32_t(x) + 1);
		c.ret_f32(x);
	});

	// --- Creature animation ---------------------------------------------------------------------
	// A CSequence handle is a block holding the sequence index (World::global_reference).
	auto activate = [](const NativeContext &c, bool prelude, float lead) {
		auto *o = c.self();
		uint32_t seq = c.arg<uint32_t>(0);
		if (getenv("OPENBG_DEBUG_ANIM")) {
			fprintf(stderr, "activate self=%s seq=%u -> %d\n", o ? o->name.c_str() : "(null)", seq,
			        seq ? c.mem().get<int32_t>(seq) : -1);
		}
		if (o && seq) {
			// AnimActivatePrelude(Time)Caller validates first: the prelude blends from the pose
			// shown now.
			if (prelude && o->fighter) {
				c.world.validate_animation(*o);
				o->pose_captured = o->pose_now;
			}
			o->sequence = c.mem().get<int32_t>(seq);
			o->anim_time = 0;
			o->prelude = prelude;
			o->prelude_time = lead;
			o->captured_blend = false;
			++o->activations;
		}
	};
	reg("CCreature::AnimActivate(class CSequence)", [=](const NativeContext &c) { activate(c, false, 0); });
	reg("CCreature::AnimActivatePrelude(class CSequence)", [=](const NativeContext &c) { activate(c, true, 0); });
	reg("CCreature::AnimActivatePrelude(class CSequence,float)",
	    [=](const NativeContext &c) { activate(c, true, c.arg<float>(1)); });
	// AnimSetTimeCaller (Creature): the time (+0x1c4), the pose made stale (+0x1c8 = 0) and
	// validated at once.
	reg("CCreature::AnimSetTime(float)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->anim_time = c.arg<float>(0);
			o->pose_valid = false;
			if (o->fighter) c.world.validate_animation(*o);
		}
	});
	// A separate CAnimationSequenceManager (Core: ActivateCaller, ActivatePrelude(Time)Caller,
	// SetTimeCaller) keeps the same state on the manager object: Gargoyle's wings.
	reg("CAnimationSequenceManager::Activate(class CSequence)", [=](const NativeContext &c) { activate(c, false, 0); });
	reg("CAnimationSequenceManager::ActivatePrelude(class CSequence)", [=](const NativeContext &c) { activate(c, true, 0); });
	reg("CAnimationSequenceManager::ActivatePrelude(class CSequence,float)", [=](const NativeContext &c) { activate(c, true, c.arg<float>(1)); });
	reg("CAnimationSequenceManager::SetTime(float)", [](const NativeContext &c) {
		if (auto *o = c.self()) o->anim_time = c.arg<float>(0);
	});
	reg("CCreature::AnimReset()", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->anim_time = 0;
		}
	});

	// --- Fighter physics (DE_COMF2Fighter / DE_CFighterPhysics; World::physics_step integrates) ----
	// Physics momentum is world units per frame. SetMomentum/IncrementMomentum take it as is;
	// the *WorldMomentum calls take units per millisecond, scaled by the fighter's +0x1dc (16 ms
	// per frame, set in the constructor): SetWorldMomentumVectorCaller.
	constexpr float kMsPerFrame = 16;
	reg("COMF2Fighter::SetWorldMomentum(class CVector)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			auto v = read_vec(c.mem(), c.arg<uint32_t>(0));
			o->momentum = {v[0] * kMsPerFrame, v[1] * kMsPerFrame, v[2] * kMsPerFrame};
		}
	});
	reg("COMF2Fighter::IncrementWorldMomentum(class CVector)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			auto v = read_vec(c.mem(), c.arg<uint32_t>(0));
			for (int k = 0; k < 3; ++k) o->momentum[size_t(k)] += v[size_t(k)] * kMsPerFrame;
		}
	});
	reg("COMF2Fighter::SetMomentum(class CVector)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->momentum = read_vec(c.mem(), c.arg<uint32_t>(0));
		}
	});
	reg("COMF2Fighter::SetMomentum(float,float,float)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->momentum = {c.arg<float>(0), c.arg<float>(1), c.arg<float>(2)};
		}
	});
	reg("COMF2Fighter::IncrementMomentum(class CVector)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			auto v = read_vec(c.mem(), c.arg<uint32_t>(0));
			for (int k = 0; k < 3; ++k) o->momentum[size_t(k)] += v[size_t(k)];
		}
	});
	auto local_momentum = [](const GameObject &o) { return o.local.inverse_rotate(o.momentum); };
	auto set_local_momentum = [](GameObject &o, std::array<float, 3> m) { o.momentum = o.local.rotate(m); };
	reg("COMF2Fighter::class CVector MomentumLocal()", [=](const NativeContext &c) {
		auto *o = c.self();
		static uint32_t v = 0;  // MomentumLocalCaller returns a static vector
		if (!v) v = c.mem().alloc(12);
		write_vec(c.mem(), v, o ? local_momentum(*o) : std::array<float, 3>{0, 0, 0});
		c.ret_u32(v);
	});
	// AirJump (0x35028670, vtable +0x1f8): each local axis (height y, left x, forward z) gains
	// its amount without passing it (no change when already beyond).
	reg("COMF2Fighter::AirJump(float,float,float)", [=](const NativeContext &c) {
		auto *o = c.self();
		if (!o) return;
		auto m = local_momentum(*o);
		auto toward = [](float cur, float v) {
			if (v > 0) return cur < v ? std::min(cur + v, v) : cur;
			return v < cur ? std::max(cur + v, v) : cur;
		};
		m[1] = toward(m[1], c.arg<float>(0));
		m[0] = toward(m[0], c.arg<float>(1));
		m[2] = toward(m[2], c.arg<float>(2));
		set_local_momentum(*o, m);
	});
	for (int axis = 0; axis < 3; ++axis) {
		std::string n(1, "XYZ"[axis]);
		reg(("COMF2Fighter::float MomentumLocal" + n + "()").c_str(), [=](const NativeContext &c) {
			auto *o = c.self();
			c.ret_f32(o ? local_momentum(*o)[size_t(axis)] : 0);
		});
		reg(("COMF2Fighter::SetMomentumLocal" + n + "(float)").c_str(), [=](const NativeContext &c) {
			if (auto *o = c.self()) {
				auto m = local_momentum(*o);
				m[size_t(axis)] = c.arg<float>(0);
				set_local_momentum(*o, m);
			}
		});
		reg(("COMF2Fighter::IncrementMomentumLocal" + n + "(float)").c_str(), [=](const NativeContext &c) {
			if (auto *o = c.self()) {
				auto m = local_momentum(*o);
				m[size_t(axis)] += c.arg<float>(0);
				set_local_momentum(*o, m);
			}
		});
	}
	reg("COMF2Fighter::IncrementMomentumLocal(float,float,float)", [=](const NativeContext &c) {
		if (auto *o = c.self()) {
			auto m = local_momentum(*o);
			for (int k = 0; k < 3; ++k) {
				m[size_t(k)] += c.arg<float>(k);
			}
			set_local_momentum(*o, m);
		}
	});
	reg("CActor::float MomentumLength()", [](const NativeContext &c) {
		auto *o = c.self();
		// MomentumLengthCaller: |momentum x TimeScale| (1/16 for a fighter, as Momentum()).
		float k = o && o->fighter ? 1.0f / 16 : 1.0f;
		c.ret_f32(o ? k * std::sqrt(o->momentum[0] * o->momentum[0] + o->momentum[1] * o->momentum[1] +
		                            o->momentum[2] * o->momentum[2])
		            : 0);
	});
	reg("COMF2Fighter::SetGravityMultiplier(float)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->gravity = c.arg<float>(0);
		}
	});
	reg("COMF2Fighter::float GravityMultiplier()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_f32(o ? o->gravity : 1);
	});
	// DecreaseGravityMultiplierToCaller (0x35021640): lowers the multiplier, never raises it.
	reg("COMF2Fighter::DecreaseGravityMultiplierTo(float)", [](const NativeContext &c) {
		if (auto *o = c.self()) o->gravity = std::min(o->gravity, c.arg<float>(0));
	});
	reg("COMF2Fighter::SetGravityRecover(float)", [](const NativeContext &c) {  // SetGravityRecoverCaller
		if (auto *o = c.self()) {
			o->gravity_recover = std::max(1e-5f, std::fabs(c.arg<float>(0)));
		}
	});
	reg("COMF2Fighter::SetTraction(float)", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->traction = c.arg<float>(0);
		}
	});
	reg("COMF2Fighter::bool IsRestingOnFloor()", [](const NativeContext &c) {
		auto *o = c.self();
		c.ret_bool(o && o->on_floor);
	});

	// --- Events (CGenericEvent local instances: +0 handle, +4 INT1, +8 object1, +12 object2) --------
	reg("CGenericEvent::SetHandle(class CNamedHandle)", [](const NativeContext &c) {
		if (uint32_t e = c.self_handle()) {
			c.mem().set<uint32_t>(e, c.arg<uint32_t>(0));
		}
	});
	reg("CGenericEvent::SetINT1(int)", [](const NativeContext &c) {
		if (uint32_t e = c.self_handle()) {
			c.mem().set<int32_t>(e + 4, c.arg<int32_t>(0));
		}
	});
	reg("CGenericEvent::int INT1()", [](const NativeContext &c) {
		uint32_t e = c.self_handle();
		if (getenv("OPENBG_DEBUG")) {
			fprintf(stderr, "INT1 event=%u -> %d\n", e, e ? c.mem().get<int32_t>(e + 4) : 0);
		}
		c.ret_u32(e ? c.mem().get<uint32_t>(e + 4) : 0);
	});
	reg("CGenericEvent::SetObject1(class CReferenceTarget)", [](const NativeContext &c) {
		if (uint32_t e = c.self_handle()) {
			c.mem().set<uint32_t>(e + 8, c.arg<uint32_t>(0));
		}
	});
	reg("CGenericEvent::SetObject2(class CReferenceTarget)", [](const NativeContext &c) {
		if (uint32_t e = c.self_handle()) {
			c.mem().set<uint32_t>(e + 12, c.arg<uint32_t>(0));
		}
	});
	// The session stand-in grants every request a fighter sends it (e.g. the OMF session's
	// "request creature enable input", answered through INT1) until session scripts run.
	reg("CEvent::SendToNow(class CObject)", [](const NativeContext &c) {
		uint32_t e = c.self_handle();
		auto *target = c.arg_object(0);
		if (getenv("OPENBG_DEBUG")) {
			fprintf(stderr, "SendToNow event=%u target=%s\n", e, target ? target->name.c_str() : "(null)");
		}
		if (e && target && target->name == "Session") {
			c.mem().set<int32_t>(e + 4, 1);
		}
	});

	// --- Controller input (player fighter) ----------------------------------------------------------
	auto in = [](const char *key, bool Input::*a, bool Input::*b = nullptr) {
		return std::make_pair(std::string(key), std::make_pair(a, b));
	};
	const std::vector<std::pair<std::string, std::pair<bool Input::*, bool Input::*>>> joystick = {
		in("COMF2Fighter::bool IsJoystickUp()", &Input::up), in("COMF2Fighter::bool IsJoystickDown()", &Input::down),
		in("COMF2Fighter::bool IsJoystickLeft()", &Input::left), in("COMF2Fighter::bool IsJoystickRight()", &Input::right),
		in("COMF2Fighter::bool IsJumpButtonDown()", &Input::jump),
		in("COMF2Fighter::bool IsLeftPunchButtonDown()", &Input::left_punch),
		in("COMF2Fighter::bool IsRightPunchButtonDown()", &Input::right_punch),
		in("COMF2Fighter::bool IsLeftKickButtonDown()", &Input::left_kick),
		in("COMF2Fighter::bool IsRightKickButtonDown()", &Input::right_kick),
	};
	for (auto &[key, fields] : joystick) {
		auto f = fields;
		w.register_native(key, [f](const NativeContext &c) {
			bool v = c.input().*(f.first);
			c.ret_bool(v);
		});
	}
	// IsDuckButtonDownCaller: controller button 5 (duck/evade).
	reg("COMF2Fighter::bool IsDuckButtonDown()", [](const NativeContext &c) { c.ret_bool(c.input().duck_evade()); });
	// Diagonals: both directions held.
	const std::vector<std::tuple<const char *, bool Input::*, bool Input::*>> diagonals = {
		{"COMF2Fighter::bool IsJoystickUpLeft()", &Input::up, &Input::left},
		{"COMF2Fighter::bool IsJoystickUpRight()", &Input::up, &Input::right},
		{"COMF2Fighter::bool IsJoystickDownLeft()", &Input::down, &Input::left},
		{"COMF2Fighter::bool IsJoystickDownRight()", &Input::down, &Input::right},
	};
	for (auto &[key, a, b] : diagonals) {
		auto fa = a, fb = b;
		w.register_native(key, [fa, fb](const NativeContext &c) { c.ret_bool(c.input().*fa && c.input().*fb); });
	}
	// Movement actions from the controller orientation (DE_COMF2Fighter::GetWalkAction 0x35027310,
	// GetJumpAction 0x35027400, GetPreJumpAction 0x35027510), indexed by World::orientation().
	auto action_table = [&](const char *key, std::array<uint8_t, 9> table) {
		reg(key, [table](const NativeContext &c) {
			GameObject *who = c.owner();
			c.ret_u32(table[who && who->user_controlled ? size_t(c.world.orientation()) : 8]);
		});
	};
	//                                                 up  ur  r   dr  d   dl  l   ul  neutral
	action_table("COMF2Fighter::byte GetWalkAction()", {1, 3, 4, 10, 8, 9, 5, 2, 0});
	action_table("COMF2Fighter::byte GetJumpAction()", {0x11, 0xf, 0xf, 0xf, 0x12, 0xe, 0xe, 0xe, 0x10});
	// GetRunAction (0x35027a10): up runs straight (11), up-left/left run turning left (12),
	// up-right/right turning right (13); anything else stops (0).
	action_table("COMF2Fighter::byte GetRunAction()", {11, 13, 13, 0, 0, 0, 12, 12, 0});
	action_table("COMF2Fighter::byte GetPreJumpAction()", {0x13, 0x13, 0x10, 0x14, 0x14, 0x14, 0x10, 0x13, 0x10});

	// Step `v` towards `target` by `step` without overshooting (the WalkStandard/JumpStandard idiom).
	auto approach = [](float v, float target, float step) {
		return v < target ? std::min(target, v + step) : std::max(target, v - step);
	};
	// TurnLeft (0x350258e0): rotate about Y by degrees x turn multiplier (+0x4c0, 1).
	auto turn_left = [](GameObject &o, float degrees) {
		float a = degrees * o.turn_multiplier * 0.017453292f, cs = std::cos(a), sn = std::sin(a);
		de::Transform yaw;
		yaw.r = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
		o.local = de::Transform{yaw.then(de::Transform{o.local.r, {0, 0, 0}}).r, o.local.t};
	};
	// Propel (0x35025af0) against a static surface. Momentum not aligned with the wanted
	// direction is cancelled (by traction x (1 - cos)), up to 0.2 x traction of it is steered onto
	// that direction, then each local axis eases towards its target by a step of
	// traction x |target| x (0.1 x normalised share, or 0.04 when the share is under 0.4).
	auto propel = [=](GameObject &o, float forward, float left) {
		auto m = o.local.inverse_rotate(o.momentum);
		float ax = std::fabs(left), az = std::fabs(forward), len = std::sqrt(ax * ax + az * az);
		float nx = len > 0 ? ax / len : 0, nz = len > 0 ? az / len : 0;
		float tr = o.traction;
		float step_x = (nx >= 0.4f ? nx * tr * 0.1f : tr * 0.04f) * ax;
		float step_z = (nz >= 0.4f ? nz * tr * 0.1f : tr * 0.04f) * az;
		std::array<float, 3> d{left < 0 ? -nx : nx, 0, forward < 0 ? -nz : nz};
		float ml = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
		std::array<float, 3> u{0, 0, 0};
		if (ml > 0) {
			u = {m[0] / ml, m[1] / ml, m[2] / ml};
		}
		float dot = u[0] * d[0] + u[1] * d[1] + u[2] * d[2];
		float keep = dot <= 0 ? 1 - tr : (dot < 1 ? 1 - (1 - dot) * tr : 1);
		for (auto &v : m) {
			v *= keep;
		}
		float steer = std::min(std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), 0.2f) * tr;
		for (int k = 0; k < 3; ++k) {
			m[size_t(k)] -= (u[size_t(k)] - d[size_t(k)]) * steer;
		}
		m[0] = approach(m[0], left, step_x);
		m[2] = approach(m[2], forward, step_z);
		o.momentum = o.local.rotate(m);
		o.propelled = true;
	};
	// Set*SpeedMultiplierCaller: +0x4bc scales WalkStandard/RunStandard's propel speed, +0x4c0
	// TurnLeft's angle (both 1 by default; speed pickups and time effects change them).
	reg("COMF2Fighter::SetMovementSpeedMultiplier(float)", [](const NativeContext &c) { if (auto *o = c.self()) o->move_multiplier = c.arg<float>(0); });
	reg("COMF2Fighter::SetTurnSpeedMultiplier(float)", [](const NativeContext &c) { if (auto *o = c.self()) o->turn_multiplier = c.arg<float>(0); });
	// RunStandard(action, factor) (0x35028330): 11 runs straight, 12 / 13 run turning left / right
	// (the turn easing towards factor x the maximum); then TurnLeft and Propel like WalkStandard.
	reg("COMF2Fighter::RunStandard(byte,float)", [=](const NativeContext &c) {
		auto *o = c.self();
		if (!o) return;
		const FighterStats &s = o->stats;
		float &turn = o->turn_rate, &speed = o->forward_speed;
		float k = c.arg<float>(1);
		switch (c.arg<uint8_t>(0)) {
		case 11: speed = k * s.run; turn = 0; break;
		case 12: speed = k * s.run_turning; turn = approach(std::max(turn, 0.0f), k * s.run_turn_max, s.run_turn_step); break;
		case 13: speed = k * s.run_turning; turn = approach(std::min(turn, 0.0f), -k * s.run_turn_max, s.run_turn_step); break;
		default: break;
		}
		turn_left(*o, turn);
		propel(*o, speed * o->move_multiplier, 0);
	});
	reg("COMF2Fighter::WalkStandard(byte)", [=](const NativeContext &c) {  // 0x35027ab0
		auto *o = c.self();
		if (!o) {
			return;
		}
		const FighterStats &s = o->stats;
		float &turn = o->turn_rate, &speed = o->forward_speed;
		switch (c.arg<uint8_t>(0)) {
		case 1: speed = s.walk; turn = 0; break;
		case 2: speed = s.walk_turning; turn = approach(std::max(turn, 0.0f), s.walk_turn_max, s.walk_turn_step); break;
		case 3: speed = s.walk_turning; turn = approach(std::min(turn, 0.0f), -s.walk_turn_max, s.walk_turn_step); break;
		case 4: turn = approach(std::min(turn, 0.0f), -s.turn_max, s.turn_step); speed = 0; break;
		case 5: turn = approach(std::max(turn, 0.0f), s.turn_max, s.turn_step); speed = 0; break;
		case 6: turn = approach(std::min(turn, 0.0f), -1.2f * s.turn_max, 1.2f * s.turn_step); speed = 0; break;
		case 7: turn = approach(std::max(turn, 0.0f), 1.2f * s.turn_max, 1.2f * s.turn_step); speed = 0; break;
		case 8: speed = s.back * 0.75f; turn = 0; break;
		case 9: speed = s.back_turning * 0.75f; turn = approach(std::max(turn, 0.0f), s.back_turn_max, s.back_turn_step); break;
		case 10: speed = s.back_turning * 0.75f; turn = approach(std::min(turn, 0.0f), -s.back_turn_max, s.back_turn_step); break;
		default: break;
		}
		if (turn != 0) {
			turn_left(*o, turn);
		}
		if (speed != 0) {
			propel(*o, speed * o->move_multiplier, 0);  // x the movement multiplier (+0x4bc, 1)
		}
	});
	// PropelForward/Backward/Left/Right callers -> Propel(forward, left); local +x is the fighter's left.
	const std::vector<std::tuple<const char *, float, float>> propels = {
		{"COMF2Fighter::PropelForward(float)", 1, 0}, {"COMF2Fighter::PropelBackward(float)", -1, 0},
		{"COMF2Fighter::PropelLeft(float)", 0, 1}, {"COMF2Fighter::PropelRight(float)", 0, -1}};
	for (auto &[key, fz, fx] : propels) {
		float z = fz, x = fx;
		reg(key, [=](const NativeContext &c) {
			if (auto *o = c.self()) {
				float a = c.arg<float>(0);
				propel(*o, z * a, x * a);
			}
		});
	}
	reg("COMF2Fighter::StandStandard()", [](const NativeContext &c) {
		if (auto *o = c.self()) {
			o->forward_speed = 0;
			o->turn_rate = 0;
		}
	});
	// BeginJumpStandard (0x350288b0): mode 0 normal (NormalJumpStandard), 1 short, 2 super.
	// Action 0x13 jumps forward, 0x14 backward, anything else straight up (keeping the forward
	// momentum). The local forward momentum steps towards the jump's by traction x |target|, then
	// the up speed is added and the fighter leaves the floor.
	auto begin_jump = [=](GameObject &o, uint8_t action, int mode) {
		const FighterStats &s = o.stats;
		bool super = mode == 2;
		float up = super ? s.super_up : s.jump_up, forward = 0;
		if (action == 0x13) forward = super ? s.super_forward : s.jump_forward;
		if (action == 0x14) forward = -(super ? s.super_backward : s.jump_backward);
		auto m = o.local.inverse_rotate(o.momentum);
		m[2] = approach(m[2], forward, std::fabs(forward) * o.traction);
		m[1] += up;
		o.momentum = o.local.rotate(m);
		o.on_floor = false;
		o.propelled = true;
	};
	reg("COMF2Fighter::NormalJumpStandard(byte)", [=](const NativeContext &c) {
		if (auto *o = c.self()) begin_jump(*o, c.arg<uint8_t>(0), 0);
	});
	reg("COMF2Fighter::ShortJumpStandard(byte)", [=](const NativeContext &c) {
		if (auto *o = c.self()) begin_jump(*o, c.arg<uint8_t>(0), 1);
	});
	reg("COMF2Fighter::SuperJumpStandard(byte)", [=](const NativeContext &c) {
		if (auto *o = c.self()) begin_jump(*o, c.arg<uint8_t>(0), 2);
	});
	// JumpStandard (0x35027620, vtable +0x1f0; JumpStandard and FallStandard pass (action, 1, 1),
	// FallFromLaunchStandard (action, 0, 0)): steering in the air. 0xe/0xf turn, 0x11/0x12 drift
	// forward/back (with the drift flag), then -- with the gravity flag -- the fighter's extra air
	// gravity (+0x52c x 0.037) is added.
	auto jump_standard = [=](const NativeContext &c, bool gravity, bool drift) {
		auto *o = c.self();
		if (!o) {
			return;
		}
		const FighterStats &s = o->stats;
		float &turn = o->turn_rate;
		float push = 0;
		switch (c.arg<uint8_t>(0)) {
		case 0xe: turn = approach(std::max(turn, 0.0f), s.air_turn_max, s.air_turn_step); break;
		case 0xf: turn = approach(std::min(turn, 0.0f), -s.air_turn_max, s.air_turn_step); break;
		case 0x11: turn = 0; push = drift ? s.air_forward * 0.75f : 0; break;
		case 0x12: turn = 0; push = drift ? s.air_backward * 0.75f : 0; break;
		default: turn = 0; break;
		}
		turn_left(*o, turn);
		auto m = o->local.inverse_rotate(o->momentum);
		m[2] += push;
		if (gravity) m[1] -= s.air_gravity * 0.037f;
		o->momentum = o->local.rotate(m);
	};
	reg("COMF2Fighter::JumpStandard(byte)", [=](const NativeContext &c) { jump_standard(c, true, true); });
	reg("COMF2Fighter::FallStandard(byte)", [=](const NativeContext &c) { jump_standard(c, true, true); });
	reg("COMF2Fighter::FallFromLaunchStandard(byte)", [=](const NativeContext &c) { jump_standard(c, false, false); });
	reg("CJoystickInputManager::bool IsOrientationLeft()", [](const NativeContext &c) { c.ret_bool(c.input().left); });
	reg("CJoystickInputManager::bool IsOrientationRight()", [](const NativeContext &c) { c.ret_bool(c.input().right); });
	// SmoothStopToStopCaller (Core 0x70045d30): two quadratic halves meeting at 0.5.
	auto stop_to_stop = [](float x) {
		if (x < 0.5f) return (x + x) * (x + x) * 0.5f;
		float y = 1 - ((x - 0.5f) + (x - 0.5f));
		return (1 - y * y) * 0.5f + 0.5f;
	};
	reg("CObject::float SmoothStopToStop(float)", [=](const NativeContext &c) { c.ret_f32(stop_to_stop(c.arg<float>(0))); });
	// SmoothStopToStopWithPowerCaller: power p blends between linear (0) and the curve (1).
	reg("CObject::float SmoothStopToStop(float,float)", [](const NativeContext &c) {
		float x = c.arg<float>(0), p = c.arg<float>(1), lin = (1 - p) * x;
		if (x < 0.5f) {
			c.ret_f32((x + x) * (x + x) * p * 0.5f + lin);
			return;
		}
		float y = 1 - ((x - 0.5f) + (x - 0.5f));
		c.ret_f32(p * 0.5f + (1 - y * y) * p * 0.5f + lin);
	});
	// DE_COMF2Fighter::AnimTurn(EMovementAction) (0x35026e80): eases the lean towards +max for
	// actions 2 / 12, -max for 3 / 13 (walk / run, +0x530 / +0x538) and back to 0 otherwise, by
	// +0x53c a call, and turns the body by it about Y this frame.
	reg("COMF2Fighter::AnimTurn(byte)", [](const NativeContext &c) {
		GameObject *o = c.self();
		if (!o) return;
		const FighterStats &s = o->stats;
		float &a = o->anim_turn, step = s.lean_step;
		auto ease = [&](float target) {
			if (target >= 0 ? a < 0 : a > 0) a = 0;
			if (a < target) a = std::min(a + step, target);
			else if (a > target) a = std::max(a - step, target);
		};
		switch (c.arg<uint8_t>(0)) {
		case 2: ease(s.walk_lean); break;
		case 3: ease(-s.walk_lean); break;
		case 12: ease(s.run_lean); break;
		case 13: ease(-s.run_lean); break;
		default:
			if (a < 0) a = std::min(a + step, 0.0f);
			else if (a > 0) a = std::max(a - step, 0.0f);
		}
		anim_rotate(*o, 1, a * 0.017453292f);
	});
	// AnimTurn(angle, power) (0x35027260) only stores the angle.
	reg("COMF2Fighter::AnimTurn(float,float)", [](const NativeContext &c) { if (auto *o = c.self()) o->anim_turn = c.arg<float>(0); });
	// DE_COMF2Fighter::AnimReset (0x35001260) overrides the creature's: it clears the lean only.
	reg("COMF2Fighter::AnimReset()", [](const NativeContext &c) {
		if (auto *o = c.self()) o->anim_turn = 0;
	});

	// VerifyCaller (Creature 0x150020c0): this = input manager, arg = sequence.
	reg("CJoystickInputManager::bool Verify(class Sequence)", [](const NativeContext &c) {
		auto it = c.world.input_handles.find(c.arg<uint32_t>(0));
		GameObject *who = c.owner();
		bool ok = it != c.world.input_handles.end() && who && who->user_controlled && c.world.verify(*it->second);
		if (getenv("OPENBG_DEBUG_INPUT") && it != c.world.input_handles.end() && ok) {
			fprintf(stderr, "frame %u verify %s:", c.world.frame(), ok ? "yes" : "no");
			for (auto &r : *it->second) {
				fprintf(stderr, " [v%x m%x d%x age %u-%u gap %u-%u]", r.value, r.mask, r.directions, r.min_age,
				        r.max_age, r.min_gap, r.max_gap);
			}
			fprintf(stderr, "\n");
		}
		c.ret_bool(ok);
	});

	// --- Pilot stats (neutral until pilot descriptions are decoded) -------------------------------
	// Pilot natives live in natives_ai.cpp (each fighter has its own pilot).

	// --- Registrations and hooks the scripts set up; accepted and ignored for now ---------------
	for (const char *k : {"CCreature::RegisterStandardState()", "CCreature::RegisterLastStateNoReplicate()",
	                      "CCreature::RegisterLastStateAsRequiredCorrection()", "CCreature::RegisterHighSignificanceState()",
	                      "CCreature::RegisterLowSignificanceState()", "COMF2Fighter::RegisterGetHitState()",
	                      "CCreature::RegisterLastStateNoInterrupt()", "CCreature::RegisterLastStateAsCorrection()",
	                      "COMF2Fighter::RegisterStandardAttack()", "COMF2Fighter::ResetCurrentHitList()",
	                      "COMF2Fighter::RegisterRecoveryState()",
	                      "CScene::PreCacheObject(class CObject)"}) {
		stub(k);
	}
}

}  // namespace game
