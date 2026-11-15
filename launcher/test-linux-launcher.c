// Linux launcher model checks; no display, game data or bundled tools needed.
// Build and run commands: launcher/README.md#validation.
#define LAUNCHER_MODEL_TESTS 1
#pragma GCC diagnostic ignored "-Wunused-function"  // UI-only helpers are compiled but not exercised.
#include "linux-launcher.c"

#define CHECK(condition, message) do { \
	if (!(condition)) { g_printerr("FAILED: %s (%s:%d)\n", message, __FILE__, __LINE__); exit(1); } \
} while (0)

static void write_file(const char *dir, const char *name, const char *text) {
	char *path = g_build_filename(dir, name, NULL);
	CHECK(g_file_set_contents(path, text, -1, NULL), "write test file");
	g_free(path);
}

static void test_events(void) {
	GHashTable *event = parse_event(
		"{\"stage\": \"patches\", \"status\": \"active\", \"detail\": \"Applying bundled update \\u00b7 3/76 files\\u2026\"}");
	CHECK(event, "install.py events parse");
	CHECK(strcmp(g_hash_table_lookup(event, "stage"), "patches") == 0, "stage member");
	CHECK(strcmp(g_hash_table_lookup(event, "detail"), "Applying bundled update · 3/76 files…") == 0, "\\u escapes decode");
	g_hash_table_unref(event);
	event = parse_event("{\"detail\":\"quote \\\" slash \\\\ \\ud83e\\udd16 tab\\t\"}");
	CHECK(event && strcmp(g_hash_table_lookup(event, "detail"), "quote \" slash \\ 🤖 tab\t") == 0, "escapes and surrogate pairs");
	g_hash_table_unref(event);
	CHECK(!parse_event("not json"), "plain text is ignored");
	CHECK(!parse_event("{\"stage\": 1}"), "non-string members are rejected");
	CHECK(!parse_event("{\"stage\": \"files\""), "truncated objects are rejected");
	CHECK(!parse_event("{\"stage\": \"bad \\x\"}"), "unknown escapes are rejected");
}

static void test_mount_candidates(void) {
	CHECK(candidate_mount("/home/me/iso", "iso9660"), "ISO 9660 mounts anywhere");
	CHECK(candidate_mount("/srv/disc", "udf"), "UDF mounts anywhere");
	CHECK(candidate_mount("/run/media/me/OMFBG", "fuseblk"), "desktop removable media");
	CHECK(candidate_mount("/media/cdrom", "auto"), "classic /media mounts");
	CHECK(!candidate_mount("/", "ext4"), "root file system is never scanned");
	CHECK(!candidate_mount("/net/server", "nfs"), "network shares are never scanned");
}

static void test_home_locations(const char *root) {
	char *app = g_build_filename(root, "Downloads/Battlegrounds", NULL);
	char *resources = g_build_filename(app, "resources", NULL);
	g_mkdir_with_parents(resources, 0755);
	g_unsetenv("OPENBG_HOME");
	g_unsetenv("OPENBG_PROJECT_ROOT");
	model_init(resources, FALSE);
	char *expected = g_build_filename(root, "Downloads/Battlegrounds Files", NULL);
	CHECK(!M.project && strcmp(M.home, expected) == 0, "a writable folder keeps game files next to the app");
	g_free(expected);
	char *project = g_build_filename(root, "checkout", NULL);
	char *marker = g_build_filename(project, "engine", NULL);
	g_mkdir_with_parents(marker, 0755);
	write_file(marker, "CMakeLists.txt", "");
	char *checkout_resources = g_build_filename(project, "dist/Battlegrounds/resources", NULL);
	g_mkdir_with_parents(checkout_resources, 0755);
	model_init(checkout_resources, FALSE);
	expected = g_build_filename(project, "source/launcher", NULL);
	CHECK(M.project && strcmp(M.project, project) == 0 && strcmp(M.home, expected) == 0, "a source checkout uses source/launcher");
	g_free(expected);
	model_init("/usr/lib/battlegrounds/resources", FALSE);
	expected = g_build_filename(g_get_user_data_dir(), "openbg", NULL);
	CHECK(strcmp(M.home, expected) == 0, "system installs use the XDG data folder");
	g_free(expected);
	g_free(checkout_resources);
	g_free(marker);
	g_free(project);
	g_free(resources);
	g_free(app);
}

static void test_consent(const char *root) {
	char *home = g_build_filename(root, "private-state", NULL);
	g_setenv("OPENBG_HOME", home, TRUE);
	char *resources = g_build_filename(root, "no-bundled-tools", NULL);
	model_init(resources, FALSE);
	char *disc = g_build_filename(root, "disc", NULL);
	char *setup = g_build_filename(disc, "SETUP", NULL);
	g_mkdir_with_parents(setup, 0755);
	write_file(setup, "Setup.INI", "ProductGUID=B8EF8B5A-6D15-436C-AC07-5F2BD5E1AA1B");
	write_file(setup, "DATA1.CAB", "");
	write_file(setup, "data1.hdr", "");
	CHECK(!setup_on_disc(disc), "an incomplete disc is not offered");
	write_file(setup, "Data2.cab", "");
	CHECK(setup_on_disc(disc), "disc detection is case-insensitive and checks the product");

#define EXPECT_IDLE() do { \
	CHECK(!M.busy && !M.worker, "no setup process before confirmation"); \
	CHECK(!g_file_test(home, G_FILE_TEST_EXISTS), "no setup writes before confirmation"); \
} while (0)
	GPtrArray *volumes = g_ptr_array_new();
	g_ptr_array_add(volumes, disc);
	GPtrArray *none = g_ptr_array_new();
	scan_volumes(volumes);
	CHECK(M.disc && strcmp(M.disc, disc) == 0 && !M.pending_source, "detection only advertises the disc");
	EXPECT_IDLE();
	request_install(disc);
	CHECK(M.pending_source && strcmp(M.pending_source, disc) == 0, "CD selection requests confirmation");
	EXPECT_IDLE();
	cancel_pending();
	scan_volumes(none);
	CHECK(!M.disc, "unmounting clears the detected disc");
	scan_volumes(volumes);
	CHECK(!M.pending_source, "remounting after cancellation does not start setup");
	EXPECT_IDLE();
	confirm_install();
	EXPECT_IDLE();
	char *other = g_build_filename(root, "readme.txt", NULL);
	request_install(other);
	CHECK(!M.pending_source && M.error, "other files are refused");
	g_clear_pointer(&M.error, g_free);
	char *iso = g_build_filename(root, "game.ISO", NULL);
	request_install(iso);
	CHECK(M.pending_source && strcmp(M.pending_source, iso) == 0, "ISO selection requests confirmation");
	EXPECT_IDLE();
	confirm_install();
	// This test has no bundled worker. Reaching the error proves only confirmation enters setup.
	CHECK(!M.pending_source && M.error && !M.busy, "confirmation enters setup");
	CHECK(g_file_test(home, G_FILE_TEST_IS_DIR), "confirmation creates the private game folder");
	g_ptr_array_unref(volumes);
	g_ptr_array_unref(none);
	g_free(iso);
	g_free(other);
	g_free(setup);
	g_free(disc);
	g_free(resources);
	g_free(home);
}

static void test_events_drive_stages(void) {
	GHashTable *event = parse_event("{\"stage\":\"files\",\"status\":\"done\",\"detail\":\"Original game files extracted\"}");
	receive(event);
	g_hash_table_unref(event);
	CHECK(strcmp(M.stages[0].status, "done") == 0 && strcmp(M.stages[0].detail, "Original game files extracted") == 0,
	      "stage events update their row");
	event = parse_event("{\"stage\":\"error\",\"status\":\"error\",\"detail\":\"Game file needs repair: Data/X\"}");
	receive(event);
	g_hash_table_unref(event);
	CHECK(M.error && strcmp(M.error, "Game file needs repair: Data/X") == 0, "error events are shown");
}

static gboolean test_trash(const char *path) { return g_remove(path) == 0; }

static const char *mod_name(guint i) { return strrchr(((ModEntry *)M.mods->pdata[i])->path, '/') + 1; }

static void test_mods(const char *root) {
	char *user = g_build_filename(root, "user home", NULL);
	g_setenv("HOME", user, TRUE);
	g_unsetenv("XDG_DATA_HOME");
	char *settings = mods_settings_folder();
	char *expected = g_build_filename(user, ".local/share/openbg", NULL);
	CHECK(strcmp(settings, expected) == 0, "the mod list lives in the engine's settings folder");
	char *folder = g_build_filename(settings, "mods", NULL), *elsewhere = g_build_filename(root, "elsewhere", NULL);
	g_mkdir_with_parents(folder, 0755);
	g_mkdir_with_parents(elsewhere, 0755);
	write_file(folder, "Beta.DEObjects", "add-on");
	write_file(folder, "alpha.deobjects", "add-on");
	write_file(folder, "notes.txt", "");
	write_file(elsewhere, "Zed.DEObjects", "add-on");
	write_file(elsewhere, "Off.DEObjects", "add-on");
	char *file = g_build_filename(settings, "mods.txt", NULL);
	GPtrArray *none = mods_read("/nonexistent/openbg/mods.txt");
	CHECK(none->len == 0, "no list and no folder: no mods");
	g_ptr_array_unref(none);
	char *text = g_strdup_printf("# comment\r\non %s/Zed.DEObjects\r\noff ../../../../elsewhere/Off.DEObjects\nmaybe x\n"
	                             "on mods/Beta.DEObjects\non %s/Zed.DEObjects\n", elsewhere, elsewhere);
	write_file(settings, "mods.txt", text);
	g_free(text);
	model_init("/nonexistent/resources", FALSE);
	CHECK(strcmp(M.mods_file, file) == 0, "the model edits the shared list");
	M.trash = test_trash;
	load_mods();
	CHECK(M.mods->len == 4 && strcmp(mod_name(0), "Zed.DEObjects") == 0 && strcmp(mod_name(1), "Off.DEObjects") == 0 &&
	      !((ModEntry *)M.mods->pdata[1])->enabled && strcmp(mod_name(2), "Beta.DEObjects") == 0 &&
	      strcmp(mod_name(3), "alpha.deobjects") == 0 && ((ModEntry *)M.mods->pdata[3])->enabled,
	      "list order, relative paths, duplicates once, then the folder's unlisted files enabled");
	char *off = g_build_filename(elsewhere, "Off.DEObjects", NULL);
	CHECK(strcmp(((ModEntry *)M.mods->pdata[1])->path, off) == 0, "relative paths are resolved from the list's folder");
	save_mods();
	char *written = NULL;
	CHECK(g_file_get_contents(file, &written, NULL, NULL), "the list is written");
	char *line = g_strdup_printf("\noff %s\n", off);
	CHECK(strstr(written, line) != NULL, "lines are on/off and the absolute path");
	GPtrArray *again = mods_read(file);
	CHECK(again->len == 4 && strcmp(((ModEntry *)again->pdata[3])->path, ((ModEntry *)M.mods->pdata[3])->path) == 0,
	      "written lists read back the same");
	g_ptr_array_unref(again);
	ModInfo present = {TRUE, FALSE, 6, NULL}, missing = {FALSE, FALSE, 0, NULL};
	char *problem = mod_problem(M.mods, 0, &present);
	CHECK(!problem, "a present add-on has no problem");
	problem = mod_problem(M.mods, 0, &missing);
	CHECK(problem && strstr(problem, "not found"), "missing files are flagged");
	g_free(problem);
	GPtrArray *twins = mods_new();
	mods_add(twins, "/a/Kreissack.DEObjects", TRUE);
	mods_add(twins, "/b/kreissack.DEObjects", TRUE);
	problem = mod_problem(twins, 1, &present);
	CHECK(problem && strstr(problem, "mod 01"), "name clashes are flagged");
	g_free(problem);
	g_ptr_array_unref(twins);

	toggle_mod(1);
	move_mod(1, -1);
	move_mod(0, -1);
	GPtrArray *saved = mods_read(file);
	CHECK(strcmp(strrchr(((ModEntry *)saved->pdata[0])->path, '/') + 1, "Off.DEObjects") == 0 &&
	      ((ModEntry *)saved->pdata[0])->enabled, "toggles and moves are saved");
	g_ptr_array_unref(saved);
	char *added = g_build_filename(root, "New.DEObjects", NULL);
	add_mod(added);
	CHECK(M.mods->len == 5 && strcmp(((ModEntry *)M.mods->pdata[4])->path, added) == 0, "added files join the end");
	remove_mod(4);
	remove_mod(2);  // Beta, in the mods folder
	saved = mods_read(file);
	char *beta = g_build_filename(folder, "Beta.DEObjects", NULL);
	CHECK(saved->len == 3 && strcmp(strrchr(((ModEntry *)saved->pdata[2])->path, '/') + 1, "alpha.deobjects") == 0 &&
	      !g_file_test(beta, G_FILE_TEST_EXISTS), "removing forgets an added file and moves a mods folder file away");
	g_ptr_array_unref(saved);
	g_free(beta);
	g_free(added);
	g_free(line);
	g_free(written);
	g_free(off);
	g_free(file);
	g_free(elsewhere);
	g_free(folder);
	g_free(expected);
	g_free(settings);
	g_free(user);
}

int main(void) {
	char *root = g_dir_make_tmp("openbg-launcher-XXXXXX", NULL);
	CHECK(root, "temporary folder");
	test_events();
	test_mount_candidates();
	test_home_locations(root);
	test_consent(root);
	test_events_drive_stages();
	test_mods(root);
	char *command = g_strdup_printf("rm -rf '%s'", root);
	if (system(command) != 0) g_printerr("could not remove %s\n", root);
	g_free(command);
	g_free(root);
	g_print("Linux launcher checks passed (events, disc detection, locations, consent, mod list)\n");
	return 0;
}
