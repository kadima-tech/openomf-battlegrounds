#include "preview.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

RobotPreview::~RobotPreview() {
	if (vbo_) glDeleteBuffers(1, &vbo_);
	if (vao_) glDeleteVertexArrays(1, &vao_);
	if (skin_) glDeleteTextures(1, &skin_);
	if (static_) glDeleteTextures(1, &static_);
	if (colour_) glDeleteTextures(1, &colour_);
	if (depth_) glDeleteRenderbuffers(1, &depth_);
	if (fbo_) glDeleteFramebuffers(1, &fbo_);
}

bool RobotPreview::load(const std::filesystem::path &data, const std::string &name, std::string *error, const std::string &model_file, const std::string &model) {
	try {
		model_file_ = std::make_unique<de::ObjectsFile>(fighter::read_file(data / ((model_file.empty() ? name + "Model1" : model_file) + ".DEObjects")));
		model_ = fighter::load_fighter(*model_file_, model.empty() ? nullptr : model.c_str(), false);
		fighter_file_ = std::make_unique<de::ObjectsFile>(fighter::read_file(data / (name + ".DEObjects")));
		rig_ = fighter::load_rig(*fighter_file_, *model_file_, model_.mesh_index, false);
	} catch (const std::exception &e) {
		if (error) *error = e.what();
		rig_.reset();
		return false;
	}
	return finish(name + (model.empty() ? "" : ":" + model));
}

bool RobotPreview::load_addon(std::vector<uint8_t> bytes, uint32_t model, uint32_t robot, const std::string &key, std::string *error) {
	try {
		model_file_ = std::make_unique<de::ObjectsFile>(std::move(bytes));
		fighter_file_.reset();
		const std::string spec = "#" + std::to_string(model);
		model_ = fighter::load_fighter(*model_file_, spec.c_str(), false);
		rig_ = fighter::load_rig(*model_file_, *model_file_, model_.mesh_index, false, robot);
	} catch (const std::exception &e) {
		if (error) *error = e.what();
		rig_.reset();
		return false;
	}
	return finish(key);
}

bool RobotPreview::finish(const std::string &name) {
	fighter_ = name;
	animations_.clear();
	for (auto &s : rig_->bank.sequences) animations_.push_back(s.name);
	if (!prog_) {
		prog_ = glu::program(fighter::kVertexShader, fighter::kFragmentShader);
		glGenVertexArrays(1, &vao_);
		glBindVertexArray(vao_);
		glGenBuffers(1, &vbo_);
		glBindBuffer(GL_ARRAY_BUFFER, vbo_);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(fighter::Vertex), (void *)offsetof(fighter::Vertex, pos));
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(fighter::Vertex), (void *)offsetof(fighter::Vertex, normal));
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(fighter::Vertex), (void *)offsetof(fighter::Vertex, uv));
		for (GLuint i = 0; i < 3; ++i) glEnableVertexAttribArray(i);
		glBindVertexArray(0);
	}
	std::vector<uint8_t> grey{200, 200, 200, 255};
	bool has_skin = !model_.skin.channels.empty();
	skin_ = upload_texture(has_skin ? model_.skin.width : 1, has_skin ? model_.skin.height : 1, has_skin ? model_.skin.rgba() : grey, skin_);
	std::vector<uint8_t> black{0, 0, 0, 255};
	bool has_static = !model_.colours.channels.empty();
	static_ = upload_texture(has_static ? model_.colours.width : 1, has_static ? model_.colours.height : 1, has_static ? model_.colours.rgba() : black, static_);
	return true;
}

void RobotPreview::ensure_target(int w, int h) {
	if (fbo_ && w == tw_ && h == th_) return;
	if (!fbo_) {
		glGenFramebuffers(1, &fbo_);
		glGenTextures(1, &colour_);
		glGenRenderbuffers(1, &depth_);
	}
	tw_ = w, th_ = h;
	glBindTexture(GL_TEXTURE_2D, colour_);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindRenderbuffer(GL_RENDERBUFFER, depth_);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour_, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_);
}

GLuint RobotPreview::render(int w, int h, const std::array<std::array<float, 3>, 3> &colours, int animation, float t, float yaw) {
	if (!rig_ || w <= 0 || h <= 0) return 0;
	GLint prev_fbo = 0, vp[4];
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	glGetIntegerv(GL_VIEWPORT, vp);
	ensure_target(w, h);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
	glViewport(0, 0, w, h);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	const de::AnimSequence *seq = animation >= 0 && size_t(animation) < rig_->bank.sequences.size() ? &rig_->bank.sequences[size_t(animation)] : nullptr;
	fighter::skin_vertices(model_, rig_.get(), seq, t, pos_);
	pose_ = seq ? rig_->bank.pose(*seq, t) : std::vector<de::Transform>{};
	auto verts = fighter::build_vertices(model_, pos_);
	float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
	for (auto &p : model_.mesh.vertices) {
		for (int j = 0; j < 3; ++j) lo[j] = std::fmin(lo[j], p[j]), hi[j] = std::fmax(hi[j], p[j]);
	}
	float centre[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
	float height = hi[1] - lo[1];
	glBindVertexArray(vao_);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_);
	glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(fighter::Vertex)), verts.data(), GL_DYNAMIC_DRAW);
	float proj[16], view[16], mvp[16];
	const float fov = 32.0f, aspect = float(w) / float(h);
	float half = std::fmin(fov * 0.5f * 3.14159265f / 180.0f, std::atan(std::tan(fov * 0.5f * 3.14159265f / 180.0f) * aspect));
	float distance = model_.radius * 0.95f / std::sin(half) / zoom_;
	(void)height;
	glu::perspective(proj, fov, aspect, model_.radius * 0.05f, model_.radius * 20.0f);
	const float pitch = 0.10f;
	float eye[3] = {centre[0] + distance * std::sin(yaw) * std::cos(pitch), centre[1] + distance * std::sin(pitch),
	                centre[2] + distance * std::cos(yaw) * std::cos(pitch)};
	glu::fps_view(view, eye, -yaw, pitch);
	glu::mul(mvp, proj, view);
	memcpy(mvp_, mvp, sizeof mvp_);
	pixel_scale_ = float(h) * 0.5f / (distance * std::tan(fov * 0.5f * 3.14159265f / 180.0f));
	glUseProgram(prog_);
	glUniformMatrix4fv(glGetUniformLocation(prog_, "mvp"), 1, GL_FALSE, mvp);
	glUniform3f(glGetUniformLocation(prog_, "light"), 0.4f, 0.7f, 0.6f);
	glUniform1i(glGetUniformLocation(prog_, "skin"), 0);
	glUniform1i(glGetUniformLocation(prog_, "statics"), 1);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, static_);
	glActiveTexture(GL_TEXTURE0);
	float m[9];
	for (int k = 0; k < 3; ++k) {
		for (int j = 0; j < 3; ++j) m[k * 3 + j] = colours[size_t(k)][size_t(j)];
	}
	glUniformMatrix3fv(glGetUniformLocation(prog_, "pilot"), 1, GL_FALSE, m);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, skin_);
	glDrawArrays(GL_TRIANGLES, 0, GLsizei(verts.size()));
	glBindVertexArray(0);
	glUseProgram(0);
	glDisable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev_fbo));
	glViewport(vp[0], vp[1], vp[2], vp[3]);
	return colour_;
}

std::vector<uint8_t> RobotPreview::pixels(int *width, int *height) const {
	*width = tw_, *height = th_;
	std::vector<uint8_t> out(size_t(tw_) * size_t(th_) * 4);
	if (!fbo_ || out.empty()) return out;
	GLint prev = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, tw_, th_, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
	glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev));
	const size_t row = size_t(tw_) * 4;
	for (int y = 0; y < th_ / 2; ++y) std::swap_ranges(out.begin() + long(size_t(y) * row), out.begin() + long(size_t(y + 1) * row), out.begin() + long(size_t(th_ - 1 - y) * row));
	return out;
}

bool RobotPreview::bone_screen(const std::string &bone, float *x, float *y, float *pixels_per_unit) const {
	if (!rig_ || tw_ <= 0) return false;
	for (size_t b = 0; b < rig_->bones.size(); ++b) {
		if (rig_->bones[b].name != bone) continue;
		int c = rig_->bone_channel[b];
		std::array<float, 3> p = c >= 0 && size_t(c) < pose_.size() ? pose_[size_t(c)].t : rig_->bones[b].bind.t;
		float clip[4];
		for (int r = 0; r < 4; ++r) clip[r] = mvp_[r] * p[0] + mvp_[4 + r] * p[1] + mvp_[8 + r] * p[2] + mvp_[12 + r];
		if (clip[3] <= 0) return false;
		*x = (clip[0] / clip[3] * 0.5f + 0.5f) * float(tw_);
		*y = (0.5f - clip[1] / clip[3] * 0.5f) * float(th_);
		*pixels_per_unit = pixel_scale_;
		return true;
	}
	return false;
}

GLuint upload_texture(int w, int h, const std::vector<uint8_t> &rgba, GLuint reuse) {
	GLuint t = reuse;
	if (!t) glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

void delete_texture(GLuint t) {
	if (t) glDeleteTextures(1, &t);
}
