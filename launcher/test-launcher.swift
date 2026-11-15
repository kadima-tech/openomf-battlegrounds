import Foundation

@main struct LauncherTests {
    @MainActor static func main() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let home = root.appendingPathComponent("private-state")
        setenv("OPENBG_HOME", home.path, 1)
        let model = LauncherModel()
        let setup = root.appendingPathComponent("Setup", isDirectory: true)
        try FileManager.default.createDirectory(at: setup, withIntermediateDirectories: true)
        try "ProductGUID=b8ef8b5a-6d15-436c-ac07-5f2bd5e1aa1b".write(
            to: setup.appendingPathComponent("setup.ini"), atomically: true, encoding: .utf8)
        for name in ["data1.cab", "data1.hdr", "data2.cab"] {
            try Data().write(to: setup.appendingPathComponent(name))
        }
        let disc = URL(fileURLWithPath: root.path, isDirectory: true)
        func expectIdle() {
            precondition(!model.busy && model.worker == nil, "No setup process before confirmation")
            precondition(!FileManager.default.fileExists(atPath: home.path), "No setup writes before confirmation")
        }
        model.scanDiscs(volumes: [disc])
        precondition(model.disc == disc && model.pendingSource == nil, "Detection must only advertise the disc")
        expectIdle()
        model.requestInstall(disc)
        precondition(model.pendingSource == disc, "CD selection must request confirmation")
        expectIdle()
        model.pendingSource = nil // Cancel the confirmation.
        model.scanDiscs(volumes: [])
        precondition(model.disc == nil, "Unmount must clear the detected disc")
        model.scanDiscs(volumes: [disc])
        expectIdle()
        precondition(model.pendingSource == nil, "Remount after cancellation must not start setup")
        model.confirmInstall()
        expectIdle()
        let iso = root.appendingPathComponent("game.iso")
        model.requestInstall(iso)
        precondition(model.pendingSource == iso, "ISO selection must request confirmation")
        expectIdle()
        model.confirmInstall()
        // This test executable has no bundled worker. Reaching the error proves
        // only confirmation enters setup, without downloads or extraction.
        precondition(model.pendingSource == nil && model.error != nil)
        precondition(FileManager.default.fileExists(atPath: home.path), "Confirmation must enter setup")
        print("Launcher consent checks passed (detection, selection, cancellation, remount, confirmation)")
        try modChecks(root)
    }

    @MainActor static func modChecks(_ root: URL) throws {
        let fm = FileManager.default
        let user = root.appendingPathComponent("user home")
        setenv("HOME", user.path, 1)
        let settings = ModConfig.settingsFolder()
        precondition(settings.path == user.appendingPathComponent("Library/Application Support/openbg").path,
                     "The mod list lives in the engine's settings folder")
        let folder = settings.appendingPathComponent("mods"), elsewhere = root.appendingPathComponent("elsewhere")
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        try fm.createDirectory(at: elsewhere, withIntermediateDirectories: true)
        for url in [folder.appendingPathComponent("Beta.DEObjects"), folder.appendingPathComponent("alpha.deobjects"),
                    elsewhere.appendingPathComponent("Zed.DEObjects"), elsewhere.appendingPathComponent("Off.DEObjects")] {
            try Data("add-on".utf8).write(to: url)
        }
        try Data().write(to: folder.appendingPathComponent("notes.txt"))
        let file = settings.appendingPathComponent("mods.txt")
        precondition(ModConfig.read(root.appendingPathComponent("none/mods.txt")).isEmpty, "No list, no folder: no mods")
        try "# comment\r\non \(elsewhere.path)/Zed.DEObjects\r\noff ../../../../elsewhere/Off.DEObjects\nmaybe x\non mods/Beta.DEObjects\non \(elsewhere.path)/Zed.DEObjects\n"
            .write(to: file, atomically: true, encoding: .utf8)
        let mods = ModConfig.read(file)
        let names = mods.map { ($0.path as NSString).lastPathComponent + ($0.enabled ? "+" : "-") }
        precondition(names == ["Zed.DEObjects+", "Off.DEObjects-", "Beta.DEObjects+", "alpha.deobjects+"],
                     "List order, relative paths, duplicates once, then the folder's unlisted files enabled: \(names)")
        try ModConfig.write(mods, to: file)
        precondition(ModConfig.read(file) == mods, "Written lists read back the same: \(ModConfig.read(file)) \(mods)")
        let text = try String(contentsOf: file, encoding: .utf8)
        precondition(text.contains("\noff \(elsewhere.path)/Off.DEObjects\n"), "Lines are on/off and the absolute path")
        var info = ModInfo(exists: true, size: 6, adds: [], invalid: false)
        precondition(ModConfig.problem(mods, 0, info: info) == nil, "A present add-on has no problem")
        info.exists = false
        precondition(ModConfig.problem(mods, 0, info: info)?.contains("not found") == true, "Missing files are flagged")
        let twins = [ModEntry(path: "/a/Kreissack.DEObjects", enabled: true), ModEntry(path: "/b/kreissack.DEObjects", enabled: true)]
        precondition(ModConfig.problem(twins, 1, info: ModInfo(exists: true))?.contains("mod 01") == true, "Name clashes are flagged")

        let model = LauncherModel()
        precondition(model.modsFile == file, "The model edits the shared list")
        model.trash = { try fm.removeItem(at: $0) }
        model.loadMods()
        precondition(model.mods == mods, "The model shows the list")
        model.toggleMod(1)
        model.moveMod(1, by: -1)
        model.moveMod(0, by: -1)
        precondition(ModConfig.read(file).map(\.enabled) == [true, true, true, true]
                     && (ModConfig.read(file)[0].path as NSString).lastPathComponent == "Off.DEObjects", "Toggles and moves are saved")
        model.addMod(root.appendingPathComponent("New.DEObjects"))
        precondition(ModConfig.read(file).last?.path == root.appendingPathComponent("New.DEObjects").path, "Added files join the end")
        model.removeMod(model.mods.count - 1)
        model.removeMod(model.mods.firstIndex { $0.path.hasSuffix("Beta.DEObjects") }!)
        let after = ModConfig.read(file).map { ($0.path as NSString).lastPathComponent }
        precondition(after == ["Off.DEObjects", "Zed.DEObjects", "alpha.deobjects"] && !fm.fileExists(atPath: folder.appendingPathComponent("Beta.DEObjects").path),
                     "Removing forgets an added file and moves a mods folder file away: \(after)")
        print("Mod list checks passed (format, folder, order, problems, toggle, move, add, remove)")
    }
}
