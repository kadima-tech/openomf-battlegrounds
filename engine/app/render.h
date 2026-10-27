// Shared renderers for the native apps: arenas (static scene batches) and skinned fighters.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "de/anim.h"
#include "de/scene.h"
#include "gl_util.h"

namespace render {

// A drawn vertex: world position, normal, the four stages' texture coordinates and the pass's
// vertex colour (its first stage's colour channel; white without one).
struct BatchVertex {
	float pos[3];
	float normal[3];
	float uv[4][2];
	float colour[3];
};

// One pass of a TextureProcess\MultiStage material over the faces its first stage covers: up to
// four stacked texture stages combined in the shader (DE_CShader::CStage::Setup), blended into the
// frame by the pass's element mode (DE_CGenericShader::AddStage).
struct MeshBatch {
	GLuint vao = 0, vbo = 0;
	GLsizei count = 0;
	int stages = 0;
	GLuint tex[4] = {0, 0, 0, 0};
	int mode[4] = {0, 0, 0, 0};       // stage blend modes (0 Replace, 1 Add, 2 Modulate, 3 AddToLightMap, 4 SemiTranslucent)
	bool environment[4] = {false, false, false, false};
	bool alpha[4] = {false, false, false, false};  // the stage's image has an alpha channel (image flag 8)
	float colour[4][4] = {};
	bool stand_in_light = false;      // faces in no material: an untextured stand-in, shaded by a fixed light
	bool overlay = false;             // drawn over the material's earlier passes (no depth writes)
	bool late = false;                // the material starts with a blended pass: drawn after the opaque ones
	// The mesh's lighting (DE_CMesh +0x40, DE_CRendererDirect3D::CreateNewMesh): 0 unlit, 1 D3D
	// vertex lighting, 2 lit by light projector passes.
	int lighting = 0;
	// Its actor is position-static (DE_CStandardRenderAffector +0x10c): static lights skip it.
	bool position_static = false;
	// The mesh is two-sided (DE_CMesh +0x40 flag 0x4000 -> shader flag 0x10): drawn without
	// culling; every other pass culls D3DCULL_CW (DE_CGenericShader::ActivateAndDrawPatches).
	bool two_sided = false;
	// The owner of the render affector that decides which lights reach it (see SceneLight::reaches).
	const void *light_owner = nullptr;
	// Drawn again into the glow layer: the mesh's GlowFactor (with the glow fog) or the
	// SpecularFactor of the pass's last stage (without it).
	float glow = 0, specular = 0;
	// The environment stage's own cube and its specular cube (0: the arena's).
	GLuint cube = 0, specular_cube = 0;
	// A stage's bump image: the reflection normal + its RG x factor (in world X/Y), at that stage's
	// coordinates (the Direct3D module's bump reflection pixel shader).
	GLuint bump = 0;
	int bump_stage = 0;
	float bump_factor = 0;
	float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
	// Kept for meshes re-posed every frame (fighters): the vertices and their mesh vertex.
	std::vector<BatchVertex> vertices;
	std::vector<uint32_t> source;
};

// A scene light where it is this frame: its colour and intensity (DE_CLight +0x38, +0x4c, or what
// the scripts set) and its affector's shape (DE_CStandardRenderAffector::Update, Direct3D
// 0x20006570): a sphere (type 7) is full inside `inner` and fades linearly to `outer`; a cone (type
// 1) fades with distance from far-begin (`inner`) to far-end (`outer`) along its local +Z and
// between the umbra and penumbra half-angles (degrees).
struct SceneLight {
	enum Shape { kSphere, kCone } shape = kSphere;
	std::array<float, 3> pos{0, 0, 0};
	de::Transform frame;  // the affector's world placement (the cone's axes)
	float inner = 0, outer = 0, umbra = 0, penumbra = 0;
	float near_begin = 0;  // a cone's near-attenuation begin (+0x184): where its projected fade starts
	std::array<float, 3> colour{1, 1, 1};
	float intensity = 1;
	uint32_t mask = 6;  // the actors it reaches (de::LightInfo::mask)
	// Its affector is position-static: DE_CStandardRenderLight::UpdateLitActors (Direct3D
	// 0x2000f720) then lights only what is not (static geometry carries its own lighting).
	bool position_static = false;
	// DE_CStandardRenderAffector::Intensity (Direct3D 0x20006fa0).
	float affector_intensity(const std::array<float, 3> &p) const;
	// DE_CStandardRenderLight::UpdateLitActors: the owners of the lightable render affectors
	// (collision group 0x20) its affector touches; only what they own is lit. `reaches_all` (the
	// arena viewer, which runs no scene) lights everything in reach.
	bool reaches_all = true;
	std::vector<const void *> reaches;
	bool lights(const void *owner) const;
	// Its own projector image (Light +0x30, a cone's: DE_CD3DLight::SetupComplete) and blend
	// (de::projector_blend); 0 = the shared LightMap projector.
	GLuint image = 0;
	int blend = 0;
	bool has_image = false;  // a sphere light with one gets no projector pass
};
struct Lighting {
	std::array<float, 3> ambient{0.3f, 0.3f, 0.3f};  // the environment's ambient (de::read_ambient)
	std::vector<SceneLight> lights;
};
// The frame's lights; the app fills it before drawing.
extern Lighting g_lighting;

GLuint texture_of(std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> &cache, const de::ObjectRef &r);
// Image\AVI textures show the frame for their image's phase (CImage::SetPhase); call once a frame.
void update_animated_textures(const std::map<std::pair<const de::ObjectsFile *, uint32_t>, float> &phases);

// Arena: every mesh drawn by the arena's actors, base x lightmap per material.
class ArenaRenderer {
public:
	void load(const de::World &world, const std::string &stem);
	void draw(const float *vp, const float *eye);
	// Its glow-layer passes (into the bound glow target).
	void draw_glow(const float *vp, const float *eye);
	// Each batch's lighting owner, from its actor and that actor's ancestors (scene file indices).
	void set_light_owners(const std::function<const void *(const std::vector<uint32_t> &chain)> &owner_of);
	// Leave out the meshes of these actors (scene file indices) and of everything under them:
	// actors a path moves at run time, drawn where they are by ActorMeshRenderer instead.
	void set_moving(const std::set<uint32_t> &actors);
	// Highest point of the fighting-surface collision mesh at (x, z) at or below `from_y`
	// (falls back to every drawn mesh); returns -1e30 when nothing is below.
	float floor_height(float x, float z, float from_y = 1e30f) const;
	// Does the segment a-b cross the fighting-surface collision mesh?
	bool segment_hits(const std::array<float, 3> &a, const std::array<float, 3> &b) const;
	// Where it first crosses it (nearest to a), if it does.
	std::optional<std::array<float, 3>> segment_point(const std::array<float, 3> &a, const std::array<float, 3> &b) const;

	float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
	size_t triangles = 0;

private:
	GLuint texture(const de::ObjectRef &r);

	GLuint prog_ = 0;
	std::vector<MeshBatch> batches_;
	std::vector<std::vector<uint32_t>> chains_;  // per batch: its actor and that actor's ancestors
	std::set<uint32_t> moving_;
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> textures_;
	std::vector<std::array<std::array<float, 3>, 3>> floor_tris_;
};

// Meshes of actors made at run time, drawn where they are each frame.
class ActorMeshRenderer {
public:
	struct Item {
		de::ObjectRef actor;
		de::Transform world;
		const void *light_owner = nullptr;  // see SceneLight::reaches
	};
	void draw(const de::World &data, const std::vector<Item> &actors, const float *vp, const float *eye);
	// The pilot colours an image is painted with when a script gave it a fighter's paint filter
	// (CImage::AddFilter); none for an unfiltered image. Asked once per image.
	std::function<std::optional<std::array<std::array<float, 3>, 3>>(const de::ObjectRef &image)> paint_of;
	// A texture manager's image as scripts set it (World::texture_overrides), when they did; the
	// meshes are rebuilt whenever `texture_version` changes.
	std::function<std::optional<de::ObjectRef>(const de::ObjectRef &manager)> image_of;
	uint32_t texture_version = 0;

private:
	GLuint prog_ = 0;
	uint32_t built_version_ = 0;
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, std::vector<MeshBatch>> meshes_;
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> textures_;
};

// A fighter's mesh, skin and rig (from <Name>Model1.DEObjects and <Name>.DEObjects), drawn with
// its mesh's own materials. The paint is the fighter's ImageFilter\OMF2Fighter applied to the
// model's "RGBMain" and "ReflectionMask" images with the pilot's three colours
// (DE_COMF2FighterImageFilter::Filter, OMF 0x350350e0).
class FighterRenderer {
public:
	using Paint = std::array<std::array<float, 3>, 3>;
	void load(const de::World &world, const de::ObjectsFile &model_file, const de::ObjectsFile *fighter_file);
	// The pilot's colours (DE_CPilot::UserDefinedColor 0..2); repaints when they change.
	void set_paint(const Paint &p);
	// Pose `sequence` (index into bank, -1 = bind pose) at normalised time t, placed by `world`.
	void draw(const float *vp, const float *eye, const de::Transform &world, int sequence, float t);
	// As above, for a creature's animation state: during a prelude, t blends from the pose shown
	// when the prelude was activated to `sequence` at `prelude_time`.
	struct AnimState {
		int sequence = -1;
		float t = 0;
		bool prelude = false;
		float prelude_time = 0;
		uint32_t activations = 0;
		// AnimActivateEmpty: blend from (initial_seq, initial_t) to (final_seq, final_t) by t.
		bool captured = false;
		int initial_seq = -1, final_seq = -1;
		float initial_t = 0, final_t = 0;
		// A script-placed Pelvis (throw follow) in the drawn body's space: the whole skeleton
		// moves with it.
		const de::Transform *pelvis = nullptr;
		// The game's own pose (World::current_pose): drawn as is when given.
		const std::vector<de::Transform> *pose = nullptr;
		// The fighter's lighting owner (SceneLight::reaches).
		const void *light_owner = nullptr;
		// Each attachment's AnimationSequenceManager state (attachment_roots() order).
		struct Attachment {
			int sequence = -1;
			float t = 0;
			bool prelude = false;
			float prelude_time = 0;
			uint32_t activations = 0;
		};
		std::vector<Attachment> attachments;
	};
	void draw(const float *vp, const float *eye, const de::Transform &world, const AnimState &a);
	// Its glow-layer passes in the pose last drawn.
	void draw_glow(const float *vp, const float *eye);
	// Second skinned meshes posed by their own bank under one of the body's bones (Gargoyle's
	// wings: "DE_Wings Mesh" with WingDeformInfo, animated by WingAnimationBank, under Chest via
	// WingBase). Per attachment, the actor right under that bone, whose child
	// AnimationSequenceManager drives it.
	std::vector<std::string> attachment_roots() const;

	de::AnimBank bank;
	bool rigged = false;
	float radius = 1;

private:
	de::Mesh mesh_;
	std::vector<de::Material> materials_;
	std::vector<MeshBatch> batches_;
	std::vector<de::SkinBone> bones_;
	std::vector<int> bone_channel_;
	std::vector<de::Transform> inv_bind_;
	std::vector<de::Transform> last_pose_, captured_;
	uint32_t seen_activations_ = 0;
	Paint paint_{};
	bool painted_ = false;
	std::array<float, 3> last_at_{0, 0, 0};
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> textures_;
	std::map<std::pair<const de::ObjectsFile *, uint32_t>, GLuint> painted_textures_;
	struct Attachment {
		de::Mesh mesh;
		std::vector<de::Material> materials;
		std::vector<MeshBatch> batches;
		std::vector<de::SkinBone> bones;
		std::vector<int> bone_channel;
		std::vector<de::Transform> inv_bind;
		de::AnimBank bank;
		int parent_channel = -1;  // the body bone it hangs from (a channel of `bank` above)
		std::string root;
		std::vector<de::Transform> last_pose, captured;
		uint32_t seen_activations = 0;
	};
	std::vector<Attachment> attachments_;
	const void *light_owner_ = nullptr;
	void draw_attachments(const float *vp, const float *eye, const de::Transform &world, const std::vector<de::Transform> &body, const AnimState &a);
	void load_attachments(const de::World &world, const de::ObjectsFile &f, size_t model, uint32_t body_mesh, const de::ObjectsFile &fighter,
	                      const de::ObjectInfo *body_deform, const de::ObjectInfo *body_bank);
	void follow(std::vector<de::Transform> &pose, const AnimState &a) const;
	void build();
	void draw_pose(const float *vp, const float *eye, const de::Transform &world, const std::vector<de::Transform> *pose);
	GLuint prog_ = 0;
};

}  // namespace render
