// Battlegrounds launcher for Linux: the GTK 3 counterpart of Launcher.swift.
//
// Same flow and files as the macOS app: a verified installation starts the game
// directly; otherwise first-time setup asks for an ISO or the game CD, requires
// confirmation, and runs the bundled worker/bootstrap.sh/install.py. The startup
// readiness check is readiness.py on the app's private Python, sharing
// verified-files.json with install.py. GTK, GLib and Cairo come from the desktop.
// GTK 3.24 and GLib 2.72 (Ubuntu 22.04) or newer; later GLib deprecations stay quiet.
#define GLIB_VERSION_MIN_REQUIRED GLIB_VERSION_2_72
#define GLIB_VERSION_MAX_ALLOWED GLIB_VERSION_2_72
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <glib-unix.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PRODUCT_GUID "b8ef8b5a-6d15-436c-ac07-5f2bd5e1aa1b"

// ---------------------------------------------------------------------------- model

typedef struct {
	const char *title;
	char *detail;
	const char *status;  // "waiting", "active" or "done"
} Stage;

typedef struct {
	char *resources, *app_dir, *project, *home;
	gboolean preview;
	Stage stages[3];
	gboolean busy, ready;
	char *error, *disc, *pending_source, *source_name, *last_source, *ready_game;
	GSubprocess *worker, *game;
	void (*changed)(void);
	void (*needs_window)(void);
	void (*hide_window)(void);
	void (*quit)(void);
} Model;

static Model M;

static void noop(void) {}

static void set_string(char **field, const char *value) {
	char *copy = g_strdup(value);
	g_free(*field);
	*field = copy;
}

static void set_error(const char *message) {
	set_string(&M.error, message);
	M.changed();
}

// A direct child whose name matches case-insensitively, as on the original disc.
static char *child_named(const char *root, const char *name) {
	GDir *dir = g_dir_open(root, 0, NULL);
	if (!dir) return NULL;
	char *found = NULL;
	const char *entry;
	while (!found && (entry = g_dir_read_name(dir)))
		if (g_ascii_strcasecmp(entry, name) == 0) found = g_build_filename(root, entry, NULL);
	g_dir_close(dir);
	return found;
}

static gboolean setup_on_disc(const char *root) {
	char *setup = child_named(root, "Setup");
	if (!setup) return FALSE;
	gboolean ok = FALSE;
	char *ini = child_named(setup, "setup.ini"), *text = NULL;
	if (ini && g_file_get_contents(ini, &text, NULL, NULL)) {
		char *lower = g_ascii_strdown(text, -1);
		ok = strstr(lower, PRODUCT_GUID) != NULL;
		g_free(lower);
		const char *parts[] = {"data1.cab", "data1.hdr", "data2.cab"};
		for (int i = 0; ok && i < 3; i++) {
			char *part = child_named(setup, parts[i]);
			ok = part != NULL;
			g_free(part);
		}
	}
	g_free(text);
	g_free(ini);
	g_free(setup);
	return ok;
}

// Disc file systems anywhere, and anything under the removable-media roots. Network
// shares and system mounts are never opened, so a slow server cannot stall detection.
static gboolean candidate_mount(const char *path, const char *fs_type) {
	if (fs_type && (strcmp(fs_type, "iso9660") == 0 || strcmp(fs_type, "udf") == 0)) return TRUE;
	return g_str_has_prefix(path, "/media/") || g_str_has_prefix(path, "/run/media/") ||
	       g_str_has_prefix(path, "/mnt/");
}

static GPtrArray *mounted_volumes(void) {
	GPtrArray *volumes = g_ptr_array_new_with_free_func(g_free);
	GList *mounts = g_unix_mounts_get(NULL);
	for (GList *item = mounts; item; item = item->next) {
		GUnixMountEntry *mount = item->data;
		const char *path = g_unix_mount_get_mount_path(mount);
		if (candidate_mount(path, g_unix_mount_get_fs_type(mount))) g_ptr_array_add(volumes, g_strdup(path));
	}
	g_list_free_full(mounts, (GDestroyNotify)g_unix_mount_free);
	return volumes;
}

static void scan_volumes(GPtrArray *volumes) {
	if (M.preview || M.busy || M.ready) return;
	const char *found = NULL;
	for (guint i = 0; !found && i < volumes->len; i++)
		if (setup_on_disc(volumes->pdata[i])) found = volumes->pdata[i];
	set_string(&M.disc, found);
	M.changed();
}

static void scan_discs(void) {
	if (M.preview || M.busy || M.ready) return;
	GPtrArray *volumes = mounted_volumes();
	scan_volumes(volumes);
	g_ptr_array_unref(volumes);
}

static gboolean has_iso_suffix(const char *path) {
	size_t n = strlen(path);
	return n >= 4 && g_ascii_strcasecmp(path + n - 4, ".iso") == 0;
}

static void request_install(const char *source) {
	if (M.busy || M.preview || M.ready) return;
	if (!g_file_test(source, G_FILE_TEST_IS_DIR) && !has_iso_suffix(source)) {
		set_error("Choose an ISO disc image, or insert your original Battlegrounds CD.");
		return;
	}
	set_string(&M.pending_source, source);
	M.changed();
}

static void launch(void);

// Event lines are flat JSON objects of strings, written by install.py/bootstrap.sh.
static const char *parse_json_string(const char *p, GString *out) {
	if (*p++ != '"') return NULL;
	while (*p && *p != '"') {
		if (*p != '\\') {
			g_string_append_c(out, *p++);
			continue;
		}
		p++;
		switch (*p) {
		case '"': case '\\': case '/': g_string_append_c(out, *p); break;
		case 'b': g_string_append_c(out, '\b'); break;
		case 'f': g_string_append_c(out, '\f'); break;
		case 'n': g_string_append_c(out, '\n'); break;
		case 'r': g_string_append_c(out, '\r'); break;
		case 't': g_string_append_c(out, '\t'); break;
		case 'u': {
			gunichar c = 0;
			for (int i = 1; i <= 4; i++) {
				if (!g_ascii_isxdigit(p[i])) return NULL;
				c = c * 16 + g_ascii_xdigit_value(p[i]);
			}
			p += 4;
			if (c >= 0xD800 && c < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
				gunichar low = 0;
				for (int i = 3; i <= 6; i++) {
					if (!g_ascii_isxdigit(p[i])) return NULL;
					low = low * 16 + g_ascii_xdigit_value(p[i]);
				}
				if (low >= 0xDC00 && low < 0xE000) {
					c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
					p += 6;
				}
			}
			g_string_append_unichar(out, g_unichar_validate(c) ? c : 0xFFFD);
			break;
		}
		default: return NULL;
		}
		p++;
	}
	return *p == '"' ? p + 1 : NULL;
}

// Returns a table of the object's string members, or NULL if the line is not one.
static GHashTable *parse_event(const char *line) {
	GHashTable *event = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	const char *p = line;
	while (g_ascii_isspace(*p)) p++;
	if (*p++ != '{') goto fail;
	while (g_ascii_isspace(*p)) p++;
	if (*p == '}') return event;
	for (;;) {
		GString *key = g_string_new(NULL), *value = g_string_new(NULL);
		while (g_ascii_isspace(*p)) p++;
		p = parse_json_string(p, key);
		if (p) {
			while (g_ascii_isspace(*p)) p++;
			p = *p == ':' ? p + 1 : NULL;
		}
		if (p) {
			while (g_ascii_isspace(*p)) p++;
			p = parse_json_string(p, value);
		}
		if (!p) {
			g_string_free(key, TRUE);
			g_string_free(value, TRUE);
			goto fail;
		}
		g_hash_table_replace(event, g_string_free(key, FALSE), g_string_free(value, FALSE));
		while (g_ascii_isspace(*p)) p++;
		if (*p == ',') { p++; continue; }
		if (*p == '}') return event;
		goto fail;
	}
fail:
	g_hash_table_unref(event);
	return NULL;
}

static void receive(GHashTable *event) {
	const char *stage = g_hash_table_lookup(event, "stage");
	if (!stage) return;
	if (strcmp(stage, "error") == 0) {
		set_error(g_hash_table_lookup(event, "detail"));
		return;
	}
	if (strcmp(stage, "complete") == 0) {
		M.ready = TRUE;
		M.busy = FALSE;
		g_free(M.ready_game);
		M.ready_game = g_build_filename(M.home, "game", NULL);
		set_string(&M.source_name, "Installation complete. Your next launch goes straight to the game.");
		M.changed();
		launch();
		return;
	}
	static const char *names[] = {"files", "patches", "runtime"};
	for (int i = 0; i < 3; i++) {
		if (strcmp(stage, names[i]) != 0) continue;
		const char *status = g_hash_table_lookup(event, "status");
		M.stages[i].status = status && strcmp(status, "done") == 0 ? "done"
		                   : status && strcmp(status, "active") == 0 ? "active" : "waiting";
		const char *detail = g_hash_table_lookup(event, "detail");
		set_string(&M.stages[i].detail, detail ? detail : "");
		M.changed();
	}
}

static void worker_finished(GObject *source, GAsyncResult *result, gpointer data) {
	(void)data;
	g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
	if (M.worker == G_SUBPROCESS(source)) g_clear_object(&M.worker);
	g_object_unref(source);
	M.busy = FALSE;
	if (!M.ready && !M.error)
		set_string(&M.error, "Setup stopped before it finished. Check your disc and free space, then retry. "
		                     "Details are in the setup log.");
	M.changed();
}

static void read_event(GObject *stream, GAsyncResult *result, gpointer worker) {
	char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(stream), result, NULL, NULL);
	if (!line) {
		g_object_unref(stream);
		g_subprocess_wait_async(worker, NULL, worker_finished, NULL);
		return;
	}
	GHashTable *event = parse_event(line);
	if (event) {
		receive(event);
		g_hash_table_unref(event);
	}
	g_free(line);
	g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(stream), G_PRIORITY_DEFAULT, NULL, read_event, worker);
}

static void install(const char *source) {
	if (M.busy || M.preview || M.ready) return;
	char *chosen = g_strdup(source);
	set_string(&M.last_source, chosen);
	char *name = g_path_get_basename(chosen);
	set_string(&M.source_name, name);
	g_free(name);
	g_clear_pointer(&M.error, g_free);
	M.busy = TRUE;
	for (int i = 0; i < 3; i++) M.stages[i].status = "waiting";
	g_mkdir_with_parents(M.home, 0755);
	char *worker = g_build_filename(M.resources, "bin/worker", NULL);
	char *bootstrap = g_build_filename(M.resources, "bootstrap.sh", NULL);
	const char *argv[] = {worker, bootstrap, M.home, "--source", chosen, M.project ? "--project" : NULL, M.project, NULL};
	GError *error = NULL;
	GSubprocess *process = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error);
	g_free(worker);
	g_free(bootstrap);
	g_free(chosen);
	if (!process) {
		M.busy = FALSE;
		char *message = g_strdup_printf("Setup could not start: %s", error->message);
		set_error(message);
		g_free(message);
		g_error_free(error);
		return;
	}
	M.worker = g_object_ref(process);  // The read loop owns the other reference.
	GDataInputStream *lines = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
	g_data_input_stream_read_line_async(lines, G_PRIORITY_DEFAULT, NULL, read_event, process);
	M.changed();
}

static void confirm_install(void) {
	char *source = M.pending_source;
	M.pending_source = NULL;
	if (source) install(source);
	g_free(source);
	M.changed();
}

static void cancel_pending(void) {
	g_clear_pointer(&M.pending_source, g_free);
	M.changed();
}

static void game_finished(GObject *source, GAsyncResult *result, gpointer data) {
	(void)data;
	GSubprocess *process = G_SUBPROCESS(source);
	g_subprocess_wait_finish(process, result, NULL);
	int code = g_subprocess_get_if_exited(process) ? g_subprocess_get_exit_status(process)
	                                               : g_subprocess_get_term_sig(process);
	g_clear_object(&M.game);
	if (code == 0 && g_subprocess_get_if_exited(process)) {
		g_object_unref(process);
		M.quit();
		return;
	}
	g_object_unref(process);
	M.needs_window();
	char *message = g_strdup_printf("The game could not start (code %d). Open the game log for details.", code);
	set_error(message);
	g_free(message);
}

static void launch(void) {
	if (!M.ready_game || M.game) return;
	g_mkdir_with_parents(M.home, 0755);
	char *log = g_build_filename(M.home, "game.log", NULL);
	int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	g_free(log);
	GError *error = NULL;
	GSubprocess *process = NULL;
	if (fd >= 0) {
		GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_INHERIT);
		g_subprocess_launcher_set_cwd(launcher, M.home);
		g_subprocess_launcher_take_stdout_fd(launcher, fd);
		g_subprocess_launcher_take_stderr_fd(launcher, dup(fd));
		GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(argv, g_build_filename(M.resources, "bin/openbg", NULL));
		g_ptr_array_add(argv, g_build_filename(M.ready_game, "Data", NULL));
		const char *extra = g_getenv("OPENBG_GAME_ARGS");
		char **words = g_strsplit(extra ? extra : "", " ", -1);
		for (char **w = words; *w; w++)
			if (**w) g_ptr_array_add(argv, g_strdup(*w));
		g_strfreev(words);
		g_ptr_array_add(argv, NULL);
		process = g_subprocess_launcher_spawnv(launcher, (const char *const *)argv->pdata, &error);
		g_ptr_array_unref(argv);
		g_object_unref(launcher);
	}
	if (!process) {
		M.needs_window();
		char *message = g_strdup_printf("The game could not start: %s", error ? error->message : g_strerror(errno));
		set_error(message);
		g_free(message);
		g_clear_error(&error);
		return;
	}
	M.game = g_object_ref(process);
	g_subprocess_wait_async(process, NULL, game_finished, NULL);
	M.hide_window();
	M.changed();
}

static char *log_path(void) {
	if (M.ready) return g_build_filename(M.home, "game.log", NULL);
	char *install_log = g_build_filename(M.home, "install.log", NULL);
	if (g_file_test(install_log, G_FILE_TEST_EXISTS)) return install_log;
	g_free(install_log);
	return g_build_filename(M.home, "bootstrap.log", NULL);
}

static void cancel_setup(void) {
	if (!M.worker) return;
	const char *id = g_subprocess_get_identifier(M.worker);
	if (id) kill(-(pid_t)atoi(id), SIGTERM);  // The worker leads its own process group.
}

static char *default_project(void) {
	const char *env = g_getenv("OPENBG_PROJECT_ROOT");
	if (env && *env) return g_strdup(env);
	char *parent = g_path_get_dirname(M.app_dir);
	char *adjacent = g_path_get_dirname(parent);
	char *marker = g_build_filename(adjacent, "engine/CMakeLists.txt", NULL);
	gboolean found = g_file_test(marker, G_FILE_TEST_IS_REGULAR);
	g_free(marker);
	g_free(parent);
	if (found) return adjacent;
	g_free(adjacent);
	return NULL;
}

static char *default_home(void) {
	const char *env = g_getenv("OPENBG_HOME");
	if (env && *env) return g_strdup(env);
	if (M.project) return g_build_filename(M.project, "source/launcher", NULL);
	char *parent = g_path_get_dirname(M.app_dir);
	gboolean system = g_str_has_prefix(parent, "/usr") || g_str_has_prefix(parent, "/opt");
	if (!system && access(parent, W_OK) == 0) {
		char *home = g_build_filename(parent, "Battlegrounds Files", NULL);
		g_free(parent);
		return home;
	}
	g_free(parent);
	return g_build_filename(g_get_user_data_dir(), "openbg", NULL);
}

static void model_init(const char *resources, gboolean preview) {
	memset(&M, 0, sizeof M);
	M.resources = g_strdup(resources);
	M.app_dir = g_path_get_dirname(resources);
	M.project = default_project();
	M.home = default_home();
	M.preview = preview;
	M.stages[0] = (Stage){"Game files", g_strdup("From your original CD or ISO"), "waiting"};
	M.stages[1] = (Stage){"Bundled update", g_strdup("Updated offline to version 2.123"), "waiting"};
	M.stages[2] = (Stage){"Local setup", g_strdup("Everything installed in your game folder"), "waiting"};
	M.changed = M.needs_window = M.hide_window = M.quit = noop;
}

// Ask readiness.py (on the private Python) for a verified installation.
static char *find_ready_game(void) {
	char *python = g_build_filename(M.resources, "python/bin/python3", NULL);
	char *script = g_build_filename(M.resources, "readiness.py", NULL);
	const char *argv[] = {python, "-I", "-B", script, M.resources, M.home, M.project, NULL};
	char *out = NULL;
	int status = 0;
	gboolean ran = g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, &status, NULL);
	g_free(python);
	g_free(script);
	if (!ran || !g_spawn_check_wait_status(status, NULL) || !out) {
		g_free(out);
		return NULL;
	}
	g_strstrip(out);
	if (!*out) g_clear_pointer(&out, g_free);
	return out;
}

static gboolean engine_present(void) {
	char *engine = g_build_filename(M.resources, "bin/openbg", NULL);
	gboolean ok = g_file_test(engine, G_FILE_TEST_IS_EXECUTABLE);
	g_free(engine);
	return ok;
}

static gboolean manifest_present(void) {
	char *manifest = g_build_filename(M.resources, "game-files.json", NULL);
	gboolean ok = g_file_test(manifest, G_FILE_TEST_IS_REGULAR);
	g_free(manifest);
	return ok;
}

#ifndef LAUNCHER_MODEL_TESTS

// ---------------------------------------------------------------------------- view

static const char *CSS =
	"window, .bay { background-color: #141718; color: #e8e3d4; }\n"
	".rule { background-color: rgba(255,181,41,0.5); min-width: 1px; }\n"
	".status { font-family: monospace; font-size: 9px; font-weight: 500; color: #ffb529; }\n"
	".muted { color: #878f8c; }\n"
	".amber { color: #ffb529; }\n"
	".title { font-family: 'DIN Condensed', 'Oswald', 'Bebas Neue', 'Roboto Condensed', 'Ubuntu Condensed',"
	"  'DejaVu Sans Condensed', sans-serif; font-weight: bold; font-size: 40px; }\n"
	".subtitle { font-size: 15px; color: rgba(232,227,212,0.65); }\n"
	".display { font-family: 'DIN Condensed', 'Oswald', 'Bebas Neue', 'Roboto Condensed', 'Ubuntu Condensed',"
	"  'DejaVu Sans Condensed', sans-serif; font-weight: bold; }\n"
	"button { background-image: none; box-shadow: none; text-shadow: none; -gtk-icon-shadow: none;"
	"  border-image: none; outline-color: rgba(255,181,41,0.6); }\n"
	"button.source { border-radius: 2px; padding: 0; min-height: 136px; transition: none; }\n"
	"button.iso { background-color: rgba(255,181,41,0.025); border: 1.5px dashed rgba(255,181,41,0.8); color: #ffb529; }\n"
	"button.iso:hover, button.iso.drop { background-color: rgba(255,181,41,0.10); }\n"
	"button.cd { background-color: rgba(232,227,212,0.025); border: 1px solid rgba(232,227,212,0.23);"
	"  color: rgba(232,227,212,0.7); }\n"
	"button.cd:hover { border-color: rgba(232,227,212,0.4); }\n"
	"button.cd.detected { color: #ffb529; }\n"
	"button.source:disabled { opacity: 0.45; }\n"
	"button.source:disabled label { color: inherit; }\n"
	".glyph { font-size: 34px; }\n"
	".hint { font-size: 12px; color: rgba(232,227,212,0.5); }\n"
	".panel { background-color: rgba(232,227,212,0.018); border: 1px solid rgba(232,227,212,0.18); padding: 16px; }\n"
	".mono { font-family: monospace; font-size: 10px; }\n"
	".divider { background-color: rgba(232,227,212,0.1); min-height: 1px; }\n"
	".stage-title { font-size: 13px; font-weight: 600; }\n"
	".small { font-size: 11px; }\n"
	".check { color: #ffb529; font-weight: bold; font-size: 12px; }\n"
	"spinner { color: #ffb529; }\n"
	".error { background-color: rgba(255,181,41,0.06); padding: 15px; }\n"
	".error label.message { color: #ffb529; font-size: 12px; }\n"
	"button.bordered { background-color: rgba(232,227,212,0.08); border: 1px solid rgba(232,227,212,0.18);"
	"  border-radius: 4px; color: #e8e3d4; padding: 4px 12px; font-size: 11px; }\n"
	"button.bordered:hover { background-color: rgba(232,227,212,0.14); }\n"
	"button.primary { background-color: #ffb529; color: #000; border-color: #ffb529; }\n"
	"button.primary label, button.launch.ready label { color: #000; }\n"
	"button.primary:hover { background-color: #ffc252; }\n"
	"button.launch { min-height: 51px; border-radius: 0; border: 1px solid rgba(232,227,212,0.14);"
	"  background-color: rgba(232,227,212,0.06); color: rgba(135,143,140,0.65); font-size: 24px; }\n"
	"button.launch.ready { background-color: #ffb529; color: #000; }\n"
	"button.launch.ready:hover { background-color: #ffc252; }\n"
	"button.launch:disabled label { color: inherit; }\n"
	".footer-rule { background-color: rgba(232,227,212,0.15); min-height: 1px; }\n"
	"button.link { background-color: transparent; border: none; color: #878f8c; padding: 2px 4px; font-size: 11px; }\n"
	"button.link:hover { color: #e8e3d4; }\n"
	".sheet { background-color: #141718; }\n"
	".sheet-title { font-size: 20px; font-weight: bold; color: #e8e3d4; }\n"
	".sheet label { color: #878f8c; font-size: 14px; }\n"
	".sheet label.sheet-title { color: #e8e3d4; }\n"
	".path { font-family: monospace; font-size: 12px; }\n";

static struct {
	GtkWidget *window, *sidebar, *status, *title, *subtitle, *iso, *iso_hint, *cd, *cd_hint, *source;
	GtkWidget *stage_icon[3], *stage_detail[3], *stage_state[3], *error, *error_label, *retry, *launch;
	GtkWidget *launch_label, *confirm;
	GdkPixbuf *artwork;
	gboolean shown;
	char *screenshot;
} V;

static GtkWidget *label(const char *text, const char *classes) {
	GtkWidget *widget = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(widget), 0);
	if (classes) {
		char **names = g_strsplit(classes, " ", -1);
		for (char **n = names; *n; n++)
			if (**n) gtk_style_context_add_class(gtk_widget_get_style_context(widget), *n);
		g_strfreev(names);
	}
	return widget;
}

static void add_class(GtkWidget *widget, const char *name) {
	gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}

static void set_class(GtkWidget *widget, const char *name, gboolean on) {
	GtkStyleContext *style = gtk_widget_get_style_context(widget);
	if (on) gtk_style_context_add_class(style, name);
	else gtk_style_context_remove_class(style, name);
}

static void show_text(cairo_t *cr, double x, double y, const char *font, const char *rgba, const char *text,
                      double align, double max_width) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *description = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, description);
	pango_layout_set_text(layout, text, -1);
	int width, height;
	pango_layout_get_pixel_size(layout, &width, &height);
	// Shrink to fit, like SwiftUI's minimumScaleFactor.
	while (max_width > 0 && width > max_width && pango_font_description_get_size(description) > 8 * PANGO_SCALE) {
		pango_font_description_set_size(description, pango_font_description_get_size(description) - PANGO_SCALE);
		pango_layout_set_font_description(layout, description);
		pango_layout_get_pixel_size(layout, &width, &height);
	}
	GdkRGBA color;
	gdk_rgba_parse(&color, rgba);
	gdk_cairo_set_source_rgba(cr, &color);
	cairo_move_to(cr, x - width * align, y);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(description);
	g_object_unref(layout);
}

static gboolean draw_sidebar(GtkWidget *widget, cairo_t *cr, gpointer data) {
	(void)data;
	double w = gtk_widget_get_allocated_width(widget), h = gtk_widget_get_allocated_height(widget);
	cairo_pattern_t *gradient = cairo_pattern_create_linear(0, 0, w, h);
	cairo_pattern_add_color_stop_rgb(gradient, 0, 0.10, 0.10, 0.10);
	cairo_pattern_add_color_stop_rgb(gradient, 1, 0.055, 0.055, 0.055);
	cairo_set_source(cr, gradient);
	cairo_paint(cr);
	cairo_pattern_destroy(gradient);
	double x0 = 25, x1 = w - 25;
	cairo_set_source_rgba(cr, 1, 0.71, 0.16, 0.65);
	cairo_rectangle(cr, x0, 31, x1 - x0, 1);
	cairo_fill(cr);
	const char *display = "DIN Condensed, Oswald, Bebas Neue, Roboto Condensed, Ubuntu Condensed, "
	                      "DejaVu Sans Condensed, Sans Bold Condensed 38px";
	show_text(cr, x0, 42, display, "#e8e3d4", "ONE MUST FALL", 0, x1 - x0);
	show_text(cr, x0, 91, "Sans Semi-Bold 10px", "#ffb529", "B A T T L E G R O U N D S", 0, 0);
	cairo_set_source_rgba(cr, 0.53, 0.56, 0.55, 1);
	cairo_rectangle(cr, x0, 128, 52, 1);
	cairo_rectangle(cr, x1 - 52, 128, 52, 1);
	cairo_fill(cr);
	show_text(cr, w / 2, 122, "Monospace 8px", "#878f8c", "OPEN ENGINE", 0.5, 0);
	double top = 160, bottom = h - 112;
	if (bottom > top) {
		cairo_save(cr);
		cairo_rectangle(cr, x0, top, x1 - x0, bottom - top);
		cairo_clip(cr);
		cairo_set_source_rgba(cr, 0.91, 0.89, 0.83, 0.04);
		cairo_set_line_width(cr, 0.5);
		for (double x = x0; x <= x1; x += 24) {
			cairo_move_to(cr, x, top);
			cairo_line_to(cr, x, bottom);
		}
		for (double y = top; y <= bottom; y += 24) {
			cairo_move_to(cr, x0, y);
			cairo_line_to(cr, x1, y);
		}
		cairo_stroke(cr);
		if (V.artwork) {
			double iw = gdk_pixbuf_get_width(V.artwork), ih = gdk_pixbuf_get_height(V.artwork);
			double scale = MIN((x1 - x0) / iw, (bottom - top) / ih) * 1.15;
			cairo_translate(cr, (x0 + x1) / 2 - iw * scale / 2, (top + bottom) / 2 - ih * scale / 2);
			cairo_scale(cr, scale, scale);
			gdk_cairo_set_source_pixbuf(cr, V.artwork, 0, 0);
			cairo_set_operator(cr, CAIRO_OPERATOR_LIGHTEN);
			cairo_paint(cr);
		}
		cairo_restore(cr);
	}
	show_text(cr, x0, h - 92, "Monospace 9px", "#878f8c", "ARENA SYSTEMS", 0, 0);
	show_text(cr, x1, h - 92, "Monospace 9px", "#ffb529", "2.123", 1, 0);
	show_text(cr, x0, h - 76, "Monospace 8px", "rgba(135,143,140,0.7)", "COMBAT CHASSIS / STANDBY", 0, 0);
	cairo_save(cr);
	double y0 = h - 42;
	cairo_rectangle(cr, x0, y0, x1 - x0, 10);
	cairo_clip(cr);
	cairo_set_source_rgba(cr, 1, 0.71, 0.16, 0.55);
	for (double x = x0 - 30; x < x1 + 30; x += 26) {
		cairo_move_to(cr, x, y0);
		cairo_line_to(cr, x + 13, y0);
		cairo_line_to(cr, x - 3, y0 + 10);
		cairo_line_to(cr, x - 16, y0 + 10);
		cairo_close_path(cr);
	}
	cairo_fill(cr);
	cairo_restore(cr);
	return FALSE;
}

static void show_window(void);
static void refresh(void);

static void open_path(const char *path) {
	GFile *file = g_file_new_for_path(path);
	char *uri = g_file_get_uri(file);
	GError *error = NULL;
	if (!gtk_show_uri_on_window(GTK_WINDOW(V.window), uri, GDK_CURRENT_TIME, &error)) {
		char *message = g_strdup_printf("Could not open %s: %s", path, error->message);
		set_error(message);
		g_free(message);
		g_error_free(error);
	}
	g_free(uri);
	g_object_unref(file);
}

static void on_choose_iso(GtkButton *button, gpointer data) {
	(void)button; (void)data;
	if (M.busy || M.ready) return;
	GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
		"Choose your Battlegrounds disc image", GTK_WINDOW(V.window), GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
	GtkFileFilter *filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, "Disc images (*.iso)");
	gtk_file_filter_add_pattern(filter, "*.iso");
	gtk_file_filter_add_pattern(filter, "*.ISO");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
	if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
		char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
		if (path) request_install(path);
		g_free(path);
	}
	g_object_unref(chooser);
}

static void on_drop(GtkWidget *widget, GdkDragContext *context, int x, int y, GtkSelectionData *data,
                    guint info, guint time, gpointer user) {
	(void)widget; (void)x; (void)y; (void)info; (void)user;
	char **uris = gtk_selection_data_get_uris(data);
	gboolean accepted = FALSE;
	if (uris && uris[0] && !M.busy && !M.ready) {
		char *path = g_filename_from_uri(uris[0], NULL, NULL);
		if (path) {
			request_install(path);
			accepted = TRUE;
		}
		g_free(path);
	}
	g_strfreev(uris);
	gtk_drag_finish(context, accepted, FALSE, time);
}

static gboolean on_drag_motion(GtkWidget *widget, GdkDragContext *context, int x, int y, guint time, gpointer data) {
	(void)context; (void)x; (void)y; (void)time; (void)data;
	set_class(widget, "drop", TRUE);
	return FALSE;
}

static void on_drag_leave(GtkWidget *widget, GdkDragContext *context, guint time, gpointer data) {
	(void)context; (void)time; (void)data;
	set_class(widget, "drop", FALSE);
}

static void on_use_cd(GtkButton *button, gpointer data) {
	(void)button; (void)data;
	scan_discs();
	if (!M.busy && M.disc) request_install(M.disc);
}

static void on_retry(GtkButton *button, gpointer data) {
	(void)button; (void)data;
	if (M.last_source) request_install(M.last_source);
}

static void on_open_log(GtkButton *button, gpointer data) {
	(void)button; (void)data;
	char *path = log_path();
	open_path(path);
	g_free(path);
}

static void on_launch(GtkButton *button, gpointer data) {
	(void)button; (void)data;
	if (M.ready && !M.game) launch();
}

static GtkWidget *sheet(const char *title, const char *const *paragraphs, const char *path) {
	GtkWidget *dialog = gtk_dialog_new();
	gtk_window_set_title(GTK_WINDOW(dialog), title);
	gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(V.window));
	gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
	gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
	gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	add_class(dialog, "sheet");
	gtk_box_set_spacing(GTK_BOX(content), 18);
	gtk_container_set_border_width(GTK_CONTAINER(content), 30);
	gtk_widget_set_size_request(content, 480, -1);
	gtk_box_pack_start(GTK_BOX(content), label(title, "sheet-title"), FALSE, FALSE, 0);
	if (path) {
		GtkWidget *value = label(path, "path");
		gtk_label_set_selectable(GTK_LABEL(value), TRUE);
		gtk_label_set_line_wrap(GTK_LABEL(value), TRUE);
		gtk_label_set_line_wrap_mode(GTK_LABEL(value), PANGO_WRAP_CHAR);
		gtk_box_pack_start(GTK_BOX(content), value, FALSE, FALSE, 0);
	}
	for (const char *const *p = paragraphs; *p; p++) {
		GtkWidget *text = label(*p, NULL);
		gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(text), 52);
		gtk_box_pack_start(GTK_BOX(content), text, FALSE, FALSE, 0);
	}
	return dialog;
}

static GtkWidget *sheet_button(GtkWidget *dialog, const char *text, int response, gboolean primary) {
	GtkWidget *button = gtk_dialog_add_button(GTK_DIALOG(dialog), text, response);
	add_class(button, "bordered");
	if (primary) {
		add_class(button, "primary");
		gtk_dialog_set_default_response(GTK_DIALOG(dialog), response);
	}
	return button;
}

static void on_help(GtkButton *button, gpointer data) {
	(void)button; (void)data;
	const char *paragraphs[] = {
		"Choose an ISO of your original English Battlegrounds 2.090 disc, or insert the CD and click Use game CD. "
		"An inserted game CD is detected automatically once your desktop mounts it. Confirm Start setup to begin. "
		"Setup uses the update and tools bundled with this app, then verifies the finished installation.",
		"When everything is ready, the game starts. Next time, this screen is skipped automatically. "
		"Your original disc and ISO are never changed.",
		"First-time setup works offline and needs 3 GB of free space. No root password or system-wide packages are required.",
		NULL};
	GtkWidget *dialog = sheet("Getting arena-ready", paragraphs, NULL);
	sheet_button(dialog, "Done", GTK_RESPONSE_CLOSE, TRUE);
	gtk_widget_show_all(dialog);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}

static void on_settings(GtkButton *button, gpointer data) {
	(void)button; (void)data;
	const char *paragraphs[] = {
		"Your extracted and updated game files stay here. Setup tools and the update are included in the app. "
		"Nothing is added to your shell or system package manager. A verified installation opens directly into the game.",
		NULL};
	GtkWidget *dialog = sheet("Your game folder", paragraphs, M.home);
	GtkWidget *open = sheet_button(dialog, "Open folder", 1, FALSE);
	gtk_button_box_set_child_secondary(GTK_BUTTON_BOX(gtk_widget_get_parent(open)), open, TRUE);
	sheet_button(dialog, "Done", GTK_RESPONSE_CLOSE, TRUE);
	gtk_widget_show_all(dialog);
	while (gtk_dialog_run(GTK_DIALOG(dialog)) == 1) {
		g_mkdir_with_parents(M.home, 0755);
		open_path(M.home);
	}
	gtk_widget_destroy(dialog);
}

static void on_confirm_response(GtkDialog *dialog, int response, gpointer data) {
	(void)data;
	gtk_widget_destroy(GTK_WIDGET(dialog));
	V.confirm = NULL;
	if (response == GTK_RESPONSE_ACCEPT) confirm_install();
	else cancel_pending();
}

static void ask_confirmation(void) {
	char *name = g_path_get_basename(M.pending_source);
	char *text = g_strdup_printf("Setup will install %s using the bundled update and tools. It works offline and "
	                             "needs at least 3 GB of free space in %s.", name, M.home);
	const char *paragraphs[] = {text, NULL};
	V.confirm = sheet("Set up Battlegrounds?", paragraphs, NULL);
	sheet_button(V.confirm, "Cancel", GTK_RESPONSE_CANCEL, FALSE);
	sheet_button(V.confirm, "Start setup", GTK_RESPONSE_ACCEPT, TRUE);
	g_signal_connect(V.confirm, "response", G_CALLBACK(on_confirm_response), NULL);
	gtk_widget_show_all(V.confirm);
	g_free(text);
	g_free(name);
}

static void refresh(void) {
	if (!V.window) return;
	gtk_label_set_text(GTK_LABEL(V.status), M.ready ? "ARENA SYSTEMS ONLINE" : "FIRST-TIME SETUP");
	gtk_label_set_text(GTK_LABEL(V.title), M.ready ? "You're ready for the arena." : "Prepare for the arena.");
	gtk_label_set_text(GTK_LABEL(V.subtitle), M.busy ? "Setting things up. This may take a few minutes."
	                                                 : "Choose your game disc or ISO. We'll handle the rest.");
	gboolean locked = M.busy || M.ready;
	gtk_widget_set_sensitive(V.iso, !locked);
	gtk_widget_set_sensitive(V.cd, !locked && M.disc);
	set_class(V.cd, "detected", M.disc != NULL);
	gtk_label_set_text(GTK_LABEL(V.cd_hint), M.disc ? "Disc detected · click to set up" : "Insert your game disc");
	char *source = g_strdup_printf("%s  %s", M.source_name ? "✓" : "◎",
	                               M.source_name ? M.source_name : "Your game CD is detected automatically when inserted.");
	gtk_label_set_text(GTK_LABEL(V.source), source);
	g_free(source);
	for (int i = 0; i < 3; i++) {
		const char *status = M.stages[i].status;
		gtk_label_set_text(GTK_LABEL(V.stage_detail[i]), M.stages[i].detail);
		const char *want = strcmp(status, "done") == 0 ? "done" : strcmp(status, "active") == 0 ? "active" : "waiting";
		gtk_stack_set_visible_child_name(GTK_STACK(V.stage_icon[i]), want);
		gtk_widget_queue_draw(gtk_widget_get_parent(V.stage_icon[i]));
		gtk_label_set_text(GTK_LABEL(V.stage_state[i]), strcmp(want, "done") == 0 ? "Ready"
		                                              : strcmp(want, "active") == 0 ? "Working…" : "Waiting");
		set_class(V.stage_state[i], "amber", strcmp(want, "waiting") != 0);
		set_class(V.stage_state[i], "muted", strcmp(want, "waiting") == 0);
	}
	if (M.error) {
		char *message = g_strdup_printf("⚠  %s", M.error);
		gtk_label_set_text(GTK_LABEL(V.error_label), message);
		g_free(message);
		gtk_widget_set_visible(V.retry, M.last_source && !M.ready);
		gtk_widget_set_sensitive(V.retry, !M.busy);
		gtk_widget_show(V.error);
	} else {
		gtk_widget_hide(V.error);
	}
	set_class(V.launch, "ready", M.ready);
	gtk_widget_set_sensitive(V.launch, M.ready && !M.game);
	gtk_label_set_text(GTK_LABEL(V.launch_label), M.busy ? "▶  Preparing your game…" : "▶  Launch game");
	if (M.pending_source && !V.confirm && V.shown) ask_confirmation();
}

static gboolean draw_stage_ring(GtkWidget *widget, cairo_t *cr, gpointer index) {
	const char *status = M.stages[GPOINTER_TO_INT(index)].status;
	double w = gtk_widget_get_allocated_width(widget), h = gtk_widget_get_allocated_height(widget);
	if (strcmp(status, "waiting") == 0) cairo_set_source_rgba(cr, 0.53, 0.56, 0.55, 0.5);
	else cairo_set_source_rgba(cr, 1, 0.71, 0.16, 0.8);
	cairo_set_line_width(cr, 1);
	cairo_arc(cr, w / 2, h / 2, 12, 0, 2 * G_PI);
	cairo_stroke(cr);
	return FALSE;
}

// Render the window offscreen rather than reading it back from the display:
// gdk_pixbuf_get_from_window() aborts with cairo 1.16 (Ubuntu 22.04), and
// reading back does not work on Wayland.
static gboolean take_screenshot(gpointer data) {
	(void)data;
	int width = gtk_widget_get_allocated_width(V.window), height = gtk_widget_get_allocated_height(V.window);
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cairo_t *cr = cairo_create(surface);
	gtk_widget_draw(V.window, cr);
	cairo_destroy(cr);
	cairo_status_t status = cairo_surface_write_to_png(surface, V.screenshot);
	cairo_surface_destroy(surface);
	if (status != CAIRO_STATUS_SUCCESS) g_printerr("Could not save %s: %s\n", V.screenshot, cairo_status_to_string(status));
	gtk_main_quit();
	return G_SOURCE_REMOVE;
}

static void show_window(void) {
	if (!V.shown) {
		V.shown = TRUE;
		gtk_widget_show_all(V.window);
		refresh();
		if (V.screenshot) g_timeout_add(1000, take_screenshot, NULL);
	}
	gtk_window_present(GTK_WINDOW(V.window));
}

static void hide_window(void) {
	if (V.window) gtk_widget_hide(V.window);
}

static void on_stop_response(GtkDialog *dialog, int response, gpointer data) {
	(void)data;
	gtk_widget_destroy(GTK_WIDGET(dialog));
	if (response == GTK_RESPONSE_REJECT) {
		cancel_setup();
		gtk_main_quit();
	}
}

static gboolean on_delete(GtkWidget *widget, GdkEvent *event, gpointer data) {
	(void)widget; (void)event; (void)data;
	if (!M.busy) {
		gtk_main_quit();
		return TRUE;
	}
	const char *paragraphs[] = {"Your original files are safe. You can restart setup whenever you're ready.", NULL};
	GtkWidget *dialog = sheet("Stop setting up the game?", paragraphs, NULL);
	sheet_button(dialog, "Stop setup", GTK_RESPONSE_REJECT, FALSE);
	sheet_button(dialog, "Keep setting up", GTK_RESPONSE_CANCEL, TRUE);
	g_signal_connect(dialog, "response", G_CALLBACK(on_stop_response), NULL);
	gtk_widget_show_all(dialog);
	return TRUE;
}

static gboolean on_quit_key(GtkAccelGroup *group, GObject *object, guint key, GdkModifierType mods, gpointer data) {
	(void)group; (void)object; (void)key; (void)mods; (void)data;
	return on_delete(V.window, NULL, NULL);
}

static gboolean on_unix_signal(gpointer data) {
	(void)data;
	cancel_setup();
	gtk_main_quit();
	return G_SOURCE_REMOVE;
}

static void on_mounts_changed(GUnixMountMonitor *monitor, gpointer data) {
	(void)monitor; (void)data;
	scan_discs();
}

static GtkWidget *source_button(const char *css, const char *glyph, const char *title, int title_px, GtkWidget **hint,
                                const char *hint_text) {
	GtkWidget *button = gtk_button_new();
	add_class(button, "source");
	add_class(button, css);
	gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
	GtkWidget *icon = label(glyph, "glyph");
	gtk_label_set_xalign(GTK_LABEL(icon), 0.5);
	char *markup = g_strdup_printf("<span size='%d'>%s</span>", title_px * PANGO_SCALE * 3 / 4, title);
	GtkWidget *name = label(NULL, "display");
	gtk_label_set_markup(GTK_LABEL(name), markup);
	gtk_label_set_xalign(GTK_LABEL(name), 0.5);
	g_free(markup);
	*hint = label(hint_text, "hint");
	gtk_label_set_xalign(GTK_LABEL(*hint), 0.5);
	gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), name, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), *hint, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(button), box);
	return button;
}

static void build_window(void) {
	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(css, CSS, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
	                                          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
	g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
	char *art = g_build_filename(M.resources, "assets/robot-blueprint.png", NULL);
	V.artwork = gdk_pixbuf_new_from_file_at_scale(art, 512, -1, TRUE, NULL);
	g_free(art);

	V.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(V.window), "One Must Fall: Battlegrounds");
	gtk_window_set_default_size(GTK_WINDOW(V.window), 1160, 780);
	gtk_widget_set_size_request(V.window, 1000, 730);
	gtk_window_set_position(GTK_WINDOW(V.window), GTK_WIN_POS_CENTER);
	g_signal_connect(V.window, "delete-event", G_CALLBACK(on_delete), NULL);
	GtkAccelGroup *accelerators = gtk_accel_group_new();
	gtk_window_add_accel_group(GTK_WINDOW(V.window), accelerators);
	GClosure *quit = g_cclosure_new(G_CALLBACK(on_quit_key), NULL, NULL);
	gtk_accel_group_connect(accelerators, GDK_KEY_q, GDK_CONTROL_MASK, 0, quit);

	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_container_add(GTK_CONTAINER(V.window), row);
	V.sidebar = gtk_drawing_area_new();
	gtk_widget_set_size_request(V.sidebar, 265, -1);
	g_signal_connect(V.sidebar, "draw", G_CALLBACK(draw_sidebar), NULL);
	atk_object_set_name(gtk_widget_get_accessible(V.sidebar), "One Must Fall: Battlegrounds. Open engine, version 2.123.");
	gtk_box_pack_start(GTK_BOX(row), V.sidebar, FALSE, FALSE, 0);
	GtkWidget *rule = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	add_class(rule, "rule");
	gtk_box_pack_start(GTK_BOX(row), rule, FALSE, FALSE, 0);

	GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	add_class(main, "bay");
	gtk_box_pack_start(GTK_BOX(row), main, TRUE, TRUE, 0);

	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	g_object_set(header, "margin-start", 38, "margin-end", 38, "margin-top", 26, "margin-bottom", 10, NULL);
	gtk_box_pack_start(GTK_BOX(main), header, FALSE, FALSE, 0);
	GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
	gtk_box_pack_start(GTK_BOX(status_row), label("●", "status"), FALSE, FALSE, 0);
	V.status = label("", "status");
	gtk_box_pack_start(GTK_BOX(status_row), V.status, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(status_row), label("SERVICE BAY  /  01", "status muted"), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(header), status_row, FALSE, FALSE, 0);
	V.title = label("", "title");
	gtk_widget_set_margin_top(V.title, 13);
	gtk_label_set_ellipsize(GTK_LABEL(V.title), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(header), V.title, FALSE, FALSE, 0);
	V.subtitle = label("", "subtitle");
	gtk_widget_set_margin_top(V.subtitle, 8);
	gtk_box_pack_start(GTK_BOX(header), V.subtitle, FALSE, FALSE, 0);

	GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
	g_object_set(body, "margin-start", 38, "margin-end", 38, "margin-top", 20, "margin-bottom", 18, NULL);
	gtk_box_pack_start(GTK_BOX(main), body, TRUE, TRUE, 0);

	GtkWidget *sources = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
	V.iso = source_button("iso", "⊕", "Choose ISO file", 27, &V.iso_hint, "or drop an ISO here");
	g_signal_connect(V.iso, "clicked", G_CALLBACK(on_choose_iso), NULL);
	gtk_drag_dest_set(V.iso, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
	gtk_drag_dest_add_uri_targets(V.iso);
	g_signal_connect(V.iso, "drag-data-received", G_CALLBACK(on_drop), NULL);
	g_signal_connect(V.iso, "drag-motion", G_CALLBACK(on_drag_motion), NULL);
	g_signal_connect(V.iso, "drag-leave", G_CALLBACK(on_drag_leave), NULL);
	gtk_widget_set_tooltip_text(V.iso, "Select your original Battlegrounds ISO disc image");
	gtk_box_pack_start(GTK_BOX(buttons), V.iso, TRUE, TRUE, 0);
	V.cd = source_button("cd", "◎", "Use game CD", 24, &V.cd_hint, "Insert your game disc");
	gtk_widget_set_size_request(V.cd, 171, 136);
	g_signal_connect(V.cd, "clicked", G_CALLBACK(on_use_cd), NULL);
	gtk_box_pack_start(GTK_BOX(buttons), V.cd, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(sources), buttons, FALSE, FALSE, 0);
	V.source = label("", "muted small");
	gtk_label_set_ellipsize(GTK_LABEL(V.source), PANGO_ELLIPSIZE_MIDDLE);
	gtk_box_pack_start(GTK_BOX(sources), V.source, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(body), sources, FALSE, FALSE, 0);

	GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	add_class(panel, "panel");
	GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_bottom(heading, 14);
	gtk_box_pack_start(GTK_BOX(heading), label("GAME  SETUP", "mono muted"), FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(heading), label("2.123", "mono amber"), FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(heading), label("TARGET  VERSION", "mono muted"), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(panel), heading, FALSE, FALSE, 0);
	for (int i = 0; i < 3; i++) {
		GtkWidget *divider = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		add_class(divider, "divider");
		gtk_box_pack_start(GTK_BOX(panel), divider, FALSE, FALSE, 0);
		GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
		g_object_set(line, "margin-top", 11, "margin-bottom", 11, NULL);
		char number[4];
		g_snprintf(number, sizeof number, "%02d", i + 1);
		gtk_box_pack_start(GTK_BOX(line), label(number, "mono muted"), FALSE, FALSE, 0);
		GtkWidget *ring = gtk_overlay_new();
		GtkWidget *circle = gtk_drawing_area_new();
		gtk_widget_set_size_request(circle, 27, 27);
		g_signal_connect(circle, "draw", G_CALLBACK(draw_stage_ring), GINT_TO_POINTER(i));
		gtk_container_add(GTK_CONTAINER(ring), circle);
		// Waiting: the bare ring; active: a spinner; done: a check mark.
		V.stage_icon[i] = gtk_stack_new();
		gtk_stack_add_named(GTK_STACK(V.stage_icon[i]), gtk_label_new(""), "waiting");
		GtkWidget *spinner = gtk_spinner_new();
		gtk_widget_set_size_request(spinner, 15, 15);
		gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
		gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
		gtk_spinner_start(GTK_SPINNER(spinner));
		gtk_stack_add_named(GTK_STACK(V.stage_icon[i]), spinner, "active");
		GtkWidget *check = label("✓", "check");
		gtk_label_set_xalign(GTK_LABEL(check), 0.5);
		gtk_stack_add_named(GTK_STACK(V.stage_icon[i]), check, "done");
		gtk_overlay_add_overlay(GTK_OVERLAY(ring), V.stage_icon[i]);
		gtk_widget_set_valign(ring, GTK_ALIGN_CENTER);
		gtk_box_pack_start(GTK_BOX(line), ring, FALSE, FALSE, 0);
		GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		gtk_box_pack_start(GTK_BOX(text), label(M.stages[i].title, "stage-title"), FALSE, FALSE, 0);
		V.stage_detail[i] = label("", "muted small");
		gtk_label_set_line_wrap(GTK_LABEL(V.stage_detail[i]), TRUE);
		gtk_label_set_lines(GTK_LABEL(V.stage_detail[i]), 2);
		gtk_label_set_ellipsize(GTK_LABEL(V.stage_detail[i]), PANGO_ELLIPSIZE_END);
		gtk_box_pack_start(GTK_BOX(text), V.stage_detail[i], FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(line), text, TRUE, TRUE, 0);
		V.stage_state[i] = label("", "mono");
		gtk_box_pack_end(GTK_BOX(line), V.stage_state[i], FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(panel), line, FALSE, FALSE, 0);
	}
	gtk_box_pack_start(GTK_BOX(body), panel, FALSE, FALSE, 0);

	V.error = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	add_class(V.error, "error");
	V.error_label = label("", "message");
	gtk_label_set_line_wrap(GTK_LABEL(V.error_label), TRUE);
	gtk_label_set_selectable(GTK_LABEL(V.error_label), TRUE);
	gtk_box_pack_start(GTK_BOX(V.error), V.error_label, FALSE, FALSE, 0);
	GtkWidget *error_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	V.retry = gtk_button_new_with_label("Try again");
	add_class(V.retry, "bordered");
	g_signal_connect(V.retry, "clicked", G_CALLBACK(on_retry), NULL);
	gtk_box_pack_start(GTK_BOX(error_buttons), V.retry, FALSE, FALSE, 0);
	GtkWidget *open_log = gtk_button_new_with_label("Open setup log");
	add_class(open_log, "bordered");
	g_signal_connect(open_log, "clicked", G_CALLBACK(on_open_log), NULL);
	gtk_box_pack_start(GTK_BOX(error_buttons), open_log, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(V.error), error_buttons, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(body), V.error, FALSE, FALSE, 0);

	V.launch = gtk_button_new();
	add_class(V.launch, "launch");
	V.launch_label = label("", "display");
	gtk_label_set_xalign(GTK_LABEL(V.launch_label), 0.5);
	gtk_container_add(GTK_CONTAINER(V.launch), V.launch_label);
	gtk_widget_set_can_default(V.launch, TRUE);
	g_signal_connect(V.launch, "clicked", G_CALLBACK(on_launch), NULL);
	gtk_box_pack_end(GTK_BOX(body), V.launch, FALSE, FALSE, 0);
	gtk_window_set_default(GTK_WINDOW(V.window), V.launch);

	GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *footer_rule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	add_class(footer_rule, "footer-rule");
	gtk_box_pack_start(GTK_BOX(footer), footer_rule, FALSE, FALSE, 0);
	GtkWidget *footer_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
	g_object_set(footer_row, "margin-start", 30, "margin-end", 30, "margin-top", 16, "margin-bottom", 16, NULL);
	gtk_box_pack_start(GTK_BOX(footer_row), label("▣  Everything stays in your game folder.", "muted small"), FALSE, FALSE, 0);
	GtkWidget *help = gtk_button_new_with_label("?  Help");
	add_class(help, "link");
	g_signal_connect(help, "clicked", G_CALLBACK(on_help), NULL);
	gtk_box_pack_end(GTK_BOX(footer_row), help, FALSE, FALSE, 0);
	GtkWidget *separator = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	add_class(separator, "divider");
	gtk_widget_set_size_request(separator, 1, 16);
	gtk_widget_set_valign(separator, GTK_ALIGN_CENTER);
	gtk_box_pack_end(GTK_BOX(footer_row), separator, FALSE, FALSE, 6);
	GtkWidget *settings = gtk_button_new_with_label("⚙  Settings");
	add_class(settings, "link");
	g_signal_connect(settings, "clicked", G_CALLBACK(on_settings), NULL);
	gtk_box_pack_end(GTK_BOX(footer_row), settings, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(footer), footer_row, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(main), footer, FALSE, FALSE, 0);
}

static int create_desktop_entry(const char *executable) {
	char *folder = g_build_filename(g_get_user_data_dir(), "applications", NULL);
	g_mkdir_with_parents(folder, 0755);
	char *path = g_build_filename(folder, "openomf-battlegrounds.desktop", NULL);
	// Desktop Entry quoting: wrap in double quotes, escape " ` $ and backslash.
	GString *exec = g_string_new("\"");
	for (const char *c = executable; *c; c++) {
		if (strchr("\"`$\\", *c)) g_string_append_c(exec, '\\');
		g_string_append_c(exec, *c);
	}
	g_string_append(exec, "\" %f");
	char *icon = g_build_filename(M.resources, "assets/robot-blueprint.png", NULL);
	GKeyFile *entry = g_key_file_new();
	const char *group = "Desktop Entry";
	g_key_file_set_string(entry, group, "Type", "Application");
	g_key_file_set_string(entry, group, "Name", "One Must Fall: Battlegrounds");
	g_key_file_set_string(entry, group, "Comment", "Play One Must Fall: Battlegrounds on the open-source engine");
	g_key_file_set_string(entry, group, "Exec", exec->str);
	g_key_file_set_string(entry, group, "Icon", icon);
	g_key_file_set_string(entry, group, "Terminal", "false");
	g_key_file_set_string(entry, group, "Categories", "Game;ActionGame;");
	g_key_file_set_string(entry, group, "StartupWMClass", "Battlegrounds");
	GError *error = NULL;
	gboolean saved = g_key_file_save_to_file(entry, path, &error);
	if (saved) g_print("Created %s\n", path);
	else g_printerr("Could not create %s: %s\n", path, error->message);
	g_clear_error(&error);
	g_key_file_unref(entry);
	g_free(icon);
	g_string_free(exec, TRUE);
	g_free(path);
	g_free(folder);
	return saved ? 0 : 1;
}

static void quit_main(void) { gtk_main_quit(); }

int main(int argc, char **argv) {
	char *self = g_file_read_link("/proc/self/exe", NULL);
	if (!self) self = g_canonicalize_filename(argv[0], NULL);
	char *app_dir = g_path_get_dirname(self);
	char *resources = g_build_filename(app_dir, "resources", NULL);
	gboolean preview = FALSE;
	const char *initial = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--preview") == 0) preview = TRUE;
		else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
			preview = TRUE;
			V.screenshot = argv[++i];
		} else if (strcmp(argv[i], "--create-desktop-entry") == 0) {
			model_init(resources, FALSE);
			return create_desktop_entry(self);
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			g_print("usage: Battlegrounds [--preview] [--screenshot PATH] [--create-desktop-entry] [ISO]\n\n"
			        "Starts the game, or first-time setup when no verified installation exists.\n"
			        "  ISO                     offer setup from this disc image (asks first)\n"
			        "  --create-desktop-entry  add Battlegrounds to your application menu\n"
			        "  --preview               show setup without installing or launching\n"
			        "  --screenshot PATH       save the setup window as a PNG and exit\n");
			return 0;
		} else if (argv[i][0] != '-') initial = argv[i];
	}
	model_init(resources, preview);
	char *ready = preview ? NULL : find_ready_game();

	if (!gtk_init_check(&argc, &argv)) {
		// No display: a verified game can still start; setup needs a window.
		if (ready && engine_present()) {
			char *engine = g_build_filename(M.resources, "bin/openbg", NULL);
			char *data = g_build_filename(ready, "Data", NULL);
			g_mkdir_with_parents(M.home, 0755);
			if (chdir(M.home) == 0) execl(engine, engine, data, (char *)NULL);
			g_printerr("Could not start %s: %s\n", engine, g_strerror(errno));
			return 1;
		}
		g_printerr("Battlegrounds needs a graphical session for first-time setup. To set up from a terminal, run:\n"
		           "  \"%s/bin/worker\" \"%s/bootstrap.sh\" \"%s\" --source /path/to/Battlegrounds.iso\n",
		           M.resources, M.resources, M.home);
		return 1;
	}
	g_set_application_name("Battlegrounds");
	char *icon = g_build_filename(M.resources, "assets/robot-blueprint.png", NULL);
	gtk_window_set_default_icon_from_file(icon, NULL);
	g_free(icon);
	build_window();
	M.changed = refresh;
	M.needs_window = show_window;
	M.hide_window = hide_window;
	M.quit = quit_main;
	g_unix_signal_add(SIGTERM, on_unix_signal, NULL);
	g_unix_signal_add(SIGINT, on_unix_signal, NULL);

	if (preview) {
		show_window();
	} else if (ready && engine_present()) {
		set_string(&M.ready_game, ready);
		M.ready = TRUE;
		launch();
	} else {
		show_window();
		if (!manifest_present()) set_error("The app is incomplete. Rebuild or download a complete copy.");
		GUnixMountMonitor *monitor = g_unix_mount_monitor_get();
		g_signal_connect(monitor, "mounts-changed", G_CALLBACK(on_mounts_changed), NULL);
		scan_discs();
		if (initial) {
			char *path = g_canonicalize_filename(initial, NULL);
			request_install(path);
			g_free(path);
		}
	}
	refresh();
	gtk_main();
	cancel_setup();
	return 0;
}

#endif
