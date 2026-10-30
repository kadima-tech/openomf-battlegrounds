#include "de/value.h"

#include <algorithm>
#include <cmath>

#include "de/contents.h"

namespace de {

namespace {

// Contents of a plain DE_CValueFLOAT / DE_CValueRGBA: [object chunk][v1 value].
void skip_object_chunk(Contents &c) {
	Chunk ch = begin_chunk(c);
	c.seek(ch.end);
}

float read_float_chunk(Contents &c) {
	Chunk ch = begin_chunk(c);
	float v = ch.version >= 1 && ch.end - c.pos() >= 4 ? c.f32() : 0;
	c.seek(ch.end);
	return v;
}

std::array<float, 4> read_rgba_chunk(Contents &c) {
	Chunk ch = begin_chunk(c);
	std::array<float, 4> v{1, 1, 1, 1};
	if (ch.version >= 1 && ch.end - c.pos() >= 16) {
		for (auto &x : v) x = c.f32();
	}
	c.seek(ch.end);
	return v;
}

// DE_CInterpolationType::ObjProcessContents: v2 reverse; v3 reverse, mirror, peak. Then the
// subclass chunk: Gamma/Bias/Gain v1 parameter; Spline v1 count + values.
void read_interpolation(Contents &c, Interpolation &s) {
	Chunk base = begin_chunk(c);
	if (base.version >= 2) s.reverse = c.boolean();
	if (base.version >= 3) {
		s.mirror = c.boolean();
		s.peak = c.f32();
	}
	c.seek(base.end);
	bool param = s.type == "Gamma Type" || s.type == "Bias Type" || s.type == "Gain Type";
	bool spline = s.type == "Spline Type";
	if (!param && !spline) return;
	Chunk ch = begin_chunk(c);
	if (ch.version == 1) {
		if (param) {
			s.param = c.f32();
		} else {
			uint32_t n = c.u32();
			for (uint32_t i = 0; i < n && c.pos() + 4 <= ch.end; ++i) s.points.push_back(c.f32());
		}
	}
	c.seek(ch.end);
}

float clamp01(float t) { return std::clamp(t, 0.0f, 1.0f); }

float bias(float t, float b) { return std::pow(t, std::log(b) / std::log(0.5f)); }

}  // namespace

float Interpolation::value(float a, float b, float t) const {
	if (mirror) t = t < peak ? t / peak : 1 - (t - peak) / (1 - peak);
	if (reverse) t = 1 - t;
	t = clamp01(t);
	if (type == "SmoothStep Type") {
		t = (3 - 2 * t) * t * t;
	} else if (type == "Sin Type") {
		t = std::sin(t * 1.5707964f);
	} else if (type == "Quarter Circle Type") {
		t = std::sin(std::acos(t));
	} else if (type == "Gamma Type") {
		t = std::pow(t, 1 / param);
	} else if (type == "Bias Type") {
		t = bias(t, param);
	} else if (type == "Gain Type") {
		// DE_CGainType::GetValue: Perlin's gain, bias(1 - g) on each half.
		t = t < 0.5f ? bias(2 * t, 1 - param) / 2 : 1 - bias(2 - 2 * t, 1 - param) / 2;
	} else if (type == "Spline Type") {
		// DE_CSplineType::GetValue: Catmull-Rom through the control values, n - 3 segments.
		int segments = int(points.size()) - 3;
		if (segments > 0) {
			float x = t * float(segments);
			int i = std::min(int(x), segments - 1);
			x -= float(i);
			const float *p = &points[size_t(i)];
			float c3 = 1.5f * p[1] + 0.5f * p[3] - 1.5f * p[2] - 0.5f * p[0];
			float c2 = 2 * p[2] - 0.5f * p[3] - 2.5f * p[1] + p[0];
			float c1 = 0.5f * p[2] - 0.5f * p[0];
			t = clamp01(((c3 * x + c2) * x + c1) * x + p[1]);
		}
	} else if (type == "Coth Type") {
		// DE_CCothType::GetValue (Core 0x7010a760): the series of coth(u) sin(u) cos(u), u = 2t - 1,
		// a bump peaking at 1 mid-way.
		float x = (2 * t - 1) * (2 * t - 1);
		t = clamp01(1 - 0.3333333f * x - 0.1111111f * x * x + 0.0486773f * x * x * x - 0.0081129f * x * x * x * x);
	}
	return (b - a) * t + a;
}

float FloatValue::eval(float t, const LiveFloat &live) const {
	float own = value;
	if (live && object) {
		if (auto v = live(object)) own = *v;
	}
	if (op >= 0) {
		auto at = [&](int i, float x) { return in[i] ? in[i]->eval(x, live) : own; };
		float a = at(0, t), b = at(1, t);
		switch (op) {
		case 0: return b + a;
		case 1: return a - b;
		case 2: return b == 0 ? 1 : a / b;
		case 3: return b * a;
		case 4: return in[0] ? in[0]->eval(b, live) : 0;
		case 5: {
			float r = 1;
			for (auto &f : in) r *= f ? f->eval(t, live) : 1;
			return r;
		}
		default: return 0;
		}
	}
	if (!inter) return own;
	if (frequency != 1) t = t * frequency - float(int(t * frequency));
	return shape.value(min, max, t);
}

std::array<float, 4> ColorValue::eval(float t) const {
	if (!color) return rgba;
	auto v = color->eval(t);
	if (alpha) v[3] *= alpha->eval(t);
	return v;
}

FloatValue read_embedded_float(Contents &c) {
	FloatValue v;
	Chunk ch = begin_chunk(c);
	if (ch.version == 1 && c.pos() < ch.end) {
		skip_object_chunk(c);
		if (c.pos() < ch.end) v.value = read_float_chunk(c);
	}
	c.seek(ch.end);
	return v;
}

ColorValue read_embedded_color(Contents &c) {
	ColorValue v;
	Chunk ch = begin_chunk(c);
	if (ch.version == 1 && c.pos() < ch.end) {
		skip_object_chunk(c);
		if (c.pos() < ch.end) v.rgba = read_rgba_chunk(c);
	}
	c.seek(ch.end);
	return v;
}

ObjectRef read_advanced_reference(const World &w, const ObjectsFile &from, Contents &c) {
	ObjectRef out;
	Chunk ch = begin_chunk(c);
	if (ch.version == 1 && c.pos() < ch.end && c.boolean()) out = w.resolve(from, Reference::read(c));
	c.seek(ch.end);
	return out;
}

namespace {

// Contents [v1: u32 op]; references, the last chunk [v1: three { BOOL, reference } inputs].
void read_math_function(const World &w, const ObjectRef &r, Contents &c, FloatValue &v, int depth) {
	Chunk ch = begin_chunk(c);
	if (ch.version == 1 && ch.end - c.pos() >= 4) v.op = int(c.u32());
	c.seek(ch.end);
	Contents rc(r.file->object_data(r.info()).references);
	size_t last = 0;
	while (rc.remaining() > 1) {
		last = rc.pos();
		Chunk skip = begin_chunk(rc);
		rc.seek(skip.end);
	}
	rc.seek(last);
	Chunk m = begin_chunk(rc);
	if (m.version != 1) return;
	for (auto &in : v.in) {
		if (rc.pos() >= m.end || !rc.boolean()) continue;
		ObjectRef ir = w.resolve(*r.file, Reference::read(rc));
		if (ir && depth < 16) in = std::make_shared<FloatValue>(read_float_value(w, ir, depth + 1));
	}
}

}  // namespace

FloatValue read_float_value(const World &w, const ObjectRef &r, int depth) {
	FloatValue v;
	if (!r) return v;
	const std::string &cls = r.info().class_path;
	if (cls.rfind("Value\\FLOAT", 0) != 0) return v;
	v.object = r;
	try {
		Contents c(r.file->object_data(r.info()).contents);
		skip_object_chunk(c);
		v.value = read_float_chunk(c);
		if (cls == "Value\\FLOAT\\MathFunction") read_math_function(w, r, c, v, depth);
		if (cls == "Value\\FLOAT\\Inter" && c.remaining() > 1) {
			Chunk ch = begin_chunk(c);
			if (ch.version >= 2) {
				v.inter = true;
				v.min = c.f32();
				v.max = c.f32();
				v.shape.type = c.string();
				read_interpolation(c, v.shape);
				if (ch.version == 3) c.boolean();
				if (ch.version >= 4) v.frequency = c.f32();
			}
			c.seek(ch.end);
		}
	} catch (const std::exception &) {
	}
	return v;
}

ColorValue read_color_value(const World &w, const ObjectRef &r) {
	ColorValue v;
	if (!r) return v;
	const std::string &cls = r.info().class_path;
	if (cls.rfind("Value\\RGBA", 0) != 0) return v;
	try {
		auto d = r.file->object_data(r.info());
		Contents c(d.contents);
		skip_object_chunk(c);
		v.rgba = read_rgba_chunk(c);
		if (cls == "Value\\RGBA\\ModulatedRGBA" && c.remaining() > 1) {
			Chunk ch = begin_chunk(c);
			if (ch.version == 1) {
				auto color = std::make_shared<ColorValue>(read_embedded_color(c));
				auto alpha = std::make_shared<FloatValue>(read_embedded_float(c));
				// References: one chunk per class, the modulated one last (colour ref, float ref).
				Contents rc(d.references);
				size_t last = 0;
				while (rc.remaining() > 1) {
					last = rc.pos();
					Chunk skip = begin_chunk(rc);
					rc.seek(skip.end);
				}
				rc.seek(last);
				if (rc.remaining() > 1) {
					Chunk m = begin_chunk(rc);
					if (m.version == 1 && m.end > rc.pos()) {
						if (ObjectRef cr = read_advanced_reference(w, *r.file, rc)) {
							*color = read_color_value(w, cr);
						}
						if (ObjectRef fr = read_advanced_reference(w, *r.file, rc)) {
							*alpha = read_float_value(w, fr);
						}
					}
				}
				v.color = color;
				v.alpha = alpha;
			}
			c.seek(ch.end);
		}
	} catch (const std::exception &) {
	}
	return v;
}

std::optional<float> child_float(const World &w, const ObjectRef &parent, const std::string &name) {
	if (!parent) return std::nullopt;
	for (uint32_t c : ObjectsFile::children(parent.file->object_data(parent.info()))) {
		const ObjectInfo &o = parent.file->objects()[c];
		if (o.name == name && o.class_path.rfind("Value\\FLOAT", 0) == 0) return read_float_value(w, {parent.file, c}).eval(0);
	}
	return std::nullopt;
}

}  // namespace de
