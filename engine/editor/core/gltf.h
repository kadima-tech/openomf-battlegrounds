// glTF 2.0 files (.gltf with its .bin and images, or .glb) as a plain in-memory scene, for the
// editor's model import and export (docs/EDITOR_SPEC.md §5.5).
//
// The scene mirrors glTF itself, in glTF's conventions (right-handed, +Y up, metres unless
// asset.extras says otherwise, column vectors, quaternions x y z w); converting to DE's space is
// the importers' job. Reading goes through cgltf (accessors unpacked to floats, sparse accessors
// applied, embedded / data: / external buffers and images resolved); images are decoded to RGBA
// (PNG through image_io, JPEG through stb_image). Writing produces a .glb (or .gltf + .bin) with
// one buffer, images stored as PNG. Extras (on the asset, nodes, meshes, primitives, materials,
// skins and animations) are kept as JSON values both ways.
//
// What it leaves out: morph targets, cameras, lights (KHR_lights_punctual), texture transforms
// and samplers (wrap modes), multiple scenes (only the default one's nodes are roots).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.h"

namespace gltf {

struct Error : std::runtime_error {
	using std::runtime_error::runtime_error;
};

using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;
using Mat4 = std::array<float, 16>;  // column-major, as glTF stores matrices

Mat4 identity();
Mat4 multiply(const Mat4 &a, const Mat4 &b);  // a * b
Mat4 compose(const Vec3 &t, const Vec4 &r, const Vec3 &s);
// Translation, rotation and scale of an affine matrix (no shear); false when it is degenerate.
bool decompose(const Mat4 &m, Vec3 *t, Vec4 *r, Vec3 *s);
Vec3 transform_point(const Mat4 &m, const Vec3 &p);
Vec3 transform_direction(const Mat4 &m, const Vec3 &d);
Mat4 inverse(const Mat4 &m);

// An image: decoded pixels (rows top to bottom) and, when read from a file, the original bytes.
struct Image {
	std::string name;
	std::string uri;        // the file name it came from or is written to ("" when embedded)
	std::string mime;       // "image/png" or "image/jpeg"
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;
	std::vector<uint8_t> encoded;  // as stored in the source file; writing uses PNG of `rgba`
	std::string error;      // why it could not be decoded (then rgba is empty)
};

// A texture reference: the image and the UV set (TEXCOORD_n) it uses.
struct TextureRef {
	int image = -1;
	int uv_set = 0;
	explicit operator bool() const { return image >= 0; }
};

struct Material {
	std::string name;
	Vec4 base_colour{1, 1, 1, 1};
	TextureRef base_texture;
	TextureRef normal_texture, emissive_texture, metallic_roughness_texture, occlusion_texture;
	Vec3 emissive{0, 0, 0};
	float metallic = 1, roughness = 1;
	enum class Alpha : uint8_t { Opaque, Mask, Blend } alpha = Alpha::Opaque;
	float alpha_cutoff = 0.5f;
	bool double_sided = false;
	bool unlit = false;  // KHR_materials_unlit
	json::Value extras;
};

// One draw of a mesh: triangles (other modes are converted or refused by the reader).
struct Primitive {
	std::vector<Vec3> positions, normals;
	std::vector<Vec4> tangents;
	std::vector<std::vector<Vec2>> uvs;       // TEXCOORD_0, TEXCOORD_1, ...
	std::vector<Vec4> colours;                // COLOR_0 (RGBA, 1 when the file has RGB)
	std::vector<std::array<uint16_t, 4>> joints;  // JOINTS_0: indices into the node's skin's joints
	std::vector<Vec4> weights;                // WEIGHTS_0
	std::vector<uint32_t> indices;            // 3 per triangle; generated when the file has none
	std::map<std::string, std::vector<float>> custom;  // scalar "_NAME" attributes (e.g. _DE_VERTEX)
	int material = -1;
	json::Value extras;
	size_t triangles() const { return indices.size() / 3; }
};

struct Mesh {
	std::string name;
	std::vector<Primitive> primitives;
	json::Value extras;
};

struct Node {
	std::string name;
	int parent = -1;
	std::vector<int> children;
	Vec3 translation{0, 0, 0};
	Vec4 rotation{0, 0, 0, 1};  // x y z w
	Vec3 scale{1, 1, 1};
	int mesh = -1;
	int skin = -1;
	json::Value extras;
	Mat4 local() const { return compose(translation, rotation, scale); }
};

struct Skin {
	std::string name;
	std::vector<int> joints;          // node indices
	std::vector<Mat4> inverse_binds;  // one per joint (identity when the file has none)
	int skeleton = -1;                // the common root node, when given
	json::Value extras;
};

struct Animation {
	enum class Path : uint8_t { Translation, Rotation, Scale, Weights };
	enum class Interpolation : uint8_t { Linear, Step, CubicSpline };
	struct Sampler {
		std::vector<float> input;   // key times in seconds
		std::vector<float> output;  // flat: 3 (translation, scale) or 4 (rotation) floats per key;
		                            // cubic spline keys hold in-tangent, value, out-tangent
		Interpolation interpolation = Interpolation::Linear;
	};
	struct Channel {
		int sampler = -1;
		int node = -1;
		Path path = Path::Translation;
	};
	std::string name;
	std::vector<Sampler> samplers;
	std::vector<Channel> channels;
	json::Value extras;
	float duration() const;
};

struct Scene {
	std::vector<Node> nodes;
	std::vector<int> roots;  // the default scene's root nodes
	std::vector<Mesh> meshes;
	std::vector<Material> materials;
	std::vector<Image> images;
	std::vector<Skin> skins;
	std::vector<Animation> animations;
	std::string generator;
	json::Value extras;  // asset.extras
	std::vector<std::string> warnings;  // from reading: what was skipped or approximated

	// A node's transform in the scene (its ancestors' local transforms applied).
	Mat4 world(int node) const;
	// Adds a node under `parent` (-1: a root) and returns its index.
	int add_node(Node n, int parent = -1);
	// The node's name and those of its ancestors, root first.
	std::vector<std::string> path(int node) const;
};

// Reads a .gltf or .glb file (relative image and buffer URIs resolve next to it).
Scene read(const std::filesystem::path &path);
// Reads a .glb or .gltf held in memory; `base` resolves relative URIs ("" refuses them).
Scene read(const std::vector<uint8_t> &bytes, const std::filesystem::path &base = {});

// A .glb when the extension is .glb, else a .gltf with "<stem>.bin" and the images as PNG files
// beside it.
void write(const Scene &scene, const std::filesystem::path &path);
std::vector<uint8_t> write_glb(const Scene &scene);

// Decoded RGBA from PNG or JPEG bytes; throws Error.
void decode_image(const std::vector<uint8_t> &bytes, int *width, int *height, std::vector<uint8_t> *rgba);

}  // namespace gltf
