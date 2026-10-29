#include "game/particles.h"

#include "game/session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace game {

namespace {

using Vec = std::array<float, 3>;
using Rows = std::array<float, 9>;

constexpr float kGravity = -0.0098f;  // DE_CScene::Gravity: units per ms per ms, -y
constexpr float kMaxStep = 1000;      // the instance's step is capped (generator +0x7c, ObjInitContents)

Vec add(const Vec &a, const Vec &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec sub(const Vec &a, const Vec &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec mul(const Vec &a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
float length(const Vec &a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
Vec normalize(const Vec &a) {
	float l = length(a);
	return l > 0 ? mul(a, 1 / l) : a;
}
// DE_TCoord3D::Rotate by a coordinate system (rows = local axes in parent space).
Vec rotate(const Vec &v, const Rows &r) {
	return {v[0] * r[0] + v[1] * r[3] + v[2] * r[6], v[0] * r[1] + v[1] * r[4] + v[2] * r[7],
	        v[0] * r[2] + v[1] * r[5] + v[2] * r[8]};
}
Vec inverse_rotate(const Vec &v, const Rows &r) {
	return {v[0] * r[0] + v[1] * r[1] + v[2] * r[2], v[0] * r[3] + v[1] * r[4] + v[2] * r[5],
	        v[0] * r[6] + v[1] * r[7] + v[2] * r[8]};
}
Vec cross(const Vec &a, const Vec &b) {
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
// DE_CCoordinateSystem::Set(vector): a frame whose Z axis is the direction.
Rows frame_from_z(const Vec &dir) {
	Vec z = normalize(dir);
	if (length(z) == 0) return {1, 0, 0, 0, 1, 0, 0, 0, 1};
	Vec up = std::fabs(z[1]) > 0.99f ? Vec{1, 0, 0} : Vec{0, 1, 0};
	Vec x = normalize(cross(up, z));
	Vec y = cross(z, x);
	return {x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
}
// DE_CCoordinateSystem::RotateLocalX/Y/Z: the frame turned about its own axis (the other two
// rows rotate in their plane).
Rows rotate_local(const Rows &r, int axis, float a) {
	float c = std::cos(a), s = std::sin(a);
	int i = (axis + 1) % 3, j = (axis + 2) % 3;
	Rows out = r;
	for (int k = 0; k < 3; ++k) {
		float ri = r[size_t(i * 3 + k)], rj = r[size_t(j * 3 + k)];
		out[size_t(i * 3 + k)] = ri * c + rj * s;
		out[size_t(j * 3 + k)] = rj * c - ri * s;
	}
	return out;
}

Rows rotate_axis(const Rows &r, int axis, float a) {
	float c = std::cos(a), s = std::sin(a);
	Rows out = r;
	for (int i = 0; i < 3; ++i) {
		Vec v{r[size_t(i * 3)], r[size_t(i * 3 + 1)], r[size_t(i * 3 + 2)]};
		Vec o;
		switch (axis) {
		case 0: o = {v[0], v[1] * c - v[2] * s, v[1] * s + v[2] * c}; break;
		case 1: o = {v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c}; break;
		default: o = {v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2]}; break;
		}
		for (int k = 0; k < 3; ++k) out[size_t(i * 3 + k)] = o[size_t(k)];
	}
	return out;
}

float rand_to(World &w, float x) { return random01(w) * x; }               // MATH::Rand
float center_rand(World &w, float x) { return (random01(w) - 0.5f) * x; }  // MATH::CenterRand

constexpr uint32_t kRelative = 2, kAlive = 4, kNew = 8, kLooping = 0x10;

// DE_CRendererDirect3D::MarkVisibleMeshElemets: an element is processed (stepped) while its own
// actor is renderable (+0x108, SetRendered / SetUnrendered: not inherited) and its mesh properties
// are enabled.
bool shown(const GameObject *o) { return o->visible && !o->meshes_disabled; }

// The actor's momentum as DE_CActor::Momentum reports it (units per millisecond).
Vec actor_momentum(const GameObject *a) {
	for (; a; a = a->parent) {
		if (a->fighter) return mul(a->momentum, 1.0f / 16);
	}
	return {0, 0, 0};
}

}  // namespace

float scene_time_ms(const World &w) { return w.scene_ms(); }

ModifierControl &ParticleSystem::control(GameObject *modifier) { return controls_[modifier]; }

const de::ParticleRendererDef *ParticleSystem::renderer(const de::ObjectRef &r) {
	auto key = std::make_pair(r.file, r.index);
	auto it = defs_.find(key);
	if (it == defs_.end()) {
		it = defs_.emplace(key, std::make_unique<de::ParticleRendererDef>(de::read_particle_renderer(w_.data(), r))).first;
	}
	return it->second.get();
}

void ParticleSystem::set_spawning(GameObject *actor, bool on) {
	actor_spawning_[actor] = on;
	for (auto &in : instances_) {
		if (in->actor == actor) in->spawning = on;
	}
}

void ParticleSystem::set_generator_spawning(const de::ObjectRef &generator, const GameObject *actor, bool on) {
	generator_spawning_[{{generator.file, generator.index}, actor}] = on;
	for (auto &in : instances_) {
		const de::ObjectRef &g = in->def->generator.object;
		if (in->actor == actor && g.file == generator.file && g.index == generator.index) in->spawning = on;
	}
}

ParticleInstance &ParticleSystem::instance(GameObject *actor, const de::ParticleRendererDef *def) {
	for (auto &in : instances_) {
		if (in->actor == actor && in->def == def) return *in;
	}
	auto in = std::make_unique<ParticleInstance>();
	static const bool debug = getenv("OPENBG_DEBUG_PARTICLES") != nullptr;
	if (debug) fprintf(stderr, "[particles %u] new instance of %s on %s\n", w_.frame(), def->object.info().name.c_str(), actor ? actor->name.c_str() : "-");
	in->def = def;
	in->actor = actor;
	in->time = scene_time_ms(w_);
	if (auto sp = actor_spawning_.find(actor); sp != actor_spawning_.end()) in->spawning = sp->second;
	const de::ObjectRef &g = def->generator.object;
	if (auto gs = generator_spawning_.find({{g.file, g.index}, actor}); gs != generator_spawning_.end()) in->spawning = gs->second;
	// The actor's own copies of the generator's modifiers (where scripts reach them by name).
	std::function<GameObject *(GameObject *, const de::ObjectRef &)> find = [&](GameObject *o, const de::ObjectRef &r) -> GameObject * {
		if (o->file == r.file && o->index == r.index) return o;
		for (auto *c : o->children) {
			if (GameObject *hit = find(c, r)) return hit;
		}
		return nullptr;
	};
	for (auto &m : def->generator.modifiers) {
		GameObject *mo = find(actor, m.object);
		if (!mo) mo = w_.global_object(m.object);
		in->modifier_objects.push_back(mo);
		// DE_CParticleModifier::Begin.
		auto &pm = in->per_modifier.emplace_back();
		ModifierControl &ctl = control(mo);
		if (!ctl.initialised) {
			ctl.initialised = true;
			ctl.spawn_time = m.spawn_time;
			ctl.spawn_time_variation = m.spawn_time_variation;
			ctl.life_span = m.life_span;
			ctl.life_span_variation = m.life_span_variation;
			ctl.spawn_size = m.spawn_size;
			ctl.spawn_size_variation = m.spawn_size_variation;
			ctl.spawning = m.spawning;
		}
		if (m.is_spawner()) {
			pm.countdown = rand_to(w_, ctl.spawn_time_variation) + ctl.spawn_time;
			pm.initial = m.initial_count + ctl.burst;
			pm.mesh_burst = m.kind == "MeshSpawner" ? m.mesh_burst : 0;
			ctl.burst = 0;
			if (m.interpolate) in->interpolate_position = in->interpolate_rotation = true;
		}
	}
	instances_.push_back(std::move(in));
	return *instances_.back();
}

std::vector<const de::ParticleRendererDef *> &ParticleSystem::renderers_of(GameObject *o) {
	auto it = actor_renderers_.find(o);
	if (it == actor_renderers_.end()) {
		std::vector<const de::ParticleRendererDef *> list;
		for (auto &r : w_.properties_of(*o)) {
			if (de::is_particle_renderer(r)) list.push_back(renderer(r));
		}
		it = actor_renderers_.emplace(o, std::move(list)).first;
	}
	return it->second;
}

std::vector<NewParticleInfo> *ParticleSystem::requests(GameObject *actor, const de::ObjectRef &r, const de::ObjectRef &spawner) {
	if (!actor || !de::is_particle_renderer(r)) return nullptr;
	const de::ParticleRendererDef *def = renderer(r);
	if (!def->generator.object) return nullptr;
	auto &list = renderers_of(actor);
	if (std::find(list.begin(), list.end(), def) == list.end()) list.push_back(def);
	ParticleInstance &in = instance(actor, def);
	for (size_t i = 0; i < def->generator.modifiers.size(); ++i) {
		auto &m = def->generator.modifiers[i].object;
		if (m.file == spawner.file && m.index == spawner.index && in.modifier_objects[i]) {
			return &control(in.modifier_objects[i]).requests;
		}
	}
	return nullptr;
}

void ParticleSystem::set_trail_enabled(GameObject *mesh, bool on) {
	trail_enabled_[mesh] = on;
	for (auto &t : trails_) {
		if (t->mesh == mesh) t->enabled = on;
	}
}

void ParticleSystem::step_trails(float now) {
	for (GameObject *o : w_.scene_objects()) {
		if (!o->is_actor || !o->file || !shown(o) || !in_scene(*o)) continue;
		auto it = actor_streamers_.find(o);
		if (it == actor_streamers_.end()) {
			std::vector<const de::PointStreamerDef *> list;
			for (auto &r : w_.properties_of(*o)) {
				if (!de::is_point_streamer(r)) continue;
				auto key = std::make_pair(r.file, r.index);
				auto d = streamer_defs_.find(key);
				if (d == streamer_defs_.end()) d = streamer_defs_.emplace(key, std::make_unique<de::PointStreamerDef>(de::read_point_streamer(w_.data(), r))).first;
				list.push_back(d->second.get());
			}
			it = actor_streamers_.emplace(o, std::move(list)).first;
		}
		for (const de::PointStreamerDef *def : it->second) {
			Trail *t = nullptr;
			for (auto &x : trails_) {
				if (x->actor == o && x->def == def) t = x.get();
			}
			if (!t) {
				auto nt = std::make_unique<Trail>();
				nt->actor = o;
				nt->def = def;
				nt->mesh = nearest_copy(o, def->object);
				if (auto e = trail_enabled_.find(nt->mesh); nt->mesh && e != trail_enabled_.end()) nt->enabled = e->second;
				t = trails_.emplace_back(std::move(nt)).get();
				static const bool debug = getenv("OPENBG_DEBUG_PARTICLES") != nullptr;
				if (debug) fprintf(stderr, "[particles %u] trail %s on %s (every %g ms, life %g)\n", w_.frame(), def->object.info().name.c_str(), o->name.c_str(), def->interval, def->life);
			}
			float dt = t->last >= 0 ? now - t->last : 0;
			t->last = now;
			t->stepped = w_.frame();
			// A new point once `interval` has passed (texture v = time / length).
			if (t->enabled && t->next < now) {
				t->next = now + def->interval;
				auto p = o->world().t;
				t->points.push_back({{p[0], p[1], p[2]}, now, def->texture_length != 0 ? now / def->texture_length : 0});
			}
			// Points older than the life go; the rest rise.
			size_t gone = 0;
			while (gone < t->points.size() && now > def->life + t->points[gone].time) ++gone;
			t->points.erase(t->points.begin(), t->points.begin() + long(gone));
			for (auto &p : t->points) p.pos[1] += dt * def->rise;
		}
	}
	// Trails of actors that left the scene go once their points have.
	trails_.erase(std::remove_if(trails_.begin(), trails_.end(), [&](const std::unique_ptr<Trail> &t) { return t->stepped + 2 < w_.frame(); }),
	              trails_.end());
}

void ParticleSystem::step_sparks(float now) {
	for (size_t i = 0; i < sparks_.size();) {
		SparkBurst &b = sparks_[i];
		// DE_CSparkProcess::Tick (OMF 0x3502f890): once when made, then as a temporal at the
		// fighter's rate (16 ms) on the scene clock's multiples of it.
		auto tick = [&] {
			b.life -= 0.005f;
			if (b.light) {
				b.light_intensity = std::max(0.0f, b.light_intensity - 0.05f);
				lights_[b.light].intensity = b.light_intensity;
			}
		};
		constexpr float kRate = 16;
		if (b.next_tick < 0) {
			tick();
			b.next_tick = (std::floor(now / kRate) + 1) * kRate;
		}
		while (now >= b.next_tick && b.life >= 0) {
			tick();
			b.next_tick += kRate;
		}
		if (b.life < 0 || !b.actor || w_.object(b.actor->handle) != b.actor) {
			if (b.actor && w_.object(b.actor->handle) == b.actor) w_.destroy(b.actor);
			sparks_.erase(sparks_.begin() + long(i));
			continue;
		}
		de::Transform t = b.actor->world();
		if (!b.setup) {
			// SetupSparks: every spark starts at the actor, jittered by up to 1.5 on each axis.
			b.setup = true;
			b.last_ms = now;
			static const bool debug = getenv("OPENBG_DEBUG_PARTICLES") != nullptr;
			if (debug) {
				fprintf(stderr, "[sparks %u] %s: %d sparks (type %d) at %.1f %.1f %.1f, image %s, light %s\n", w_.frame(), b.actor->name.c_str(), b.count,
				        b.type, t.t[0], t.t[1], t.t[2], b.image ? b.image.info().name.c_str() : "-", b.light ? b.light->name.c_str() : "-");
			}
			b.sparks.resize(size_t(std::max(0, b.count)));
			Vec z{t.r[6], t.r[7], t.r[8]};
			for (auto &sp : b.sparks) {
				sp.life = 1;
				sp.decay = random01(w_) * 0.003144963f + 0.00078624f;
				sp.width = random01(w_) * 0.2f + 0.16f;
				sp.pos = t.t;
				for (auto &v : sp.pos) v += random01(w_) * 3 - 1.5f;
				if (b.type == 0) {
					float f2 = 1 - random01(w_), f3 = random01(w_) * 70;
					sp.pos = add(sp.pos, mul(z, f3 * 0.10752f));
					sp.vel = mul(z, f2 * 0.10752f);
					for (auto &v : sp.vel) v += (random01(w_) * 0.11424f - 0.05712f) * (1 - f2);
				} else {
					float f2 = random01(w_);
					Vec v{(random01(w_) * 0.01344f - 0.00672f) * f2, (random01(w_) * 0.1344f - 0.0672f) * f2, random01(w_) * 0.0672f * f2};
					sp.vel = t.rotate(v);
				}
				sp.pos = add(sp.pos, mul(sp.vel, 80));
			}
		} else {
			float dt = std::min(now - b.last_ms, 1000.0f);
			b.last_ms = now;
			for (auto &sp : b.sparks) {
				if (sp.life <= 0) continue;
				sp.pos = add(sp.pos, mul(sp.vel, dt));
				sp.life -= dt * sp.decay;
				sp.vel[1] -= dt * 3.84e-5f;
			}
		}
		++i;
	}
}

void ParticleSystem::step() {
	float now = scene_time_ms(w_);
	for (GameObject *o : w_.scene_objects()) {
		if (!o->is_actor || !o->file || !shown(o) || !in_scene(*o)) continue;
		for (auto *def : renderers_of(o)) {
			if (!def->generator.object) continue;
			step(instance(o, def), now);
		}
	}
	for (GameObject *k : kills_) w_.destroy(k);
	kills_.clear();
	step_sparks(now);
	step_trails(now);
	// OPENBG_DEBUG_PARTICLES=1: the live effects once a second.
	static const bool debug = getenv("OPENBG_DEBUG_PARTICLES") != nullptr;
	if (debug) {
		for (auto &[f, set] : trailers_) {
			for (auto &t : set) {
				if (t.points.size() >= 2 && t.points.back().born == w_.frame()) {
					fprintf(stderr, "[trailer %u] %s %s: %zu points, colour %.2f %.2f %.2f, emitter %s\n", w_.frame(), f->name.c_str(),
					        t.object->name.c_str(), t.points.size(), t.color[0], t.color[1], t.color[2],
					        t.emitter ? t.emitter->name.c_str() : "-");
				}
			}
		}
	}
	if (debug && w_.frame() % 60 == 0) {
		for (auto &in : instances_) {
			if (in->stepped == w_.frame() && !in->particles.empty()) {
				const auto &p = in->particles.front();
				fprintf(stderr, "[particles %u] %s/%s: %zu (first at %.1f %.1f %.1f age %.2f size %.2f)\n", w_.frame(),
				        in->actor->parent ? in->actor->parent->name.c_str() : "-", in->def->object.info().name.c_str(),
				        in->particles.size(), p.pos[0], p.pos[1], p.pos[2], p.age, p.size);
			}
		}
	}
}

void ParticleSystem::step(ParticleInstance &in, float now) {
	const auto &gen = in.def->generator;
	float dt = std::clamp(now - in.time, 0.0f, kMaxStep);
	in.time = now;
	in.dt = dt;
	in.stepped = w_.frame();
	if (dt == 0) return;
	World &w = w_;

	// Where the actor is (world-space generators place their particles in its frame).
	if (!gen.local) {
		de::Transform t = in.actor->world();
		Vec pos{t.t[0], t.t[1], t.t[2]};
		in.actor_move = in.placed && in.interpolate_position ? sub(pos, in.actor_pos) : Vec{0, 0, 0};
		in.actor_pos = pos;
		in.prev_rot = in.placed && in.interpolate_rotation ? in.actor_rot : t.r;
		in.actor_rot = t.r;
		in.placed = true;
	}
	for (auto &p : in.particles) p.flags &= ~kNew;

	auto place = [&](Particle &p, Vec v) {
		if (in.placed) {
			Vec r = rotate(v, in.actor_rot);
			if (in.interpolate_rotation) r = add(r, mul(sub(rotate(v, in.prev_rot), r), p.spawn_fraction));
			v = add(sub(r, mul(in.actor_move, p.spawn_fraction)), in.actor_pos);
		}
		p.pos = p.prev = v;
	};

	// 2. Spawning (ManageParticles).
	in.created = 0;
	for (size_t mi = 0; mi < gen.modifiers.size(); ++mi) {
		const auto &m = gen.modifiers[mi];
		auto &pm = in.per_modifier[mi];
		ModifierControl &ctl = control(in.modifier_objects[mi]);
		size_t first_new = in.particles.size();
		uint32_t flags = kNew | kAlive | (m.looping ? kLooping : 0);
		auto life = [&](bool plus_one) {
			if (ctl.life_span == 0 && ctl.life_span_variation == 0) return 0.0f;
			return 1 / (rand_to(w, ctl.life_span_variation) + ctl.life_span + (plus_one ? 1 : 0));
		};
		if (m.kind == "ManualParticleSpawner") {
			// DE_CManualParticleSpawner::ManageParticles.
			static const bool debug = getenv("OPENBG_DEBUG_PARTICLES") != nullptr;
			if (debug && !ctl.requests.empty()) {
				fprintf(stderr, "[particles %u] %s spawns %zu at %.1f %.1f %.1f (size %.2f, %.0f ms)\n", w.frame(),
				        m.object.info().name.c_str(), ctl.requests.size(), ctl.requests[0].pos[0], ctl.requests[0].pos[1],
				        ctl.requests[0].pos[2], ctl.requests[0].size, ctl.requests[0].life_span);
			}
			for (auto &info : ctl.requests) {
				Particle &p = in.particles.emplace_back();
				p.flags = kNew | kAlive;
				p.serial = pm.serial++;
				p.size = info.size;
				p.inv_life = info.life_span != 0 ? 1 / info.life_span : 0;
				p.pos = info.pos;
				if (info.relative) {
					de::Transform t = in.actor->world();
					p.flags |= kRelative;
					p.pos = add(rotate(p.pos, t.r), Vec{t.t[0], t.t[1], t.t[2]});
				}
				if (m.interpolate && info.time != 0) p.age = (now - info.time) * p.inv_life;
				if (m.manual_orientation) p.orientation = info.orientation;
				p.intensity = info.intensity;
				p.rotation = info.rotation;
				p.prev = p.pos;
			}
			ctl.requests.clear();
		} else if (m.is_spawner()) {
			if (m.kind == "LighteningSpawner" && m.bolt_follow && pm.bolt_target_set && !m.bolt_targets.empty()) {
				// DE_CLighteningSpawner::ManageParticles (0x4000bb70): with follow, the bolts' points
				// move with the first target, each by how far along its bolt it is.
				if (GameObject *target = nearest_copy(in.actor, m.bolt_targets[0])) {
					Vec now = target->world().t;
					if (gen.local) {
						de::Transform t = in.actor->world();
						now = t.inverse_rotate(sub(now, Vec{t.t[0], t.t[1], t.t[2]}));
					}
					Vec d = sub(now, pm.bolt_target);
					pm.bolt_target = now;
					for (auto &p : in.particles) {
						if (p.bolt_t >= 0) p.pos = add(p.pos, mul(d, p.bolt_t));
					}
				}
			}
			// DE_CParticleModifier::CommonSpawnerManageParticles.
			float step = in.dt;
			pm.countdown -= step;
			if (!ctl.spawning || !in.spawning) {
				pm.countdown = 0;
			} else if (ctl.life_span == 0 && ctl.life_span_variation == 0) {
				ctl.life_span = 1;
			}
			auto spawn = [&](float fraction) -> Particle & {
				Particle &p = in.particles.emplace_back();
				p.spawn_fraction = fraction;
				p.size = (rand_to(w, ctl.spawn_size_variation) + ctl.spawn_size) * in.size_multiplier;
				p.serial = pm.serial++;
				p.flags = flags;
				return p;
			};
			if (pm.countdown < 0) {
				if (pm.countdown < -ctl.life_span_variation - ctl.life_span) {
					pm.countdown = -ctl.life_span - ctl.life_span_variation * 0.5f;
					step = -pm.countdown;
				}
				while (pm.countdown < 0) {
					Particle &p = spawn(step > 0 ? -pm.countdown / step : 0);
					p.inv_life = life(true);
					pm.countdown += rand_to(w, ctl.spawn_time_variation) + ctl.spawn_time;
					if (ctl.spawn_time <= 0 && ctl.spawn_time_variation <= 0) pm.countdown = 0;
					if (pm.countdown < 0) {
						p.age_step = -pm.countdown * p.inv_life;
						p.age += p.age_step;
					}
					for (int32_t e = 0; e < m.extra_per_spawn; ++e) {
						Particle &q = spawn(m.interpolate && step > 0 ? -pm.countdown / step : 0);
						q.inv_life = life(false);
					}
				}
			}
			ctl.burst += pm.initial;
			pm.initial = 0;
			for (; ctl.burst > 0; --ctl.burst) {
				Particle &p = spawn(0);
				p.size = rand_to(w, ctl.spawn_size_variation) + ctl.spawn_size;
				p.inv_life = life(false);
			}
			const float beam_length = ctl.beam_length >= 0 ? ctl.beam_length : m.beam_length;
			const float beam_start = ctl.beam_start >= 0 ? ctl.beam_start : m.beam_start;
			static const bool dbg_beam = getenv("OPENBG_DEBUG_PARTICLES") != nullptr;
			if (dbg_beam && m.kind == "BeamSpawner")
				fprintf(stderr, "[particles %u] beam %s: length %.1f (data %.1f), start %.1f, spacing %.1f~%.1f, done %d\n", w.frame(),
				        m.object.info().name.c_str(), beam_length, m.beam_length, beam_start, m.beam_spacing, m.beam_spacing_variation, pm.beam_done);
			if (m.kind == "BeamSpawner" && !pm.beam_done && beam_length != 0) {
				// DE_CBeamSpawner::ManageParticles (0x4000a460): once per instance, particles along the
				// actor's Z from the start distance to the length, at the spacing (+ up to its
				// variation), each on a ring of the radius (+ up to its variation) at a random angle.
				pm.beam_done = true;
				for (float z = beam_start; z < beam_length;) {
					Particle &p = spawn(0);
					Vec v{0, 0, z};
					if (m.beam_radius != 0 || m.beam_radius_variation != 0) {
						float a = float(int(random01(w) * 256) & 0xff) * (6.2831853f / 256);
						float sn = std::sin(a), cs = std::cos(a);
						v[0] = sn * m.beam_radius + rand_to(w, sn * m.beam_radius_variation);
						v[1] = cs * m.beam_radius + rand_to(w, cs * m.beam_radius_variation);
					}
					z += rand_to(w, m.beam_spacing_variation) + m.beam_spacing;
					p.age = 0;
					p.size = rand_to(w, ctl.spawn_size_variation) + ctl.spawn_size;
					p.inv_life = ctl.life_span == 0 && ctl.life_span_variation == 0
					                 ? 0
					                 : 1 / (rand_to(w, ctl.life_span_variation) + ctl.life_span + 1);
					p.flags |= 0x100;
					place(p, v);
					if (m.beam_spacing <= 0 && m.beam_spacing_variation <= 0) break;
				}
				static const bool debug = getenv("OPENBG_DEBUG_PARTICLES") != nullptr;
				if (debug && in.particles.size() > first_new) {
					const Vec &a = in.particles[first_new].pos, &b = in.particles.back().pos;
					fprintf(stderr, "[particles %u] %s lays %zu from %.1f %.1f %.1f to %.1f %.1f %.1f\n", w.frame(), m.object.info().name.c_str(),
					        in.particles.size() - first_new, a[0], a[1], a[2], b[0], b[1], b[2]);
				}
			}
			if (m.kind == "MeshSpawner") {
				// DE_CMeshSpawner::ManageParticles: its burst puts one particle on every vertex.
				if (pm.mesh_burst != 0) {
					pm.mesh_burst = 0;
					size_t n = std::max<size_t>(1, m.mesh_vertices.size());
					for (size_t v = 0; v < n; ++v) {
						Particle &p = spawn(0);
						p.size = rand_to(w, ctl.spawn_size_variation) + ctl.spawn_size;
						p.inv_life = life(true);
						p.flags |= 0x100;  // placed: CommonSpawnerNewParticles leaves it
						place(p, m.mesh_vertices.empty() ? Vec{0, 0, 0} : m.mesh_vertices[v]);
					}
				}
			}
			// CommonSpawnerNewParticles: where the spawner's new particles start.
			for (size_t i = first_new; i < in.particles.size(); ++i) {
				Particle &p = in.particles[i];
				if (p.flags & 0x100) {
					p.flags &= ~0x100u;
					continue;
				}
				if (m.kind == "BeamSpawner") continue;  // its CommonSpawnerNewParticles places nothing
				if (m.kind == "LighteningSpawner") {
					// DE_CLighteningSpawner::CommonSpawnerNewParticles (0x4000bf40): the particle at
					// the actor, then a bolt from it to every target: a start point, the bolt's
					// subdivisions (Bolt), and an end point at the target that ends the stream.
					place(p, {0, 0, 0});
					p.bolt_t = -1;
					const Vec from = p.pos;
					const float size = p.size, inv_life = p.inv_life;
					for (const auto &tr : m.bolt_targets) {
						GameObject *target = nearest_copy(in.actor, tr);
						if (!target) continue;
						Vec to = target->world().t;
						if (gen.local) {
							de::Transform t = in.actor->world();
							to = t.inverse_rotate(sub(to, Vec{t.t[0], t.t[1], t.t[2]}));
						}
						pm.bolt_target = to;
						pm.bolt_target_set = true;
						auto point = [&](const Vec &at, float along, float sz, bool connected) {
							Particle &q = in.particles.emplace_back();
							q.flags = kNew | kAlive | 0x100 | (connected ? kRelative : 0);
							q.serial = pm.serial++;
							q.pos = q.prev = at;
							q.bolt_t = along;
							q.size = sz;
							q.inv_life = inv_life;
						};
						point(from, 0, m.bolt_taper ? 0 : size, true);
						// Bolt(a, b, t, life, half): segments longer than the shortest split at their
						// middle, moved by up to +- jitter x length / 2 on each axis; t counts down
						// from 1 at the start, the points store 1 - t.
						std::function<void(const Vec &, const Vec &, float, float)> bolt = [&](const Vec &a, const Vec &b, float t, float half) {
							Vec d = sub(b, a);
							float l = length(d);
							float q = half * 0.5f;
							if (!(m.bolt_min_segment < l)) return;
							float j = l * m.bolt_jitter;
							Vec mid{d[0] * 0.5f + center_rand(w, j) + a[0], d[1] * 0.5f + center_rand(w, j) + a[1], d[2] * 0.5f + center_rand(w, j) + a[2]};
							bolt(a, mid, t + q, q);
							point(mid, 1 - t, m.bolt_taper ? t * size : size, true);
							bolt(mid, b, t - q, q);
						};
						bolt(from, to, 0.5f, 0.5f);
						point(to, 1, m.bolt_taper ? 0 : size, false);
					}
					continue;
				}
				Vec v{0, 0, 0};
				if (m.kind == "RandomEllipsoidSpawner") {
					Vec c{(m.box[0] + m.box[3]) / 2, (m.box[1] + m.box[4]) / 2, (m.box[2] + m.box[5]) / 2};
					Vec half{m.box[3] - c[0], m.box[4] - c[1], m.box[5] - c[2]};
					if (ctl.radius >= 0) half = {ctl.radius, ctl.radius, ctl.radius};
					Vec d = normalize({center_rand(w, 2), center_rand(w, 2), center_rand(w, 2)});
					v = {d[0] * rand_to(w, half[0]) + c[0], d[1] * rand_to(w, half[1]) + c[1], d[2] * rand_to(w, half[2]) + c[2]};
				} else if (m.kind == "RandomBoxSpawner") {
					v = {rand_to(w, m.box[3] - m.box[0]) + m.box[0], rand_to(w, m.box[4] - m.box[1]) + m.box[1],
					     rand_to(w, m.box[5] - m.box[2]) + m.box[2]};
				} else if (m.kind == "MeshSpawner" && !m.mesh_vertices.empty()) {
					int n = int(m.mesh_vertices.size());
					int k = 0;
					if (m.mesh_order == 0) {
						k = std::min(n - 1, int(random01(w) * float(n)));
					} else if (m.mesh_order == 2) {
						if (pm.serial <= 0) pm.serial = n;
						k = --pm.serial % n;
					} else {
						k = (p.serial % n + n) % n;
					}
					v = m.mesh_vertices[size_t(k)];
				}
				place(p, v);
			}
		}
		in.created += int(in.particles.size() - first_new);
	}

	// 3. New particles' data (ManageParticleUserData).
	if (in.created > 0) {
		auto fresh = [&](auto fn) {
			for (auto &p : in.particles) {
				if (p.flags & kNew) fn(p);
			}
		};
		Vec am = actor_momentum(in.actor);
		for (const auto &m : gen.modifiers) {
			if (m.kind == "NewParticleModifier") {
				if (m.rotation_mode == 1) fresh([&](Particle &p) { p.rotation = rand_to(w, 1); });
				switch (m.orientation_mode) {
				case 1: fresh([&](Particle &p) { p.orientation = frame_from_z(sub(p.pos, in.actor_pos)); }); break;
				case 2: fresh([&](Particle &p) { p.orientation = frame_from_z(sub(in.actor_pos, p.pos)); }); break;
				case 3:
					fresh([&](Particle &p) {
						Rows r{1, 0, 0, 0, 1, 0, 0, 0, 1};
						r = rotate_axis(r, 0, rand_to(w, 3.14159265f));
						r = rotate_axis(r, 1, rand_to(w, 3.14159265f));
						p.orientation = rotate_axis(r, 2, rand_to(w, 3.14159265f));
					});
					break;
				case 4: fresh([&](Particle &p) { p.orientation = frame_from_z(am); }); break;
				case 5: fresh([&](Particle &p) { p.orientation = in.actor_rot; }); break;
				default: break;
				}
				Vec inherit{0, 0, 0};
				if (m.inherit_momentum) {
					inherit = am;
					float l = length(inherit);
					if (l > m.inherit_cap) inherit = mul(inherit, m.inherit_cap / l);
					inherit = mul(inherit, m.inherit_scale);
				}
				auto apply = [&](Particle &p, const Vec &v) { p.momentum = m.add_momentum ? add(p.momentum, v) : v; };
				switch (m.momentum_mode) {
				case 0:
					if (m.inherit_momentum) fresh([&](Particle &p) { apply(p, inherit); });
					break;
				case 1: fresh([&](Particle &p) { apply(p, add(Vec{0, kGravity * m.momentum_scale, 0}, inherit)); }); break;
				case 2:
					fresh([&](Particle &p) { apply(p, mul({p.orientation[6], p.orientation[7], p.orientation[8]}, m.momentum_scale)); });
					break;
				case 3:
					fresh([&](Particle &p) {
						apply(p, {center_rand(w, m.momentum_scale), center_rand(w, m.momentum_scale), center_rand(w, m.momentum_scale)});
					});
					break;
				case 4: fresh([&](Particle &p) { apply(p, add(mul(sub(p.pos, in.actor_pos), m.momentum_scale), inherit)); }); break;
				case 5:
					fresh([&](Particle &p) { apply(p, add(mul(normalize(sub(p.pos, in.actor_pos)), m.momentum_scale), inherit)); });
					break;
				default: break;
				}
			} else if (m.kind == "AutoExternalSpawner" && m.auto_spawner) {
				// DE_CAutoExternalSpawner::ManageParticleUserData (0x40001560): chosen new particles
				// have the external manual spawner spawn one where they are.
				GameObject *ext = nearest_copy(in.actor, m.auto_spawner);
				if (ext) {
					de::Transform t = in.actor->world();
					fresh([&](Particle &p) {
						bool every = m.auto_every != 0 && p.serial % m.auto_every == 0;
						bool chance = m.auto_random != 0 && int(random01(w) * float(m.auto_random)) == 0;
						if (!every && !chance) return;
						NewParticleInfo info;
						info.pos = gen.local ? add(rotate(p.pos, t.r), Vec{t.t[0], t.t[1], t.t[2]}) : p.pos;
						info.size = m.auto_size * p.size;
						info.life_span = (p.inv_life != 0 ? 1 / p.inv_life : 0) * m.auto_life;
						control(ext).requests.push_back(info);
					});
				}
			} else if (m.kind == "MiscellaneousNewParticleFunctions" && m.misc_mode <= 2) {
				// DE_CMiscellaneousNewParticleFunctions::ManageParticleUserData (0x40004980): each new
				// particle's orientation turns about its own X / Y / Z by a random angle within
				// +- half the curve at its age, in degrees.
				fresh([&](Particle &p) {
					p.orientation = rotate_local(p.orientation, int(m.misc_mode), center_rand(w, m.curve.eval(p.age)) * 0.017453292f);
				});
			} else if (m.kind == "MomentumAndAcceleration") {
				Rows ar = in.actor->world().r;
				Vec local = inverse_rotate(am, ar);
				Vec base{local[0] * m.inherit_axes[0], local[1] * m.inherit_axes[1], local[2] * m.inherit_axes[2]};
				fresh([&](Particle &p) {
					Vec v = base;
					for (int k = 0; k < 3; ++k) {
						if (m.random_momentum[size_t(k)] != 0) v[size_t(k)] += center_rand(w, m.random_momentum[size_t(k)]);
						v[size_t(k)] += m.initial_momentum[size_t(k)];
					}
					p.momentum = gen.local ? v : rotate(v, ar);
				});
			}
		}
	}

	if (in.created > 0 && in.def->kind == "FourFaceTriangle") {
		// DE_CFourFaceTriangleParticleRenderer::DE_CInstanceInfo::Process (0x400145d0), on a new
		// particle: its corners (jittered, x size, turned by its orientation unless camera-facing)
		// and a planar texture projection at a random offset.
		for (auto &p : in.particles) {
			if (!(p.flags & kNew)) continue;
			float ou = rand_to(w, 1), ov = rand_to(w, 1);
			for (int k = 0; k < 9; ++k) p.tri[size_t(k)] = in.def->tri_corners[size_t(k)] + rand_to(w, in.def->tri_jitter[size_t(k)]);
			if (p.size != 1)
				for (auto &x : p.tri) x *= p.size;
			int a = in.def->tri_uv_plane == 2 ? 1 : 0, b = in.def->tri_uv_plane == 0 ? 1 : 2;
			float sc = in.def->tri_b4[3];
			for (int k = 0; k < 3; ++k) {
				p.tri_uv[size_t(k * 2)] = sc * p.tri[size_t(k * 3 + a)] + ou;
				p.tri_uv[size_t(k * 2 + 1)] = sc * p.tri[size_t(k * 3 + b)] + ov;
			}
			p.tri_uv[6] = ou;
			p.tri_uv[7] = ov;
			if (gen.channels[de::kChOrientation] && in.def->tri_facing == 0) {
				for (int k = 0; k < 3; ++k) {
					Vec v = rotate({p.tri[size_t(k * 3)], p.tri[size_t(k * 3 + 1)], p.tri[size_t(k * 3 + 2)]}, p.orientation);
					for (int j = 0; j < 3; ++j) p.tri[size_t(k * 3 + j)] = v[size_t(j)];
				}
			}
		}
	}
	// 4. Age and move (SetTime).
	int alive = 0;
	for (auto &p : in.particles) {
		if (!(p.flags & kAlive)) continue;
		if (p.flags & kNew) {
			p.pos = add(p.pos, mul(p.momentum, p.spawn_fraction * dt));
			++alive;
			continue;
		}
		p.age_step = p.inv_life * dt;
		p.age += p.age_step;
		if (p.age <= 1) {
			p.pos = add(p.pos, mul(p.momentum, dt));
			++alive;
		} else if (p.flags & kLooping) {
			p.age -= 1;
			++alive;
		} else {
			p.flags &= ~kAlive;
		}
	}
	in.particles.erase(std::remove_if(in.particles.begin(), in.particles.end(), [](const Particle &p) { return !(p.flags & kAlive); }),
	                   in.particles.end());
	// DE_CKillWithParticles::Modify: after its countdown, once no particle is left, the objects it
	// names are deleted (in a copy, the copy's own).
	for (size_t mi = 0; mi < gen.modifiers.size(); ++mi) {
		const auto &m = gen.modifiers[mi];
		if (m.kind != "KillWithParticles") continue;
		float &left = in.per_modifier[mi].kill_countdown;
		if (left != 0) {
			if (left < 999999) left = std::max(0.0f, left - dt);
		} else if (in.particles.empty()) {
			for (auto &k : m.kill) {
				if (GameObject *victim = nearest_copy(in.actor, k)) kills_.push_back(victim);
			}
			left = 999999;
		}
	}
	if (alive == 0) return;

	// 5. Modify.
	for (const auto &m : gen.modifiers) {
		if (m.kind == "Gravity") {
			for (auto &p : in.particles) p.momentum[1] -= dt * m.strength * 9.6e-6f;
		} else if (m.kind == "Wind") {
			// DE_CWind::Modify: momentum eases towards the scene's wind (none) over `strength` s.
			float f = m.strength != 0 ? dt / m.strength / 1000 : 0;
			for (auto &p : in.particles) p.momentum = sub(p.momentum, mul(p.momentum, f));
		} else if (m.kind == "MiscellaneousParticleFunctions") {
			for (auto &p : in.particles) {
				float v = m.curve.eval(p.age);
				if (m.misc_mode == 0) {
					p.size = v;
				} else if (m.misc_mode == 1) {
					p.rotation = v;
				}
			}
		} else if (m.kind == "MomentumAndAcceleration" && m.accel_mode != 0) {
			const Vec &c = in.actor_pos;
			for (auto &p : in.particles) {
				float remaining = 1 - p.age;
				float life_ms = p.inv_life > 0 ? 1 / p.inv_life : 0;
				switch (m.accel_mode) {
				case 1: {  // back towards where it was born (relative to the actor)
					Vec target = add(mul(sub(p.prev, c), remaining), c);
					p.momentum = add(p.momentum, mul(sub(target, p.pos), dt / 1e6f));
					break;
				}
				case 2:
				case 4: {
					if (remaining <= 0 || life_ms <= 0) break;
					Vec target = add(mul(sub(m.accel_mode == 2 ? p.prev : p.pos, c), remaining), mul(p.momentum, remaining * life_ms));
					if (m.accel_mode == 2) {
						target = add(target, c);
						p.momentum = add(p.momentum, mul(sub(target, p.pos), dt / (remaining * life_ms)));
					} else {
						p.momentum = sub(p.momentum, mul(target, dt / 1000 / (remaining * life_ms) * m.accel));
					}
					break;
				}
				case 3: {  // swirl about the actor's Z axis
					Vec v = inverse_rotate(sub(p.pos, c), in.actor_rot);
					float a = m.accel_angle * 0.017453292f + 1.5707964f;
					float cs = std::cos(a), sn = std::sin(a);
					v = {v[0] * cs - v[1] * sn, v[0] * sn + v[1] * cs, v[2]};
					p.momentum = add(p.momentum, mul(rotate(v, in.actor_rot), dt / 1e6f * m.accel));
					break;
				}
				case 5: p.momentum[1] -= dt * m.accel * 9.6e-6f; break;
				case 6: {
					Vec d = mul(sub(p.pos, c), dt * m.accel / 1000);
					p.momentum = sub(p.momentum, d);
					break;
				}
				case 7: {
					float k = dt * m.accel / 1000;
					p.momentum[0] -= (p.pos[0] - c[0]) * k;
					p.momentum[2] -= (p.pos[2] - c[2]) * k;
					break;
				}
				default: break;
				}
			}
		} else if (m.kind == "Rotator") {
			// DE_CRotator::Modify (0x40009150), on the rotation channel.
			for (auto &p : in.particles) {
				float v = m.curve.eval(p.age);
				if (m.rotator_mode == 1) p.rotation += v * m.rotator_factor * dt;
				else if (m.rotator_mode == 3) p.rotation += p.size * m.rotator_factor * v * dt;
				else if (m.rotator_mode == 4) p.rotation = v + m.rotator_factor;
			}
		} else if (m.kind == "OriginMagnet") {
			// DE_CParticleOriginMagnet::Modify (0x40008210): a point that moves from the particle's
			// spawn position to the actor as it ages; the particle is drawn towards it by the life left.
			const Vec &c = in.actor_pos;
			for (auto &p : in.particles) {
				float left = 1 - p.age;
				Vec target = add(c, mul(sub(p.prev, c), left));
				p.pos = add(target, mul(sub(p.pos, target), left));
			}
		} else if (m.kind == "PushDownZModifier") {
			// DE_CPushDownZModifier::Modify (0x400088d0): along the actor's X, Y or Z by the amount
			// x the step's share of the particle's life (modes 3-5 also x the life left).
			if (m.push_mode >= 0 && m.push_mode <= 5) {
				int axis = m.push_mode % 3;
				Vec ax{in.actor_rot[size_t(axis * 3)], in.actor_rot[size_t(axis * 3 + 1)], in.actor_rot[size_t(axis * 3 + 2)]};
				for (auto &p : in.particles) {
					float k = p.age_step * m.push_amount * (m.push_mode >= 3 ? 1 - p.age : 1);
					p.pos = add(p.pos, mul(ax, k));
				}
			}
		} else if (m.kind == "RandomBoxSpawner" && (m.wrap[0] || m.wrap[1] || m.wrap[2])) {
			// DE_CRandomBoxSpawner::Modify: particles leaving the box re-enter on the other side.
			for (auto &p : in.particles) {
				for (int k = 0; k < 3; ++k) {
					if (!m.wrap[size_t(k)]) continue;
					float lo = m.box[size_t(k)] + in.actor_pos[size_t(k)], hi = m.box[size_t(k + 3)] + in.actor_pos[size_t(k)];
					float span = hi - lo;
					if (span <= 0) continue;
					float &x = p.pos[size_t(k)];
					while (x < lo) x += span;
					while (x > hi) x -= span;
				}
			}
		}
	}
}

// DE_CAttackTrailer::RampToward: reach `c` over n points (n = 0: at once).
void AttackTrailer::ramp_toward(const std::array<float, 4> &c, int n) {
	if (n != 0) {
		for (int k = 0; k < 4; ++k) step[size_t(k)] = (c[size_t(k)] - color[size_t(k)]) / float(n);
	} else {
		step = {0, 0, 0, 1};
		color = c;
	}
	steps = n;
	target = c;
	dest = c;
	dest_steps = n;
}

void AttackTrailer::generate_point(uint32_t now) {
	bool add_point = false;
	if (!continuing) {
		if (target[0] > 0 || target[1] > 0 || target[2] > 0) {
			// InternalRampToward(black, 8): fade out without touching the destination colour.
			std::array<float, 4> black{0, 0, 0, 1};
			for (int k = 0; k < 4; ++k) step[size_t(k)] = (black[size_t(k)] - color[size_t(k)]) / 8;
			steps = 8;
			target = black;
		}
		add_point = color[0] > 0.01f || color[1] > 0.01f || color[2] > 0.01f;
	} else {
		continuing = false;
		add_point = true;
		if (dest != target) {
			if (dest_steps == 0) {
				step = {0, 0, 0, 0};
				steps = 0;
				color = dest;
			} else {
				for (int k = 0; k < 4; ++k) step[size_t(k)] = (dest[size_t(k)] - color[size_t(k)]) / float(dest_steps);
				steps = dest_steps;
			}
			target = dest;
		}
	}
	if (steps != 0) {
		for (int k = 0; k < 4; ++k) color[size_t(k)] += step[size_t(k)];
		--steps;
	}
	if (!add_point || !emitter) return;
	de::Transform t = emitter->world();
	Point p;
	p.pos = t.apply(offset);
	p.width = t.rotate(width);
	p.color = color;
	p.life = 1;
	p.decay = length != 0 ? 1 / length : 1;
	p.born = now;
	p.cut = !(!points.empty() && now - points.back().born < 2);
	points.push_back(p);
}

namespace {

// The fighter's own copy of the actor a Value\ObjectReference child names.
GameObject *emitter_of(World &w, GameObject *fighter, const std::string &value) {
	GameObject *v = nullptr;
	for (auto *ch : fighter->children) {
		if (ch->name == value && ch->is("Value\\ObjectReference")) v = ch;
	}
	if (!v || !v->file) return nullptr;
	std::function<GameObject *(GameObject *, const de::ObjectRef &)> find = [&](GameObject *o, const de::ObjectRef &r) -> GameObject * {
		if (o->file == r.file && o->index == r.index) return o;
		for (auto *c : o->children) {
			if (GameObject *hit = find(c, r)) return hit;
		}
		return nullptr;
	};
	for (auto &lists : reference_lists(w.data(), {v->file, v->index})) {
		for (auto &l : lists) {
			for (auto &r : l) {
				if (!r) continue;
				if (GameObject *o = find(fighter, r)) return o;
				return w.global_object(r);
			}
		}
	}
	return nullptr;
}

}  // namespace

AttackTrailer *ParticleSystem::trailer(GameObject *fighter, int which) {
	if (!fighter || which < 0 || which > 3) return nullptr;
	auto [it, made] = trailers_.try_emplace(fighter);
	auto &set = it->second;
	if (made) {
		// DE_COMF2Fighter::ObjPrepare: Setup(fighter, emitter, offset, width, image, 30, 1).
		static const char *emitters[4] = {"LeftPunchTrailerEmitter", "RightPunchTrailerEmitter", "LeftKickTrailerEmitter",
		                                  "RightKickTrailerEmitter"};
		static const int vectors[4] = {6, 0, 18, 12};  // +0x558, +0x540, +0x588, +0x570
		static const char *names[4] = {"LeftHandTrailer", "RightHandTrailer", "LeftFootTrailer", "RightFootTrailer"};
		de::ObjectRef image;
		if (fighter->file) {
			for (auto &r : w_.actor_properties({fighter->file, fighter->index})) {
				if (r && r.info().name == "AttackTrailer") image = r;
			}
		}
		for (int i = 0; i < 4; ++i) {
			AttackTrailer &t = set[size_t(i)];
			t.object = w_.create_child(nullptr, names[i], "Mesh\\AttackTrailer");
			trailer_objects_[t.object] = &t;
			t.image = image;
			const auto &v = fighter->stats.trailer_vectors;
			for (int k = 0; k < 3; ++k) {
				t.offset[size_t(k)] = v[size_t(vectors[i] + k)];
				t.width[size_t(k)] = v[size_t(vectors[i] + 3 + k)];
			}
			t.emitter = emitter_of(w_, fighter, emitters[i]);
		}
	}
	return &set[size_t(which)];
}

AttackTrailer *ParticleSystem::trailer_of(const GameObject *object) {
	auto it = trailer_objects_.find(object);
	return it == trailer_objects_.end() ? nullptr : it->second;
}

void ParticleSystem::tick_trailers(GameObject &fighter) {
	auto it = trailers_.find(&fighter);
	if (it == trailers_.end()) return;
	for (auto &t : it->second) t.generate_point(fighter.creature_ticks);
}

void ParticleSystem::process_trailers() {
	for (auto &[fighter, set] : trailers_) {
		const uint32_t now = fighter->creature_ticks;
		for (auto &t : set) {
			if (t.points.size() >= 2) {
				float dt = float(now - t.last);
				for (auto &p : t.points) {
					p.v = 1 - p.life;
					p.life -= dt * p.decay;
				}
			}
			t.last = now;
			size_t dead = 0;
			while (dead < t.points.size() && t.points[dead].life < 0) ++dead;
			t.points.erase(t.points.begin(), t.points.begin() + long(dead));
		}
	}
}

void register_particle_natives(World &w) {
	auto reg = [&](const std::string &key, Native fn) { w.register_native(key, std::move(fn)); };
	auto queue = [](const NativeContext &c) -> std::vector<NewParticleInfo> * {
		GameObject *o = c.self();
		return o ? &c.world.particles->control(o).requests : nullptr;
	};

	// --- CManualParticleSpawner::Spawn* (CNewParticleInfo requests) ------------------------------
	reg("CManualParticleSpawner::Spawn(float,float)", [queue](const NativeContext &c) {
		if (auto *q = queue(c)) {
			NewParticleInfo &i = q->emplace_back();
			i.size = c.arg<float>(0);
			i.life_span = c.arg<float>(1);
			i.relative = true;
		}
	});
	reg("CManualParticleSpawner::Spawn(class CVector,float,float)", [queue](const NativeContext &c) {
		if (auto *q = queue(c)) {
			NewParticleInfo &i = q->emplace_back();
			i.pos = read_vec(c.mem(), c.arg<uint32_t>(0));
			i.size = c.arg<float>(1);
			i.life_span = c.arg<float>(2);
		}
	});
	reg("CManualParticleSpawner::Spawn(class CVector,class CCoordinateSystem,float,float)", [queue](const NativeContext &c) {
		if (auto *q = queue(c)) {
			NewParticleInfo &i = q->emplace_back();
			i.pos = read_vec(c.mem(), c.arg<uint32_t>(0));
			if (uint32_t cs = c.arg<uint32_t>(1)) {
				for (int k = 0; k < 9; ++k) i.orientation[size_t(k)] = c.mem().get<float>(cs + uint32_t(k) * 4);
			}
			i.size = c.arg<float>(2);
			i.life_span = c.arg<float>(3);
		}
	});
	reg("CManualParticleSpawner::Spawn(class CActor,float,float)", [queue](const NativeContext &c) {
		auto *q = queue(c);
		GameObject *a = c.arg_object(0);
		if (!q || !a) return;
		de::Transform t = a->world();
		NewParticleInfo &i = q->emplace_back();
		i.pos = {t.t[0], t.t[1], t.t[2]};
		i.orientation = t.r;
		i.size = c.arg<float>(1);
		i.life_span = c.arg<float>(2);
		i.time = scene_time_ms(c.world);
	});
	reg("CManualParticleSpawner::SetLastIntensity(float)", [queue](const NativeContext &c) {
		if (auto *q = queue(c); q && !q->empty()) q->back().intensity = c.arg<float>(0);
	});
	reg("CManualParticleSpawner::SetLastRotation(float)", [queue](const NativeContext &c) {
		if (auto *q = queue(c); q && !q->empty()) q->back().rotation = c.arg<float>(0);
	});

	// --- CParticleModifier ---------------------------------------------------------------------------
	auto ctl = [](const NativeContext &c) -> ModifierControl * {
		GameObject *o = c.self();
		if (!o) return nullptr;
		ModifierControl &m = c.world.particles->control(o);
		if (!m.initialised && o->file) {
			// Not drawn yet: start from the data so later instances pick the change up.
			de::ParticleModifierDef d;
			for (auto &in : c.world.particles->instances()) {
				for (auto &md : in->def->generator.modifiers) {
					if (md.object.file == o->file && md.object.index == o->index) d = md;
				}
			}
			if (d.object) {
				m.initialised = true;
				m.spawn_time = d.spawn_time;
				m.spawn_time_variation = d.spawn_time_variation;
				m.life_span = d.life_span;
				m.life_span_variation = d.life_span_variation;
				m.spawn_size = d.spawn_size;
				m.spawn_size_variation = d.spawn_size_variation;
				m.spawning = d.spawning;
			}
		}
		return &m;
	};
	// AssureInstanceCaller (Core 0x701d51e0) -> DE_CParticleGenerator::NewInstance(actor, time): the
	// generator's instance for that actor (made if there is none yet). Its SpawningOff/On
	// (InstanceSpawningOffCaller 0x701d5250: +0x58) switch that instance's spawning.
	static std::map<uint32_t, std::pair<de::ObjectRef, GameObject *>> g_generator_instances;
	reg("CParticleGenerator::class CInstance AssureInstance(class CActor,float)", [](const NativeContext &c) {
		GameObject *g = c.self(), *a = c.arg_object(0);
		if (!g || !g->file || !a) {
			c.ret_u32(0);
			return;
		}
		de::ObjectRef ref{g->file, g->index};
		for (auto &[h, v] : g_generator_instances) {
			if (v.first.file == ref.file && v.first.index == ref.index && v.second == a) {
				c.ret_u32(h);
				return;
			}
		}
		uint32_t h = c.mem().alloc(8);
		g_generator_instances[h] = {ref, a};
		c.ret_u32(h);
	});
	for (auto [sig, on] : {std::pair{"CParticleGenerator::CInstance::SpawningOff()", false}, std::pair{"CParticleGenerator::CInstance::SpawningOn()", true}}) {
		reg(sig, [on = on](const NativeContext &c) {
			auto it = g_generator_instances.find(c.self_handle());
			if (it != g_generator_instances.end()) c.world.particles->set_generator_spawning(it->second.first, it->second.second, on);
		});
	}
	reg("CParticleModifier::SpawningOff()", [ctl](const NativeContext &c) {
		if (auto *m = ctl(c)) m->spawning = false;
	});
	reg("CParticleModifier::SpawningOn()", [ctl](const NativeContext &c) {
		if (auto *m = ctl(c)) m->spawning = true;
	});
	// SetRadiusCaller (Particles 0x4000e420): every radius becomes the value and the box is
	// rebuilt around the centre (+0x78).
	// SetColorCaller (Particles 0x4000ff00): sets the RGB of the renderer's colour value (+0x58).
	reg("CBillboardedQuadParticleRenderer::SetColor(float,float,float)", [](const NativeContext &c) {
		GameObject *r = c.self();
		const de::ParticleRendererDef *d = r && r->file ? c.world.particles->renderer({r->file, r->index}) : nullptr;
		if (d) c.world.particles->set_value_color(d->color_ref, {c.arg<float>(0), c.arg<float>(1), c.arg<float>(2), -1});
	});
	reg("CValueRGBA::Set(float,float,float,float)", [](const NativeContext &c) {
		if (GameObject *v = c.self(); v && v->file) c.world.particles->set_value_color({v->file, v->index}, {c.arg<float>(0), c.arg<float>(1), c.arg<float>(2), c.arg<float>(3)});
	});
	reg("CRandomEllipsoidSpawner::SetRadius(float)", [ctl](const NativeContext &c) {
		if (auto *m = ctl(c)) m->radius = c.arg<float>(0);
	});
	// DE_CPointStreamer::Enable / Disable (Core 0x701d2500): the trail stops or starts adding points.
	reg("CPointStreamer::Enable()", [](const NativeContext &c) { if (GameObject *o = c.self()) c.world.particles->set_trail_enabled(o, true); });
	reg("CPointStreamer::Disable()", [](const NativeContext &c) { if (GameObject *o = c.self()) c.world.particles->set_trail_enabled(o, false); });
	reg("CBeamSpawner::SetTotalLength(float)", [ctl](const NativeContext &c) {
		if (auto *m = ctl(c)) m->beam_length = c.arg<float>(0);
	});
	reg("CBeamSpawner::SetInitialSpawnLength(float)", [ctl](const NativeContext &c) {
		if (auto *m = ctl(c)) m->beam_start = c.arg<float>(0);
	});
	reg("CParticleModifier::Spawn(int)", [ctl](const NativeContext &c) {
		if (auto *m = ctl(c)) m->burst += c.arg<int32_t>(0);
	});
	struct Field {
		const char *name;
		float ModifierControl::*member;
	};
	for (Field f : {Field{"SpawnTime", &ModifierControl::spawn_time}, Field{"SpawnTimeVariation", &ModifierControl::spawn_time_variation},
	                Field{"LifeSpan", &ModifierControl::life_span}, Field{"LifeSpanVariation", &ModifierControl::life_span_variation},
	                Field{"SpawnSize", &ModifierControl::spawn_size}, Field{"SpawnSizeVariation", &ModifierControl::spawn_size_variation}}) {
		auto member = f.member;
		reg(std::string("CParticleModifier::Set") + f.name + "(float)", [ctl, member](const NativeContext &c) {
			if (auto *m = ctl(c)) m->*member = c.arg<float>(0);
		});
		reg(std::string("CParticleModifier::float ") + f.name + "()", [ctl, member](const NativeContext &c) {
			auto *m = ctl(c);
			c.ret_f32(m ? m->*member : 0);
		});
	}

	// --- Attack trailers -----------------------------------------------------------------------------
	const char *getters[4] = {"LeftHandTrailer", "RightHandTrailer", "LeftFootTrailer", "RightFootTrailer"};
	for (int i = 0; i < 4; ++i) {
		reg(std::string("COMF2Fighter::class CAttackTrailer ") + getters[i] + "()", [i](const NativeContext &c) {
			AttackTrailer *t = c.world.particles->trailer(c.self(), i);
			c.ret_u32(t && t->object ? t->object->handle : 0);
		});
	}
	auto trailer = [](const NativeContext &c) { return c.world.particles->trailer_of(c.self()); };
	reg("CAttackTrailer::Continue()", [trailer](const NativeContext &c) {
		if (auto *t = trailer(c)) t->continuing = true;
	});
	// Setup*Attack: ramp to (0.1, 0.1, 0.3) over 8 points; the points live 20/30/50 ticks.
	for (auto [name, length] : {std::pair{"SetupLightAttack", 20.0f}, std::pair{"SetupMediumAttack", 30.0f},
	                            std::pair{"SetupHardAttack", 50.0f}, std::pair{"SetupHeavyAttack", 50.0f}}) {
		float len = length;
		reg(std::string("CAttackTrailer::") + name + "()", [trailer, len](const NativeContext &c) {
			if (auto *t = trailer(c)) {
				t->ramp_toward({0.1f, 0.1f, 0.3f, t->color[3]}, 8);
				t->length = len;
			}
		});
	}
	reg("CAttackTrailer::SetColor(float,float,float)", [trailer](const NativeContext &c) {
		if (auto *t = trailer(c)) t->ramp_toward({c.arg<float>(0), c.arg<float>(1), c.arg<float>(2), t->color[3]}, 1);
	});
	reg("CAttackTrailer::SetDestColor(float,float,float)", [trailer](const NativeContext &c) {
		if (auto *t = trailer(c)) t->ramp_toward({c.arg<float>(0), c.arg<float>(1), c.arg<float>(2), t->color[3]}, t->steps);
	});

	// --- Lights: the renderer takes their colour and intensity from here when set ---------------
	reg("CLight::SetIntensity(float)", [](const NativeContext &c) {
		if (GameObject *o = c.self()) c.world.particles->light(o).intensity = c.arg<float>(0);
	});
	reg("CLight::SetColor(float,float,float)", [](const NativeContext &c) {
		if (GameObject *o = c.self()) c.world.particles->light(o).color = std::array<float, 3>{c.arg<float>(0), c.arg<float>(1), c.arg<float>(2)};
	});

	// --- CActor::Enable/DisableParticleSpawning: the generator instances the actor draws -------
	reg("CActor::DisableParticleSpawning()", [](const NativeContext &c) {
		if (GameObject *o = c.self()) c.world.particles->set_spawning(o, false);
	});
	reg("CActor::EnableParticleSpawning()", [](const NativeContext &c) {
		if (GameObject *o = c.self()) c.world.particles->set_spawning(o, true);
	});
}

}  // namespace game
