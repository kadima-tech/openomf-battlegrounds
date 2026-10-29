#include "scene_lights.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <tuple>

#include "de/lighting.h"
#include "game/forces.h"
#include "game/particles.h"
#include "render.h"

namespace render {

FrameLights collect_lights(game::World &world, int fr, const std::function<bool(const game::GameObject *)> &in_scene_layer) {
	FrameLights out;
	auto &render_affectors = out.render_affectors;
	for (game::GameObject *a : world.affectors()) {
		if (!(a->groups & 0x20) || !game::in_scene(*a)) continue;
		if (in_scene_layer && !in_scene_layer(a)) continue;
		game::GameObject *ow = world.affector_owner(a);
		if (!ow || ow == a) continue;
		render_affectors.emplace_back(a, ow);
		out.owners.insert(ow);
	}
	{
		static std::map<std::pair<const de::ObjectsFile *, uint32_t>, std::optional<de::LightInfo>> infos;
		static std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> light_images;
		g_lighting.lights.clear();
		// An affector's lights: its Light children (the arenas) and the Lights among its
		// properties (the GUI's MainLight, whose Light is a component reference).
		std::vector<std::tuple<de::ObjectRef, game::GameObject *, game::GameObject *>> placed;  // light, its object, affector
		for (game::GameObject *o : world.scene_objects()) {
			if (in_scene_layer && !in_scene_layer(o)) continue;
			if (o->class_path == "Light" && o->file && game::in_scene(*o) && o->parent) placed.emplace_back(de::ObjectRef{o->file, o->index}, o, o->parent);
			if ((o->shape == game::GameObject::Shape::Sphere || o->shape == game::GameObject::Shape::Cone) && o->file && game::in_scene(*o)) {
				for (const de::ObjectRef &p : world.properties_of(*o)) {
					if (p && p.info().class_path == "Light") placed.emplace_back(p, nullptr, o);
				}
			}
		}
		for (auto &[lref, o, aff] : placed) {
			if (aff->shape != game::GameObject::Shape::Sphere && aff->shape != game::GameObject::Shape::Cone) continue;
			auto key = std::make_pair(lref.file, lref.index);
			auto it = infos.find(key);
			if (it == infos.end()) it = infos.emplace(key, de::read_light(world.data(), lref)).first;
			if (!it->second) continue;
			const de::LightInfo &info = *it->second;
			SceneLight l;
			l.frame = aff->world();
			l.position_static = game::position_static(*aff);
			l.pos = l.frame.t;
			if (aff->shape == game::GameObject::Shape::Sphere) {
				l.shape = SceneLight::kSphere;
				l.inner = aff->hotspot, l.outer = aff->falloff;
			} else {
				l.shape = SceneLight::kCone;
				l.inner = aff->far_begin, l.outer = aff->far_end, l.umbra = aff->umbra, l.penumbra = aff->penumbra;
				l.near_begin = aff->near_begin;
			}
			auto col = info.colour.eval(world.scene_ms() / 1000.0f);
			l.colour = {col[0], col[1], col[2]};
			l.intensity = info.intensity;
			l.mask = info.mask();
			// DE_CStandardRenderLight::UpdateLitActors: its affector against the render affectors.
			l.reaches_all = false;
			// A static light skips a render affector that is position-static too: the affector's
			// own IsPositionStatic (SetAffector +0x10c), not its meshes'.
			for (auto &[ra, ow] : render_affectors) {
				if (l.position_static && game::position_static(*ra)) continue;
				if (game::touch(world, *ra, *aff) && std::find(l.reaches.begin(), l.reaches.end(), ow) == l.reaches.end()) l.reaches.push_back(ow);
			}
			l.has_image = bool(info.image);
			if (info.image && info.image.info().class_path.rfind("Image", 0) == 0 && l.shape == SceneLight::kCone) {
				l.image = texture_of(light_images, info.image);
				l.blend = de::projector_blend(info);
			}
			if (const auto *st = o ? world.particles->find_light(o) : nullptr) {
				if (st->intensity) l.intensity = *st->intensity;
				if (st->color) l.colour = *st->color;
			}
			g_lighting.lights.push_back(l);
		}
		static const bool dbg = getenv("OPENBG_DEBUG_LIGHTS") != nullptr;
		if (dbg && fr % 60 == 0) {
			for (auto &[ra, ow] : render_affectors) fprintf(stderr, "  render affector %s owned by %s\n", ra->name.c_str(), ow->name.c_str());
			fprintf(stderr, "[lights %d] ambient %.2f %.2f %.2f, %zu lights\n", fr, g_lighting.ambient[0], g_lighting.ambient[1],
			        g_lighting.ambient[2], g_lighting.lights.size());
			for (auto &l : g_lighting.lights) {
				fprintf(stderr, "  %s at %.0f %.0f %.0f r %.0f..%.0f colour %.2f %.2f %.2f x %.2f mask %x%s blend %d, reaches", l.shape ? "cone" : "sphere", l.pos[0], l.pos[1],
				        l.pos[2], l.inner, l.outer, l.colour[0], l.colour[1], l.colour[2], l.intensity, l.mask, l.image ? " own image" : "", l.blend);
				if (l.position_static) fprintf(stderr, " static");
				for (const void *ow : l.reaches) fprintf(stderr, " %s", static_cast<const game::GameObject *>(ow)->name.c_str());
				fprintf(stderr, "\n");
			}
		}
	}
	return out;
}

}  // namespace render
