// The pilot editor's in-engine preview: a robot posed by its own animation bank and painted in
// the pilot's colours with the game's fighter filter, drawn into a texture.
#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "fighter_model.h"
#include "gl_util.h"

class RobotPreview {
public:
	~RobotPreview();
	// Loads <fighter>Model1.DEObjects and <fighter>.DEObjects from the Data folder.
	bool load(const std::filesystem::path &data, const std::string &fighter, std::string *error);
	const std::string &fighter() const { return fighter_; }
	const std::vector<std::string> &animations() const { return animations_; }
	// Draws at `width` x `height` (pixels) and returns the texture.
	GLuint render(int width, int height, const std::array<std::array<float, 3>, 3> &colours, int animation, float t, float yaw);
	// Where a bone's origin was in the last render (pixels from the top left) and how many pixels
	// a model unit there spans; false for a bone the rig does not have.
	bool bone_screen(const std::string &bone, float *x, float *y, float *pixels_per_unit) const;

private:
	void ensure_target(int w, int h);
	std::string fighter_;
	std::unique_ptr<de::ObjectsFile> model_file_, fighter_file_;
	fighter::Model model_;
	std::unique_ptr<fighter::Rig> rig_;
	std::vector<std::string> animations_;
	std::vector<std::array<float, 3>> pos_;
	std::vector<de::Transform> pose_;
	float mvp_[16] = {}, pixel_scale_ = 0;
	GLuint prog_ = 0, vao_ = 0, vbo_ = 0, skin_ = 0;
	GLuint fbo_ = 0, colour_ = 0, depth_ = 0;
	int tw_ = 0, th_ = 0;
};

// An RGBA image as a GL texture (linear filtering, clamped).
GLuint upload_texture(int width, int height, const std::vector<uint8_t> &rgba, GLuint reuse = 0);
void delete_texture(GLuint t);
