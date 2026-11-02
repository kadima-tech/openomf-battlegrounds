// Opening the apps' SDL2 window with its OpenGL 4.1 core context.
#pragma once

#include <SDL.h>

#include <stdexcept>
#include <string>

namespace glu {

// Callers set the context attributes (4.1 core, depth, 4x multisampling) first.
// Displays without a multisampled visual (Linux VMs, remote X, Xvfb, some drivers)
// get a window without multisampling instead of none at all.
inline SDL_Window *create_gl_window(const char *title, int x, int y, int w, int h, Uint32 flags, SDL_GLContext *ctx) {
	for (int attempt = 0; attempt < 2; ++attempt) {
		if (attempt) {
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
		}
		SDL_Window *win = SDL_CreateWindow(title, x, y, w, h, flags);
		if (!win) continue;
		if ((*ctx = SDL_GL_CreateContext(win))) return win;
		SDL_DestroyWindow(win);
	}
	throw std::runtime_error(std::string("could not open an OpenGL 4.1 core window: ") + SDL_GetError());
}

}  // namespace glu
