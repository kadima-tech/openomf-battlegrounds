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
    }
}
