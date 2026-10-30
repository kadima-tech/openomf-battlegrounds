// Value objects that particles, renderers and scripts evaluate (Core DE_CValueFLOAT,
// DE_CValueInterFLOAT 0x7010d1a0, DE_CValueRGBA, DE_CValueModulatedRGBA 0x701117d0) and the
// "advanced references" that embed a default value or point at a Value object
// (DE_CValueFLOAT::CAdvancedReference 0x7010eee0).
//
// Contents: [object chunk: name][FLOAT v1: value | RGBA v1: r g b a][subclass chunk].
//  - Value\FLOAT\Inter v4: min, max, interpolation type name (DE_CInterpolationType::New),
//    the type's contents ([v3: reverse, mirror, peak][type chunk]), frequency.
//    GetValue(t): t = frac(t * frequency) unless frequency is 1, then type(min, max, t).
//  - Value\RGBA\ModulatedRGBA v1: an RGBA reference and a FLOAT reference (their embedded
//    defaults in the contents, the referenced objects in the references); GetValue(t) is the
//    colour at t with alpha scaled by the float at t.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "de/scene.h"

namespace de {

// DE_CInterpolationType and subclasses (Lerp, SmoothStep, Gamma, Bias, Gain, Spline, Quarter
// Circle, Sin, Coth). AdjustT: with `mirror` t rises to 1 at `peak` and falls back; `reverse`
// flips it; then clamped to 0..1.
struct Interpolation {
	std::string type = "Lerp Type";
	bool reverse = false, mirror = false;
	float peak = 0.5f;
	float param = 0.5f;        // Gamma / Bias / Gain (+0x18)
	std::vector<float> points;  // Spline: Catmull-Rom control values
	float value(float a, float b, float t) const;
};

struct FloatValue {
	ObjectRef object;
	bool inter = false;
	float value = 0;  // DE_CValueFLOAT
	float min = 0, max = 1, frequency = 1;
	Interpolation shape;
	// Value\FLOAT\MathFunction (DE_CValueFLOATMathFunction::GetValue, Core 0x70110770): op +0x54
	// (0 add, 1 a - b, 2 a / b, 3 multiply, 4 in[0] at b, 5 product), a = in[0](t), b = in[1](t),
	// each the function's own value when unset.
	int op = -1;
	std::shared_ptr<FloatValue> in[3];
	float eval(float t, const LiveFloat &live = {}) const;
};

struct ColorValue {
	std::array<float, 4> rgba{1, 1, 1, 1};
	// ModulatedRGBA: colour and alpha modulator.
	std::shared_ptr<ColorValue> color;
	std::shared_ptr<FloatValue> alpha;
	std::array<float, 4> eval(float t) const;
};

// A Value object (by class path).
FloatValue read_float_value(const World &w, const ObjectRef &r, int depth = 0);
ColorValue read_color_value(const World &w, const ObjectRef &r);

// DE_CObject::GetFLOATValue(name) for a value kept as a named child (GlowFactor on meshes and
// particle renderers, SpecularFactor on texture managers): the child Value\FLOAT's value at 0.
std::optional<float> child_float(const World &w, const ObjectRef &parent, const std::string &name);

// CAdvancedReference contents: chunk v1 holding the embedded default value's contents.
FloatValue read_embedded_float(Contents &c);
ColorValue read_embedded_color(Contents &c);
// CAdvancedReference references: chunk v1 { BOOL, reference }; null when absent.
ObjectRef read_advanced_reference(const World &w, const ObjectsFile &from, Contents &c);

}  // namespace de
