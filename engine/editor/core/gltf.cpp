#include "gltf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>

#include "image_io.h"

#include "cgltf.h"
#include "stb_image.h"

namespace gltf {

// ---------------------------------------------------------------------------------------------
// Matrices

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4 multiply(const Mat4 &a, const Mat4 &b) {
	Mat4 o{};
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			float s = 0;
			for (int k = 0; k < 4; ++k) s += a[size_t(k * 4 + r)] * b[size_t(c * 4 + k)];
			o[size_t(c * 4 + r)] = s;
		}
	}
	return o;
}

Mat4 compose(const Vec3 &t, const Vec4 &q, const Vec3 &s) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	Mat4 m = identity();
	m[0] = (1 - 2 * (y * y + z * z)) * s[0];
	m[1] = (2 * (x * y + z * w)) * s[0];
	m[2] = (2 * (x * z - y * w)) * s[0];
	m[4] = (2 * (x * y - z * w)) * s[1];
	m[5] = (1 - 2 * (x * x + z * z)) * s[1];
	m[6] = (2 * (y * z + x * w)) * s[1];
	m[8] = (2 * (x * z + y * w)) * s[2];
	m[9] = (2 * (y * z - x * w)) * s[2];
	m[10] = (1 - 2 * (x * x + y * y)) * s[2];
	m[12] = t[0], m[13] = t[1], m[14] = t[2];
	return m;
}

bool decompose(const Mat4 &m, Vec3 *t, Vec4 *r, Vec3 *s) {
	Vec3 sc;
	for (int c = 0; c < 3; ++c) sc[size_t(c)] = std::sqrt(m[size_t(c * 4)] * m[size_t(c * 4)] + m[size_t(c * 4 + 1)] * m[size_t(c * 4 + 1)] + m[size_t(c * 4 + 2)] * m[size_t(c * 4 + 2)]);
	if (sc[0] < 1e-12f || sc[1] < 1e-12f || sc[2] < 1e-12f) return false;
	float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
	if (det < 0) sc[0] = -sc[0];
	float a[9];
	for (int c = 0; c < 3; ++c) {
		for (int k = 0; k < 3; ++k) a[c * 3 + k] = m[size_t(c * 4 + k)] / sc[size_t(c)];
	}
	// a is column-major: a[c*3+k] = R(k, c).
	float m00 = a[0], m11 = a[4], m22 = a[8];
	float tr = m00 + m11 + m22;
	Vec4 q;
	if (tr > 0) {
		float S = std::sqrt(tr + 1) * 2;
		q = {(a[5] - a[7]) / S, (a[6] - a[2]) / S, (a[1] - a[3]) / S, 0.25f * S};
	} else if (m00 > m11 && m00 > m22) {
		float S = std::sqrt(1 + m00 - m11 - m22) * 2;
		q = {0.25f * S, (a[3] + a[1]) / S, (a[6] + a[2]) / S, (a[5] - a[7]) / S};
	} else if (m11 > m22) {
		float S = std::sqrt(1 + m11 - m00 - m22) * 2;
		q = {(a[3] + a[1]) / S, 0.25f * S, (a[7] + a[5]) / S, (a[6] - a[2]) / S};
	} else {
		float S = std::sqrt(1 + m22 - m00 - m11) * 2;
		q = {(a[6] + a[2]) / S, (a[7] + a[5]) / S, 0.25f * S, (a[1] - a[3]) / S};
	}
	float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	for (auto &v : q) v /= l;
	if (t) *t = {m[12], m[13], m[14]};
	if (r) *r = q;
	if (s) *s = sc;
	return true;
}

Vec3 transform_point(const Mat4 &m, const Vec3 &p) {
	return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
	        m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

Vec3 transform_direction(const Mat4 &m, const Vec3 &d) {
	return {m[0] * d[0] + m[4] * d[1] + m[8] * d[2], m[1] * d[0] + m[5] * d[1] + m[9] * d[2], m[2] * d[0] + m[6] * d[1] + m[10] * d[2]};
}

Mat4 inverse(const Mat4 &m) {
	Mat4 inv;
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (std::fabs(det) < 1e-30f) return identity();
	for (auto &v : inv) v /= det;
	return inv;
}

float Animation::duration() const {
	float d = 0;
	for (const auto &s : samplers) {
		if (!s.input.empty()) d = std::max(d, s.input.back());
	}
	return d;
}

Mat4 Scene::world(int node) const {
	Mat4 m = identity();
	for (int n = node; n >= 0 && size_t(n) < nodes.size(); n = nodes[size_t(n)].parent) m = multiply(nodes[size_t(n)].local(), m);
	return m;
}

int Scene::add_node(Node n, int parent) {
	int id = int(nodes.size());
	n.parent = parent;
	nodes.push_back(std::move(n));
	if (parent >= 0) nodes[size_t(parent)].children.push_back(id);
	else roots.push_back(id);
	return id;
}

std::vector<std::string> Scene::path(int node) const {
	std::vector<std::string> out;
	for (int n = node; n >= 0 && size_t(n) < nodes.size(); n = nodes[size_t(n)].parent) out.insert(out.begin(), nodes[size_t(n)].name);
	return out;
}

// ---------------------------------------------------------------------------------------------
// Reading

void decode_image(const std::vector<uint8_t> &bytes, int *width, int *height, std::vector<uint8_t> *rgba) {
	if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') {
		try {
			ed::RgbaImage img = ed::decode_png(bytes);
			*width = img.width, *height = img.height, *rgba = std::move(img.rgba);
			return;
		} catch (const std::exception &e) {
			throw Error(e.what());
		}
	}
	if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8) {
		int w = 0, h = 0, n = 0;
		unsigned char *px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
		if (!px) throw Error(std::string("JPEG: ") + stbi_failure_reason());
		rgba->assign(px, px + size_t(w) * size_t(h) * 4);
		stbi_image_free(px);
		*width = w, *height = h;
		return;
	}
	throw Error("not a PNG or JPEG image");
}

namespace {

std::string str(const char *s) { return s ? s : ""; }

json::Value extras_of(const cgltf_data *d, const cgltf_extras &e) {
	std::string text;
	if (e.data) text = e.data;
	// cgltf fills only the offsets of a mesh's extras (it reads targetNames from them).
	else if (d->json && e.end_offset > e.start_offset && e.end_offset <= d->json_size) text.assign(d->json + e.start_offset, d->json + e.end_offset);
	if (text.empty()) return {};
	try {
		return json::Value::parse(text);
	} catch (const std::exception &) {
		return {};
	}
}

std::vector<float> unpack(const cgltf_accessor *a, size_t components) {
	std::vector<float> out;
	if (!a) return out;
	size_t n = cgltf_num_components(a->type);
	if (n != components) throw Error("accessor has " + std::to_string(n) + " components, expected " + std::to_string(components));
	out.resize(a->count * n);
	cgltf_accessor_unpack_floats(a, out.data(), out.size());
	return out;
}

template <size_t N>
std::vector<std::array<float, N>> unpack_vec(const cgltf_accessor *a) {
	auto flat = unpack(a, N);
	std::vector<std::array<float, N>> out(flat.size() / N);
	for (size_t i = 0; i < out.size(); ++i) {
		for (size_t k = 0; k < N; ++k) out[i][k] = flat[i * N + k];
	}
	return out;
}

std::vector<uint8_t> read_bytes(const std::filesystem::path &p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) throw Error("cannot open " + p.string());
	return {std::istreambuf_iterator<char>(f), {}};
}

std::string percent_decode(const std::string &s) {
	std::string out;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '%' && i + 2 < s.size() && isxdigit(uint8_t(s[i + 1])) && isxdigit(uint8_t(s[i + 2]))) {
			out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

TextureRef texture_ref(const cgltf_data *data, const cgltf_texture_view &v) {
	TextureRef r;
	if (!v.texture || !v.texture->image) return r;
	r.image = int(cgltf_image_index(data, v.texture->image));
	r.uv_set = v.texcoord;
	return r;
}

Primitive read_primitive(const cgltf_data *data, const cgltf_primitive &p, std::vector<std::string> &warnings, const std::string &mesh_name) {
	Primitive out;
	if (p.material) out.material = int(cgltf_material_index(data, p.material));
	out.extras = extras_of(data, p.extras);
	size_t count = 0;
	for (size_t a = 0; a < p.attributes_count; ++a) {
		const cgltf_attribute &at = p.attributes[a];
		switch (at.type) {
		case cgltf_attribute_type_position:
			out.positions = unpack_vec<3>(at.data);
			count = out.positions.size();
			break;
		case cgltf_attribute_type_normal: out.normals = unpack_vec<3>(at.data); break;
		case cgltf_attribute_type_tangent: out.tangents = unpack_vec<4>(at.data); break;
		case cgltf_attribute_type_texcoord:
			if (at.index >= 0 && at.index < 8) {
				if (out.uvs.size() <= size_t(at.index)) out.uvs.resize(size_t(at.index) + 1);
				out.uvs[size_t(at.index)] = unpack_vec<2>(at.data);
			}
			break;
		case cgltf_attribute_type_color:
			if (at.index == 0) {
				if (cgltf_num_components(at.data->type) == 3) {
					for (auto &c : unpack_vec<3>(at.data)) out.colours.push_back({c[0], c[1], c[2], 1});
				} else {
					out.colours = unpack_vec<4>(at.data);
				}
			}
			break;
		case cgltf_attribute_type_joints:
			if (at.index == 0) {
				for (auto &j : unpack_vec<4>(at.data)) out.joints.push_back({uint16_t(j[0]), uint16_t(j[1]), uint16_t(j[2]), uint16_t(j[3])});
			} else {
				warnings.push_back(mesh_name + ": only the first 4 joint influences per vertex are read");
			}
			break;
		case cgltf_attribute_type_weights:
			if (at.index == 0) out.weights = unpack_vec<4>(at.data);
			break;
		case cgltf_attribute_type_custom:
			if (at.name && cgltf_num_components(at.data->type) == 1) out.custom[at.name] = unpack(at.data, 1);
			break;
		default: break;
		}
	}
	if (p.targets_count) warnings.push_back(mesh_name + ": morph targets are not imported");
	std::vector<uint32_t> idx;
	if (p.indices) {
		idx.resize(p.indices->count);
		for (size_t i = 0; i < idx.size(); ++i) idx[i] = uint32_t(cgltf_accessor_read_index(p.indices, i));
	} else {
		idx.resize(count);
		for (size_t i = 0; i < count; ++i) idx[i] = uint32_t(i);
	}
	for (uint32_t i : idx) {
		if (i >= count) throw Error(mesh_name + ": an index is past the vertex count");
	}
	switch (p.type) {
	case cgltf_primitive_type_triangles: out.indices = std::move(idx); break;
	case cgltf_primitive_type_triangle_strip:
		for (size_t i = 2; i < idx.size(); ++i) {
			if (i % 2) out.indices.insert(out.indices.end(), {idx[i - 1], idx[i - 2], idx[i]});
			else out.indices.insert(out.indices.end(), {idx[i - 2], idx[i - 1], idx[i]});
		}
		break;
	case cgltf_primitive_type_triangle_fan:
		for (size_t i = 2; i < idx.size(); ++i) out.indices.insert(out.indices.end(), {idx[0], idx[i - 1], idx[i]});
		break;
	default:
		warnings.push_back(mesh_name + ": points and lines are skipped");
		out.indices.clear();
		break;
	}
	out.indices.resize(out.indices.size() / 3 * 3);
	return out;
}

Scene convert(cgltf_data *data, const std::filesystem::path &base) {
	Scene s;
	s.generator = str(data->asset.generator);
	s.extras = extras_of(data, data->asset.extras);
	for (size_t i = 0; i < data->extensions_required_count; ++i) {
		std::string e = data->extensions_required[i];
		if (e != "KHR_materials_unlit" && e != "KHR_texture_transform") s.warnings.push_back("required extension " + e + " is not supported");
	}
	// Images.
	for (size_t i = 0; i < data->images_count; ++i) {
		const cgltf_image &ci = data->images[i];
		Image img;
		img.name = str(ci.name);
		img.mime = str(ci.mime_type);
		try {
			if (ci.buffer_view) {
				const uint8_t *p = cgltf_buffer_view_data(ci.buffer_view);
				if (!p) throw Error("its buffer is not loaded");
				img.encoded.assign(p, p + ci.buffer_view->size);
			} else if (ci.uri && strncmp(ci.uri, "data:", 5) == 0) {
				const char *comma = strchr(ci.uri, ',');
				if (!comma || !strstr(ci.uri, ";base64")) throw Error("unsupported data URI");
				std::string b64(comma + 1);
				size_t n = b64.size() * 3 / 4;
				while (!b64.empty() && b64.back() == '=') b64.pop_back(), --n;
				void *out = nullptr;
				cgltf_options opt{};
				if (cgltf_load_buffer_base64(&opt, n, comma + 1, &out) != cgltf_result_success) throw Error("bad base64 data");
				img.encoded.assign(static_cast<uint8_t *>(out), static_cast<uint8_t *>(out) + n);
				free(out);
			} else if (ci.uri) {
				img.uri = percent_decode(ci.uri);
				if (base.empty()) throw Error("external image " + img.uri + " without a folder");
				img.encoded = read_bytes(base / std::filesystem::path(std::u8string(img.uri.begin(), img.uri.end())));
			}
			if (img.name.empty()) img.name = img.uri.empty() ? "Image" + std::to_string(i) : std::filesystem::path(img.uri).stem().string();
			decode_image(img.encoded, &img.width, &img.height, &img.rgba);
		} catch (const std::exception &e) {
			img.error = e.what();
			s.warnings.push_back("image " + (img.name.empty() ? std::to_string(i) : img.name) + ": " + e.what());
		}
		s.images.push_back(std::move(img));
	}
	// Materials.
	for (size_t i = 0; i < data->materials_count; ++i) {
		const cgltf_material &cm = data->materials[i];
		Material m;
		m.name = cm.name ? cm.name : "Material" + std::to_string(i);
		if (cm.has_pbr_metallic_roughness) {
			const auto &pbr = cm.pbr_metallic_roughness;
			for (int k = 0; k < 4; ++k) m.base_colour[size_t(k)] = pbr.base_color_factor[k];
			m.base_texture = texture_ref(data, pbr.base_color_texture);
			m.metallic_roughness_texture = texture_ref(data, pbr.metallic_roughness_texture);
			m.metallic = pbr.metallic_factor, m.roughness = pbr.roughness_factor;
		} else if (cm.has_pbr_specular_glossiness) {
			const auto &sg = cm.pbr_specular_glossiness;
			for (int k = 0; k < 4; ++k) m.base_colour[size_t(k)] = sg.diffuse_factor[k];
			m.base_texture = texture_ref(data, sg.diffuse_texture);
		}
		m.normal_texture = texture_ref(data, cm.normal_texture);
		m.emissive_texture = texture_ref(data, cm.emissive_texture);
		m.occlusion_texture = texture_ref(data, cm.occlusion_texture);
		for (int k = 0; k < 3; ++k) m.emissive[size_t(k)] = cm.emissive_factor[k];
		m.alpha = cm.alpha_mode == cgltf_alpha_mode_mask ? Material::Alpha::Mask : cm.alpha_mode == cgltf_alpha_mode_blend ? Material::Alpha::Blend : Material::Alpha::Opaque;
		m.alpha_cutoff = cm.alpha_cutoff;
		m.double_sided = cm.double_sided;
		m.unlit = cm.unlit;
		m.extras = extras_of(data, cm.extras);
		s.materials.push_back(std::move(m));
	}
	// Meshes.
	for (size_t i = 0; i < data->meshes_count; ++i) {
		const cgltf_mesh &cm = data->meshes[i];
		Mesh m;
		m.name = cm.name ? cm.name : "Mesh" + std::to_string(i);
		m.extras = extras_of(data, cm.extras);
		for (size_t p = 0; p < cm.primitives_count; ++p) {
			Primitive prim = read_primitive(data, cm.primitives[p], s.warnings, m.name);
			if (!prim.indices.empty()) m.primitives.push_back(std::move(prim));
		}
		s.meshes.push_back(std::move(m));
	}
	// Nodes.
	s.nodes.resize(data->nodes_count);
	for (size_t i = 0; i < data->nodes_count; ++i) {
		const cgltf_node &cn = data->nodes[i];
		Node &n = s.nodes[i];
		n.name = cn.name ? cn.name : "Node" + std::to_string(i);
		if (cn.has_matrix) {
			Mat4 m;
			for (int k = 0; k < 16; ++k) m[size_t(k)] = cn.matrix[k];
			if (!decompose(m, &n.translation, &n.rotation, &n.scale)) s.warnings.push_back(n.name + ": degenerate matrix");
		} else {
			if (cn.has_translation) n.translation = {cn.translation[0], cn.translation[1], cn.translation[2]};
			if (cn.has_rotation) n.rotation = {cn.rotation[0], cn.rotation[1], cn.rotation[2], cn.rotation[3]};
			if (cn.has_scale) n.scale = {cn.scale[0], cn.scale[1], cn.scale[2]};
		}
		if (cn.mesh) n.mesh = int(cgltf_mesh_index(data, cn.mesh));
		if (cn.skin) n.skin = int(cgltf_skin_index(data, cn.skin));
		if (cn.parent) n.parent = int(cgltf_node_index(data, cn.parent));
		for (size_t c = 0; c < cn.children_count; ++c) n.children.push_back(int(cgltf_node_index(data, cn.children[c])));
		n.extras = extras_of(data, cn.extras);
		if (cn.camera || cn.light) s.warnings.push_back(n.name + ": cameras and lights are not imported");
	}
	const cgltf_scene *scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
	if (scene) {
		for (size_t i = 0; i < scene->nodes_count; ++i) s.roots.push_back(int(cgltf_node_index(data, scene->nodes[i])));
	} else {
		for (size_t i = 0; i < s.nodes.size(); ++i) {
			if (s.nodes[i].parent < 0) s.roots.push_back(int(i));
		}
	}
	// Skins.
	for (size_t i = 0; i < data->skins_count; ++i) {
		const cgltf_skin &cs = data->skins[i];
		Skin sk;
		sk.name = str(cs.name);
		for (size_t j = 0; j < cs.joints_count; ++j) sk.joints.push_back(int(cgltf_node_index(data, cs.joints[j])));
		if (cs.skeleton) sk.skeleton = int(cgltf_node_index(data, cs.skeleton));
		if (cs.inverse_bind_matrices) {
			auto flat = unpack(cs.inverse_bind_matrices, 16);
			for (size_t j = 0; j < cs.joints_count; ++j) {
				Mat4 m = identity();
				if ((j + 1) * 16 <= flat.size()) std::copy(flat.begin() + long(j * 16), flat.begin() + long(j * 16 + 16), m.begin());
				sk.inverse_binds.push_back(m);
			}
		} else {
			sk.inverse_binds.assign(sk.joints.size(), identity());
		}
		sk.extras = extras_of(data, cs.extras);
		s.skins.push_back(std::move(sk));
	}
	// Animations.
	for (size_t i = 0; i < data->animations_count; ++i) {
		const cgltf_animation &ca = data->animations[i];
		Animation a;
		a.name = ca.name ? ca.name : "Animation" + std::to_string(i);
		a.extras = extras_of(data, ca.extras);
		for (size_t k = 0; k < ca.samplers_count; ++k) {
			const cgltf_animation_sampler &cs = ca.samplers[k];
			Animation::Sampler smp;
			smp.input = unpack(cs.input, 1);
			size_t nc = cgltf_num_components(cs.output->type);
			smp.output = unpack(cs.output, nc);
			smp.interpolation = cs.interpolation == cgltf_interpolation_type_step ? Animation::Interpolation::Step
			                    : cs.interpolation == cgltf_interpolation_type_cubic_spline ? Animation::Interpolation::CubicSpline
			                                                                                 : Animation::Interpolation::Linear;
			a.samplers.push_back(std::move(smp));
		}
		for (size_t k = 0; k < ca.channels_count; ++k) {
			const cgltf_animation_channel &cc = ca.channels[k];
			if (!cc.target_node || !cc.sampler) continue;
			Animation::Channel ch;
			ch.sampler = int(cc.sampler - ca.samplers);
			ch.node = int(cgltf_node_index(data, cc.target_node));
			switch (cc.target_path) {
			case cgltf_animation_path_type_translation: ch.path = Animation::Path::Translation; break;
			case cgltf_animation_path_type_rotation: ch.path = Animation::Path::Rotation; break;
			case cgltf_animation_path_type_scale: ch.path = Animation::Path::Scale; break;
			case cgltf_animation_path_type_weights: ch.path = Animation::Path::Weights; break;
			default: continue;
			}
			a.channels.push_back(ch);
		}
		s.animations.push_back(std::move(a));
	}
	return s;
}

const char *result_text(cgltf_result r) {
	switch (r) {
	case cgltf_result_data_too_short: return "the file is too short";
	case cgltf_result_unknown_format: return "not a glTF file";
	case cgltf_result_invalid_json: return "invalid JSON";
	case cgltf_result_invalid_gltf: return "invalid glTF";
	case cgltf_result_invalid_options: return "invalid options";
	case cgltf_result_file_not_found: return "a referenced file is missing";
	case cgltf_result_io_error: return "a read failed";
	case cgltf_result_out_of_memory: return "out of memory";
	case cgltf_result_legacy_gltf: return "glTF 1.0 is not supported";
	default: return "cannot read the file";
	}
}

struct DataGuard {
	cgltf_data *d = nullptr;
	~DataGuard() {
		if (d) cgltf_free(d);
	}
};

Scene read_with(const std::vector<uint8_t> &bytes, const std::filesystem::path &base, const std::string &label) {
	cgltf_options opt{};
	DataGuard g;
	cgltf_result r = cgltf_parse(&opt, bytes.data(), bytes.size(), &g.d);
	if (r != cgltf_result_success) throw Error(label + ": " + result_text(r));
	// External buffers: cgltf resolves them against a path, so give it one inside `base`.
	std::string gltf_path = base.empty() ? std::string() : (base / "x.gltf").string();
	for (size_t i = 0; i < g.d->buffers_count; ++i) {
		const char *uri = g.d->buffers[i].uri;
		if (uri && strncmp(uri, "data:", 5) != 0 && base.empty()) throw Error(label + ": an external buffer needs the file's folder");
	}
	r = cgltf_load_buffers(&opt, g.d, gltf_path.empty() ? nullptr : gltf_path.c_str());
	if (r != cgltf_result_success) throw Error(label + ": " + result_text(r));
	r = cgltf_validate(g.d);
	if (r != cgltf_result_success) throw Error(label + ": " + result_text(r));
	return convert(g.d, base);
}

}  // namespace

Scene read(const std::filesystem::path &path) { return read_with(read_bytes(path), path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path(), path.filename().string()); }

Scene read(const std::vector<uint8_t> &bytes, const std::filesystem::path &base) { return read_with(bytes, base, "glTF"); }

// ---------------------------------------------------------------------------------------------
// Writing

namespace {

struct Writer {
	explicit Writer(const Scene &scene) : s(scene) {}
	const Scene &s;
	json::Value root = json::Value::object();
	json::Value accessors = json::Value::array(), views = json::Value::array();
	std::vector<uint8_t> bin;

	void align() {
		while (bin.size() % 4) bin.push_back(0);
	}
	int view(const void *data, size_t size, int target) {
		align();
		json::Value v = json::Value::object();
		v["buffer"] = 0;
		v["byteOffset"] = int64_t(bin.size());
		v["byteLength"] = int64_t(size);
		if (target) v["target"] = target;
		const uint8_t *p = static_cast<const uint8_t *>(data);
		bin.insert(bin.end(), p, p + size);
		views.push(std::move(v));
		return int(views.size()) - 1;
	}
	static json::Value num(float f) { return std::isfinite(f) ? json::Value(double(f)) : json::Value(0.0); }
	int floats(const std::vector<float> &data, size_t components, const char *type, bool bounds, int target = 34962) {
		size_t count = data.size() / components;
		json::Value a = json::Value::object();
		a["bufferView"] = view(data.data(), data.size() * 4, target);
		a["componentType"] = 5126;
		a["count"] = int64_t(count);
		a["type"] = type;
		if (bounds && count) {
			json::Value lo = json::Value::array(), hi = json::Value::array();
			for (size_t k = 0; k < components; ++k) {
				float l = data[k], h = data[k];
				for (size_t i = 1; i < count; ++i) l = std::min(l, data[i * components + k]), h = std::max(h, data[i * components + k]);
				lo.push(num(l)), hi.push(num(h));
			}
			a["min"] = std::move(lo);
			a["max"] = std::move(hi);
		}
		accessors.push(std::move(a));
		return int(accessors.size()) - 1;
	}
	template <size_t N>
	int vecs(const std::vector<std::array<float, N>> &v, const char *type, bool bounds = false, int target = 34962) {
		std::vector<float> flat;
		flat.reserve(v.size() * N);
		for (auto &x : v) flat.insert(flat.end(), x.begin(), x.end());
		return floats(flat, N, type, bounds, target);
	}
	int indices(const std::vector<uint32_t> &idx, size_t vertex_count) {
		json::Value a = json::Value::object();
		if (vertex_count <= 65535) {
			std::vector<uint16_t> s16(idx.begin(), idx.end());
			a["bufferView"] = view(s16.data(), s16.size() * 2, 34963);
			a["componentType"] = 5123;
		} else {
			a["bufferView"] = view(idx.data(), idx.size() * 4, 34963);
			a["componentType"] = 5125;
		}
		a["count"] = int64_t(idx.size());
		a["type"] = "SCALAR";
		accessors.push(std::move(a));
		return int(accessors.size()) - 1;
	}
	int joints(const std::vector<std::array<uint16_t, 4>> &j) {
		std::vector<uint16_t> flat;
		for (auto &x : j) flat.insert(flat.end(), x.begin(), x.end());
		json::Value a = json::Value::object();
		a["bufferView"] = view(flat.data(), flat.size() * 2, 34962);
		a["componentType"] = 5123;
		a["count"] = int64_t(j.size());
		a["type"] = "VEC4";
		accessors.push(std::move(a));
		return int(accessors.size()) - 1;
	}
	static json::Value vec(const float *v, size_t n) {
		json::Value a = json::Value::array();
		for (size_t i = 0; i < n; ++i) a.push(num(v[i]));
		return a;
	}
	static void put_extras(json::Value &o, const json::Value &e) {
		if (!e.is_null()) o["extras"] = e;
	}
	static json::Value texinfo(const TextureRef &t) {
		json::Value v = json::Value::object();
		v["index"] = t.image;
		if (t.uv_set) v["texCoord"] = t.uv_set;
		return v;
	}

	void build(bool embed_images, const std::string &bin_uri, std::vector<std::pair<std::string, std::vector<uint8_t>>> *files) {
		json::Value asset = json::Value::object();
		asset["version"] = "2.0";
		asset["generator"] = s.generator.empty() ? "OpenBG content editor" : s.generator;
		put_extras(asset, s.extras);
		root["asset"] = std::move(asset);
		std::set<std::string> used_uris;
		if (!s.images.empty()) {
			json::Value images = json::Value::array(), textures = json::Value::array(), samplers = json::Value::array();
			json::Value smp = json::Value::object();
			smp["magFilter"] = 9729;
			smp["minFilter"] = 9987;
			samplers.push(std::move(smp));
			for (size_t i = 0; i < s.images.size(); ++i) {
				const Image &img = s.images[i];
				std::vector<uint8_t> png;
				if (!img.rgba.empty() && img.width > 0 && img.height > 0) png = ed::encode_png(img.width, img.height, img.rgba);
				else png = ed::encode_png(1, 1, {255, 255, 255, 255});
				json::Value v = json::Value::object();
				if (!img.name.empty()) v["name"] = img.name;
				if (embed_images) {
					v["bufferView"] = view(png.data(), png.size(), 0);
					v["mimeType"] = "image/png";
				} else {
					std::string base = img.name.empty() ? "image" + std::to_string(i) : img.name;
					for (auto &c : base) {
						if (strchr("\\/:*?\"<>| ", c)) c = '_';
					}
					std::string uri = base + ".png";
					for (int k = 2; used_uris.count(uri); ++k) uri = base + "_" + std::to_string(k) + ".png";
					used_uris.insert(uri);
					v["uri"] = uri;
					files->push_back({uri, std::move(png)});
				}
				images.push(std::move(v));
				json::Value t = json::Value::object();
				t["sampler"] = 0;
				t["source"] = int64_t(i);
				textures.push(std::move(t));
			}
			root["images"] = std::move(images);
			root["samplers"] = std::move(samplers);
			root["textures"] = std::move(textures);
		}
		if (!s.materials.empty()) {
			json::Value mats = json::Value::array();
			bool unlit = false;
			for (const Material &m : s.materials) {
				json::Value v = json::Value::object();
				v["name"] = m.name;
				json::Value pbr = json::Value::object();
				pbr["baseColorFactor"] = vec(m.base_colour.data(), 4);
				if (m.base_texture) pbr["baseColorTexture"] = texinfo(m.base_texture);
				if (m.metallic_roughness_texture) pbr["metallicRoughnessTexture"] = texinfo(m.metallic_roughness_texture);
				pbr["metallicFactor"] = num(m.metallic);
				pbr["roughnessFactor"] = num(m.roughness);
				v["pbrMetallicRoughness"] = std::move(pbr);
				if (m.normal_texture) v["normalTexture"] = texinfo(m.normal_texture);
				if (m.emissive_texture) v["emissiveTexture"] = texinfo(m.emissive_texture);
				if (m.occlusion_texture) v["occlusionTexture"] = texinfo(m.occlusion_texture);
				if (m.emissive != Vec3{0, 0, 0}) v["emissiveFactor"] = vec(m.emissive.data(), 3);
				if (m.alpha == Material::Alpha::Mask) {
					v["alphaMode"] = "MASK";
					v["alphaCutoff"] = num(m.alpha_cutoff);
				} else if (m.alpha == Material::Alpha::Blend) {
					v["alphaMode"] = "BLEND";
				}
				if (m.double_sided) v["doubleSided"] = true;
				if (m.unlit) {
					json::Value ext = json::Value::object();
					ext["KHR_materials_unlit"] = json::Value::object();
					v["extensions"] = std::move(ext);
					unlit = true;
				}
				put_extras(v, m.extras);
				mats.push(std::move(v));
			}
			root["materials"] = std::move(mats);
			if (unlit) {
				json::Value used = json::Value::array();
				used.push("KHR_materials_unlit");
				root["extensionsUsed"] = std::move(used);
			}
		}
		if (!s.meshes.empty()) {
			json::Value meshes = json::Value::array();
			for (const Mesh &m : s.meshes) {
				json::Value v = json::Value::object();
				v["name"] = m.name;
				json::Value prims = json::Value::array();
				for (const Primitive &p : m.primitives) {
					json::Value pv = json::Value::object(), attrs = json::Value::object();
					attrs["POSITION"] = vecs(p.positions, "VEC3", true);
					if (p.normals.size() == p.positions.size()) attrs["NORMAL"] = vecs(p.normals, "VEC3");
					if (p.tangents.size() == p.positions.size()) attrs["TANGENT"] = vecs(p.tangents, "VEC4");
					for (size_t k = 0; k < p.uvs.size(); ++k) {
						if (p.uvs[k].size() == p.positions.size()) attrs["TEXCOORD_" + std::to_string(k)] = vecs(p.uvs[k], "VEC2");
					}
					if (p.colours.size() == p.positions.size()) attrs["COLOR_0"] = vecs(p.colours, "VEC4");
					for (const auto &[name, values] : p.custom) {
						if (values.size() == p.positions.size() && !name.empty() && name[0] == '_') attrs[name] = floats(values, 1, "SCALAR", false);
					}
					if (p.joints.size() == p.positions.size() && p.weights.size() == p.positions.size()) {
						attrs["JOINTS_0"] = joints(p.joints);
						attrs["WEIGHTS_0"] = vecs(p.weights, "VEC4");
					}
					pv["attributes"] = std::move(attrs);
					pv["indices"] = indices(p.indices, p.positions.size());
					if (p.material >= 0) pv["material"] = p.material;
					pv["mode"] = 4;
					put_extras(pv, p.extras);
					prims.push(std::move(pv));
				}
				v["primitives"] = std::move(prims);
				put_extras(v, m.extras);
				meshes.push(std::move(v));
			}
			root["meshes"] = std::move(meshes);
		}
		if (!s.nodes.empty()) {
			json::Value nodes = json::Value::array();
			for (const Node &n : s.nodes) {
				json::Value v = json::Value::object();
				v["name"] = n.name;
				if (!n.children.empty()) {
					json::Value c = json::Value::array();
					for (int k : n.children) c.push(k);
					v["children"] = std::move(c);
				}
				if (n.translation != Vec3{0, 0, 0}) v["translation"] = vec(n.translation.data(), 3);
				if (n.rotation != Vec4{0, 0, 0, 1}) v["rotation"] = vec(n.rotation.data(), 4);
				if (n.scale != Vec3{1, 1, 1}) v["scale"] = vec(n.scale.data(), 3);
				if (n.mesh >= 0) v["mesh"] = n.mesh;
				if (n.skin >= 0) v["skin"] = n.skin;
				put_extras(v, n.extras);
				nodes.push(std::move(v));
			}
			root["nodes"] = std::move(nodes);
			json::Value scene = json::Value::object(), list = json::Value::array();
			for (int r : s.roots) list.push(r);
			scene["nodes"] = std::move(list);
			json::Value scenes = json::Value::array();
			scenes.push(std::move(scene));
			root["scenes"] = std::move(scenes);
			root["scene"] = 0;
		}
		if (!s.skins.empty()) {
			json::Value skins = json::Value::array();
			for (const Skin &sk : s.skins) {
				json::Value v = json::Value::object();
				if (!sk.name.empty()) v["name"] = sk.name;
				json::Value j = json::Value::array();
				for (int k : sk.joints) j.push(k);
				v["joints"] = std::move(j);
				std::vector<float> flat;
				for (size_t k = 0; k < sk.joints.size(); ++k) {
					Mat4 m = k < sk.inverse_binds.size() ? sk.inverse_binds[k] : identity();
					flat.insert(flat.end(), m.begin(), m.end());
				}
				v["inverseBindMatrices"] = floats(flat, 16, "MAT4", false, 0);
				if (sk.skeleton >= 0) v["skeleton"] = sk.skeleton;
				put_extras(v, sk.extras);
				skins.push(std::move(v));
			}
			root["skins"] = std::move(skins);
		}
		if (!s.animations.empty()) {
			json::Value anims = json::Value::array();
			for (const Animation &a : s.animations) {
				json::Value v = json::Value::object();
				v["name"] = a.name;
				json::Value samplers = json::Value::array(), channels = json::Value::array();
				std::vector<int> components(a.samplers.size(), 3);
				for (const auto &c : a.channels) {
					if (c.sampler >= 0 && size_t(c.sampler) < components.size()) components[size_t(c.sampler)] = c.path == Animation::Path::Rotation ? 4 : 3;
				}
				for (size_t k = 0; k < a.samplers.size(); ++k) {
					const auto &smp = a.samplers[k];
					json::Value sv = json::Value::object();
					sv["input"] = floats(smp.input, 1, "SCALAR", true, 0);
					sv["output"] = floats(smp.output, size_t(components[k]), components[k] == 4 ? "VEC4" : "VEC3", false, 0);
					sv["interpolation"] = smp.interpolation == Animation::Interpolation::Step ? "STEP" : smp.interpolation == Animation::Interpolation::CubicSpline ? "CUBICSPLINE" : "LINEAR";
					samplers.push(std::move(sv));
				}
				for (const auto &c : a.channels) {
					json::Value cv = json::Value::object(), target = json::Value::object();
					cv["sampler"] = c.sampler;
					target["node"] = c.node;
					target["path"] = c.path == Animation::Path::Translation ? "translation" : c.path == Animation::Path::Rotation ? "rotation" : c.path == Animation::Path::Scale ? "scale" : "weights";
					cv["target"] = std::move(target);
					channels.push(std::move(cv));
				}
				v["samplers"] = std::move(samplers);
				v["channels"] = std::move(channels);
				put_extras(v, a.extras);
				anims.push(std::move(v));
			}
			root["animations"] = std::move(anims);
		}
		align();
		json::Value buffers = json::Value::array(), buffer = json::Value::object();
		buffer["byteLength"] = int64_t(bin.size());
		if (!bin_uri.empty()) buffer["uri"] = bin_uri;
		buffers.push(std::move(buffer));
		if (!bin.empty()) {
			root["buffers"] = std::move(buffers);
			root["bufferViews"] = std::move(views);
			root["accessors"] = std::move(accessors);
		}
	}
};

void put_u32(std::vector<uint8_t> &b, uint32_t v) {
	for (int k = 0; k < 4; ++k) b.push_back(uint8_t(v >> (8 * k)));
}

void write_bytes(const std::filesystem::path &p, const std::vector<uint8_t> &b) {
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	if (!f) throw Error("cannot write " + p.string());
	f.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
	if (!f) throw Error("cannot write " + p.string());
}

}  // namespace

std::vector<uint8_t> write_glb(const Scene &scene) {
	Writer w(scene);
	w.build(true, "", nullptr);
	std::string text = w.root.dump();
	while (text.size() % 4) text += ' ';
	std::vector<uint8_t> out;
	put_u32(out, 0x46546C67);
	put_u32(out, 2);
	put_u32(out, uint32_t(12 + 8 + text.size() + (w.bin.empty() ? 0 : 8 + w.bin.size())));
	put_u32(out, uint32_t(text.size()));
	put_u32(out, 0x4E4F534A);
	out.insert(out.end(), text.begin(), text.end());
	if (!w.bin.empty()) {
		put_u32(out, uint32_t(w.bin.size()));
		put_u32(out, 0x004E4942);
		out.insert(out.end(), w.bin.begin(), w.bin.end());
	}
	return out;
}

void write(const Scene &scene, const std::filesystem::path &path) {
	std::string ext = path.extension().string();
	for (auto &c : ext) c = char(tolower(uint8_t(c)));
	if (ext == ".glb") {
		write_bytes(path, write_glb(scene));
		return;
	}
	Writer w(scene);
	std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
	std::string bin_name = path.stem().string() + ".bin";
	w.build(false, bin_name, &files);
	std::string text = w.root.dump();
	write_bytes(path, std::vector<uint8_t>(text.begin(), text.end()));
	if (!w.bin.empty()) write_bytes(path.parent_path() / bin_name, w.bin);
	for (auto &[name, bytes] : files) write_bytes(path.parent_path() / name, bytes);
}

}  // namespace gltf
