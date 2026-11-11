// openbg-editor: the DEObjects content editor (docs/EDITOR_SPEC.md).
//
//   openbg-editor [--data <Data dir>] [<add-on.DEObjects>]
//                 [--new-pilot Base:Name] [--open-pilot Name] [--tab 0-4] [--preview photo|robot]
//                 [--photo image.png] [--content <type>] [--base-content] [--browse-file <file stem>]
//                 [--open-base Class:Name] [--select-base Class:Name] [--copy-base <pilot>]
//                 [--select <object name>] [--level] [--save] [--launch-game]
//                 [--arena <name or stem>] [--variant] [--select-node <name>] [--camera x,y,z,yaw,pitch]
//                 [--layout 1|2|4] [--shading 0|1|2] [--move-to x,y,z] [--light-colour r,g,b,i]
//                 [--play | --simulate] [--play-ticks N] [--eject] [--inspector] [--debug hits,nav,energy]
//                 [--screenshot out.png [--frames N] [--size WxH] [--scale S]]
//
// The add-on is created when it does not exist. --screenshot draws N frames (default 8) in a hidden
// window at WxH points (1600x1000) and S pixels per point (2), writes the last one and quits.
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "de/platform.h"
#include "editor.h"
#include "gl_util.h"
#include "gl_window.h"
#include "image_io.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "theme.h"

static std::filesystem::path utf8_path(const std::string &s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

int main(int argc, char **argv) {
	de::init_platform();
	Options opt;
	std::string shot;
	int frames = 8, shot_w = 1600, shot_h = 1000;
	float shot_scale = 2;
	for (int i = 1; i < argc; ++i) {
		auto arg = [&](const char *name) { return !strcmp(argv[i], name) && i + 1 < argc; };
		if (arg("--data")) opt.data = utf8_path(argv[++i]);
		else if (arg("--new-pilot")) opt.new_pilot = argv[++i];
		else if (arg("--open-pilot")) opt.open_pilot = argv[++i];
		else if (arg("--tab")) opt.pilot_tab = atoi(argv[++i]);
		else if (arg("--preview")) opt.preview = argv[++i];
		else if (arg("--photo")) opt.photo = utf8_path(argv[++i]);
		else if (arg("--content")) opt.content = argv[++i];
		else if (!strcmp(argv[i], "--base-content")) opt.base_content = true;
		else if (arg("--browse-file")) opt.browse_file = argv[++i];
		else if (arg("--open-base")) opt.open_base = argv[++i];
		else if (arg("--select-base")) opt.select_base = argv[++i];
		else if (arg("--copy-base")) opt.copy_base = argv[++i];
		else if (!strcmp(argv[i], "--level")) opt.level = true;
		else if (!strcmp(argv[i], "--save")) opt.save = true;
		else if (!strcmp(argv[i], "--launch-game")) opt.launch = true;
		else if (arg("--select")) opt.select = argv[++i];
		else if (arg("--new-tournament")) opt.new_tournament = argv[++i];
		else if (arg("--open-tournament")) opt.open_tournament = argv[++i];
		else if (arg("--new-har")) opt.new_har = argv[++i];
		else if (arg("--open-har")) opt.open_har = argv[++i];
		else if (arg("--har-constant")) opt.har_constants.push_back(argv[++i]);
		else if (arg("--asset-tab")) opt.asset_tab = atoi(argv[++i]);
		else if (arg("--asset-select")) opt.asset_select = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--compile")) opt.compile = true;
		else if (arg("--arena")) opt.arena = argv[++i];
		else if (!strcmp(argv[i], "--variant")) opt.variant = true;
		else if (arg("--select-node")) opt.select_node = argv[++i];
		else if (arg("--camera")) opt.camera = argv[++i];
		else if (arg("--layout")) opt.layout = atoi(argv[++i]);
		else if (arg("--shading")) opt.shading = atoi(argv[++i]);
		else if (arg("--move-to")) opt.move_to = argv[++i];
		else if (arg("--light-colour")) opt.light_colour = argv[++i];
		else if (!strcmp(argv[i], "--play")) opt.play = true;
		else if (!strcmp(argv[i], "--simulate")) opt.play = opt.simulate = true;
		else if (!strcmp(argv[i], "--eject")) opt.eject = true;
		else if (!strcmp(argv[i], "--replay")) opt.replay = true;
		else if (!strcmp(argv[i], "--inspector")) opt.inspector = true;
		else if (arg("--play-ticks")) opt.play_ticks = atoi(argv[++i]);
		else if (arg("--debug")) opt.debug = argv[++i];
		else if (arg("--screenshot")) shot = argv[++i];
		else if (arg("--frames")) frames = atoi(argv[++i]);
		else if (arg("--size")) sscanf(argv[++i], "%dx%d", &shot_w, &shot_h);
		else if (arg("--scale")) shot_scale = float(atof(argv[++i]));
		else if (argv[i][0] != '-') opt.project = utf8_path(argv[i]);
		else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}
	opt.screenshot = !shot.empty();

#ifdef SDL_HINT_IME_SHOW_UI
	SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	int w = 1600, h = 1000;
	SDL_Rect usable;
	if (!opt.screenshot && SDL_GetDisplayUsableBounds(0, &usable) == 0) {
		w = std::min(w, usable.w * 9 / 10);
		h = std::min(h, usable.h * 9 / 10);
	}
	SDL_GLContext ctx = nullptr;
	SDL_Window *window = nullptr;
	try {
		window = glu::create_gl_window("DEObjects Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, opt.screenshot ? shot_w : w,
		                               opt.screenshot ? shot_h : h,
		                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
		                                   (opt.screenshot ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN),
		                               &ctx);
	} catch (const std::exception &ex) {
		fprintf(stderr, "%s\n", ex.what());
		return 1;
	}
	SDL_GL_MakeCurrent(window, ctx);
	SDL_GL_SetSwapInterval(1);
	if (!glu::load_gl()) {
		fprintf(stderr, "OpenGL 4.1 is required; update your graphics driver\n");
		return 1;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
	if (!opt.screenshot) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	io.ConfigWindowsMoveFromTitleBarOnly = true;
	io.ConfigDockingTransparentPayload = true;
	const float scale = opt.screenshot ? 1.0f : ImGui_ImplSDL2_GetContentScaleForDisplay(0);
	theme::load_fonts();
	theme::apply_style(scale > 0 ? scale : 1.0f);
	ImGui_ImplSDL2_InitForOpenGL(window, ctx);
	ImGui_ImplOpenGL3_Init("#version 410 core");

	int status = 0;
	{
		Editor editor(opt, window);
		GLuint fbo = 0;
		const int fw = int(float(shot_w) * shot_scale), fh = int(float(shot_h) * shot_scale);
		if (opt.screenshot) fbo = glu::offscreen(fw, fh);
		for (int frame = 0;; ++frame) {
			SDL_Event e;
			while (SDL_PollEvent(&e)) {
				ImGui_ImplSDL2_ProcessEvent(&e);
				if (e.type == SDL_QUIT) editor.request_quit();
				if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE && e.window.windowID == SDL_GetWindowID(window)) {
					editor.request_quit();
				}
			}
			if (editor.quit_requested()) break;
			if (!opt.screenshot && (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
				SDL_Delay(30);
				continue;
			}
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplSDL2_NewFrame();
			if (opt.screenshot) {
				io.DisplaySize = ImVec2(float(shot_w), float(shot_h));
				io.DisplayFramebufferScale = ImVec2(shot_scale, shot_scale);
				io.DeltaTime = 1.0f / 60.0f;
				io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
			}
			ImGui::NewFrame();
			editor.frame();
			ImGui::Render();
			if (opt.screenshot) {
				glBindFramebuffer(GL_FRAMEBUFFER, fbo);
				glViewport(0, 0, fw, fh);
			} else {
				int dw, dh;
				SDL_GL_GetDrawableSize(window, &dw, &dh);
				glViewport(0, 0, dw, dh);
			}
			glClearColor(0.098f, 0.102f, 0.129f, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
				SDL_Window *w0 = SDL_GL_GetCurrentWindow();
				SDL_GLContext c0 = SDL_GL_GetCurrentContext();
				ImGui::UpdatePlatformWindows();
				ImGui::RenderPlatformWindowsDefault();
				SDL_GL_MakeCurrent(w0, c0);
			}
			if (opt.screenshot) {
				if (frame + 1 >= frames) {
					std::vector<uint8_t> px(size_t(fw) * size_t(fh) * 4), flipped(px.size());
					glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
					for (int y = 0; y < fh; ++y) {
						memcpy(flipped.data() + size_t(y) * size_t(fw) * 4, px.data() + size_t(fh - 1 - y) * size_t(fw) * 4, size_t(fw) * 4);
					}
					for (size_t k = 3; k < flipped.size(); k += 4) flipped[k] = 255;
					try {
						ed::write_png(shot, fw, fh, flipped);
						printf("wrote %s\n", shot.c_str());
					} catch (const std::exception &ex) {
						fprintf(stderr, "%s\n", ex.what());
						status = 1;
					}
					break;
				}
			} else {
				SDL_GL_SwapWindow(window);
			}
		}
	}
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	SDL_GL_DeleteContext(ctx);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return status;
}
