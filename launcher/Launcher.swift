import AppKit
import SwiftUI
import CryptoKit
import UniformTypeIdentifiers
import Darwin

private let amber = Color(red: 1, green: 0.71, blue: 0.16)
private let ivory = Color(red: 0.91, green: 0.89, blue: 0.83)
private let muted = Color(red: 0.53, green: 0.56, blue: 0.55)
private let bayPanel = Color(red: 0.078, green: 0.09, blue: 0.094)


struct ModEntry: Equatable {
    var path: String
    var enabled: Bool
}

struct ModInfo: Equatable {
    var exists = false
    var size: Int64 = 0
    var adds: [String] = []
    var invalid = false
}

/// The player's mod list, read by the engine itself (engine/src/de/mods.h): mods.txt in the
/// engine's settings folder, "on <path>" / "off <path>" lines in load order, then the unlisted
/// add-ons of the "mods" folder beside it, enabled, in name order.
enum ModConfig {
    static func settingsFolder(_ environment: [String: String] = ProcessInfo.processInfo.environment) -> URL {
        let home = environment["HOME"].map { URL(fileURLWithPath: $0) } ?? FileManager.default.homeDirectoryForCurrentUser
        return home.appendingPathComponent("Library/Application Support/openbg")
    }

    static func key(_ path: String) -> String {
        URL(fileURLWithPath: path).standardizedFileURL.resolvingSymlinksInPath().path
    }

    static func parse(_ text: String, base: URL) -> [ModEntry] {
        var out: [ModEntry] = [], seen = Set<String>()
        for raw in text.split(omittingEmptySubsequences: false, whereSeparator: { $0 == "\n" || $0 == "\r\n" }) {
            var line = String(raw)
            if line.hasSuffix("\r") { line.removeLast() }
            let on = line.hasPrefix("on "), off = line.hasPrefix("off ")
            guard on || off else { continue }
            let value = String(line.dropFirst(on ? 3 : 4))
            guard !value.isEmpty else { continue }
            let path = (value.hasPrefix("/") ? URL(fileURLWithPath: value) : base.appendingPathComponent(value)).standardizedFileURL.path
            if seen.insert(key(path)).inserted { out.append(ModEntry(path: path, enabled: on)) }
        }
        return out
    }

    static func format(_ mods: [ModEntry]) -> String {
        "# OpenBG mods, in load order: \"on\" or \"off\", a space, the add-on file.\n"
            + mods.map { "\($0.enabled ? "on" : "off") \($0.path)\n" }.joined()
    }

    static func folderMods(_ folder: URL) -> [String] {
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.pathExtension.lowercased() == "deobjects" }
            .map(\.standardizedFileURL.path).sorted { ($0 as NSString).lastPathComponent.lowercased() < ($1 as NSString).lastPathComponent.lowercased() }
    }

    static func read(_ file: URL) -> [ModEntry] {
        let base = file.deletingLastPathComponent()
        var mods = (try? String(contentsOf: file, encoding: .utf8)).map { parse($0, base: base) } ?? []
        var seen = Set(mods.map { key($0.path) })
        for path in folderMods(base.appendingPathComponent("mods")) where seen.insert(key(path)).inserted {
            mods.append(ModEntry(path: path, enabled: true))
        }
        return mods
    }

    static func write(_ mods: [ModEntry], to file: URL) throws {
        try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        try format(mods).write(to: file, atomically: true, encoding: .utf8)
    }

    /// In a source checkout: the add-ons mods/<name>/build.sh wrote.
    static func checkoutMods(_ project: URL) -> [String] {
        let folder = project.appendingPathComponent("mods")
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        return names.sorted().flatMap { folderMods(folder.appendingPathComponent($0).appendingPathComponent("build")) }
    }

    /// The problem that keeps an enabled mod from loading, if any.
    static func problem(_ mods: [ModEntry], _ index: Int, info: ModInfo?) -> String? {
        guard let info else { return nil }
        if !info.exists { return "File not found. Remove it, or put the file back." }
        if info.invalid { return "Not a Battlegrounds add-on (.DEObjects)." }
        let name = (mods[index].path as NSString).lastPathComponent.lowercased()
        if mods[index].enabled, let first = mods[..<index].firstIndex(where: {
            $0.enabled && ($0.path as NSString).lastPathComponent.lowercased() == name
        }) {
            return String(format: "Same file name as mod %02d, so only that one loads.", first + 1)
        }
        return nil
    }
}

struct Stage {
    let title: String
    var detail: String
    var status = "waiting"
    let symbol: String
}

@MainActor final class LauncherModel: ObservableObject {
    @Published var stages = [
        Stage(title: "Game files", detail: "From your original CD or ISO", symbol: "doc.on.doc"),
        Stage(title: "Bundled update", detail: "Updated offline to version 2.123", symbol: "wrench.and.screwdriver"),
        Stage(title: "Local setup", detail: "Everything installed in your game folder", symbol: "shippingbox")
    ]
    @Published var busy = false
    @Published var ready = false
    @Published var error: String?
    @Published var disc: URL?
    @Published var pendingSource: URL?
    @Published var sourceName: String?
    @Published var dropping = false
    @Published var showHelp = false
    @Published var showSettings = false
    @Published var configuring = false
    @Published var mods: [ModEntry] = []
    @Published var modInfo: [String: ModInfo] = [:]
    let modsFile = ModConfig.settingsFolder().appendingPathComponent("mods.txt")
    var trash: (URL) throws -> Void = { try FileManager.default.trashItem(at: $0, resultingItemURL: nil) }
    let resources: URL
    let project: URL?
    let home: URL
    let preview: Bool
    var readyGame: URL?
    var worker: Process?
    var game: Process?
    var lastSource: URL?
    var window: NSWindow?
    var onNeedsWindow: (() -> Void)?
    private var mountObserver: NSObjectProtocol?

    init() {
        resources = Bundle.main.resourceURL!
        preview = CommandLine.arguments.contains("--preview") || CommandLine.arguments.contains("--screenshot")
        let env = ProcessInfo.processInfo.environment
        let adjacent = Bundle.main.bundleURL.deletingLastPathComponent().deletingLastPathComponent()
        if let path = env["OPENBG_PROJECT_ROOT"] {
            project = URL(fileURLWithPath: path)
        } else if FileManager.default.fileExists(atPath: adjacent.appendingPathComponent("engine/CMakeLists.txt").path) {
            project = adjacent
        } else { project = nil }
        if let path = env["OPENBG_HOME"] {
            home = URL(fileURLWithPath: path)
        } else if let project {
            home = project.appendingPathComponent("source/launcher")
        } else {
            let parent = Bundle.main.bundleURL.deletingLastPathComponent()
            if FileManager.default.isWritableFile(atPath: parent.path), !parent.path.hasPrefix("/Applications") {
                home = parent.appendingPathComponent("Battlegrounds Files")
            } else {
                home = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/OpenBG")
            }
        }
    }

    func start() {
        if configuring { loadMods() }
        if preview && !configuring { onNeedsWindow?(); return }
        let resources = resources, home = home, project = project
        DispatchQueue.global(qos: .userInitiated).async {
            let manifest = try? JSONDecoder().decode(GameManifest.self, from: Data(contentsOf: resources.appendingPathComponent("game-files.json")))
            var candidates = [home.appendingPathComponent("game")]
            if let project { candidates.append(project.appendingPathComponent("source/patched/v2123")) }
            let installed = manifest.flatMap { m in candidates.first {
                Readiness.verify($0, manifest: m, cacheURL: home.appendingPathComponent("verified-files.json"))
            } }
            DispatchQueue.main.async {
                if let installed, FileManager.default.isExecutableFile(atPath: resources.appendingPathComponent("bin/openbg").path) {
                    self.readyGame = installed
                    self.ready = true
                    if self.configuring { self.onNeedsWindow?() } else { self.launch() }
                } else {
                    self.onNeedsWindow?()
                    if manifest == nil { self.error = "The app is incomplete. Rebuild or download a complete copy." }
                    self.watchDiscs()
                }
            }
        }
    }

    static func setupOnDisc(_ root: URL) -> Bool {
        func child(_ root: URL, _ name: String) -> URL? {
            (try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil))?
                .first { $0.lastPathComponent.lowercased() == name.lowercased() }
        }
        guard let setup = child(root, "Setup"), let ini = child(setup, "setup.ini"),
              let text = try? String(contentsOf: ini, encoding: .utf8),
              text.lowercased().contains("b8ef8b5a-6d15-436c-ac07-5f2bd5e1aa1b") else { return false }
        return ["data1.cab", "data1.hdr", "data2.cab"].allSatisfy { child(setup, $0) != nil }
    }

    func watchDiscs() {
        mountObserver = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didMountNotification, object: nil, queue: .main
        ) { [weak self] _ in Task { @MainActor in self?.scanDiscs() } }
        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didUnmountNotification, object: nil, queue: .main
        ) { [weak self] _ in Task { @MainActor in self?.scanDiscs() } }
        scanDiscs()
    }

    func scanDiscs(volumes: [URL]? = nil) {
        guard !preview, !busy, !ready else { return }
        let volumes = volumes ?? FileManager.default.mountedVolumeURLs(includingResourceValuesForKeys: nil, options: []) ?? []
        disc = volumes.first { Self.setupOnDisc($0) }
    }

    func chooseISO() {
        let picker = NSOpenPanel()
        picker.title = "Choose your Battlegrounds disc image"
        picker.allowedContentTypes = [UTType(filenameExtension: "iso") ?? .diskImage]
        picker.allowsMultipleSelection = false
        if picker.runModal() == .OK, let url = picker.url { requestInstall(url) }
    }

    func useCD() {
        scanDiscs()
        if !busy, let disc { requestInstall(disc) }
    }

    func requestInstall(_ source: URL) {
        guard !busy, !preview, !ready else { return }
        if !source.hasDirectoryPath && source.pathExtension.lowercased() != "iso" {
            error = "Choose an ISO disc image, or insert your original Battlegrounds CD."; return
        }
        pendingSource = source
    }

    func confirmInstall() {
        guard let source = pendingSource else { return }
        pendingSource = nil
        install(source)
    }

    private func install(_ source: URL) {
        guard !busy, !preview, !ready else { return }
        lastSource = source
        sourceName = source.lastPathComponent
        error = nil
        busy = true
        for index in stages.indices { stages[index].status = "waiting" }
        do {
            try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
            let process = Process(), output = Pipe()
            process.executableURL = resources.appendingPathComponent("bin/worker")
            process.arguments = [resources.appendingPathComponent("bootstrap.sh").path, home.path, "--source", source.path]
            if let project { process.arguments! += ["--project", project.path] }
            process.standardOutput = output
            process.standardError = FileHandle.standardError
            worker = process
            try process.run()
            // Read whole lines off the main thread, including partial pipe reads.
            DispatchQueue.global(qos: .userInitiated).async {
                var pending = Data()
                while true {
                    let data = output.fileHandleForReading.availableData
                    if data.isEmpty { break }
                    pending.append(data)
                    while let end = pending.firstIndex(of: 10) {
                        let line = pending[..<end]
                        if let event = try? JSONSerialization.jsonObject(with: line) as? [String: String] {
                            DispatchQueue.main.async { self.receive(event) }
                        }
                        pending.removeSubrange(...end)
                    }
                }
                process.waitUntilExit()
                DispatchQueue.main.async {
                    self.busy = false
                    self.worker = nil
                    if !self.ready && self.error == nil {
                        self.error = "Setup stopped before it finished. Check your disc and free space, then retry. Details are in the setup log."
                    }
                }
            }
        } catch {
            busy = false; worker = nil; self.error = error.localizedDescription
        }
    }

    func receive(_ event: [String: String]) {
        guard let stage = event["stage"] else { return }
        if stage == "error" { error = event["detail"]; return }
        if stage == "complete" {
            ready = true; readyGame = home.appendingPathComponent("game"); busy = false
            sourceName = "Installation complete. Your next launch goes straight to the game."
            launch()
        } else if let index = ["files", "patches", "runtime"].firstIndex(of: stage) {
            stages[index].status = event["status"] ?? "waiting"
            stages[index].detail = event["detail"] ?? ""
        }
    }

    func launch() {
        guard let root = readyGame else { return }
        do {
            try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
            let process = Process()
            process.executableURL = resources.appendingPathComponent("bin/openbg")
            process.arguments = [root.appendingPathComponent("Data").path]
            if let extra = ProcessInfo.processInfo.environment["OPENBG_GAME_ARGS"] {
                process.arguments! += extra.split(separator: " ").map(String.init)
            }
            process.currentDirectoryURL = home
            let log = home.appendingPathComponent("game.log")
            FileManager.default.createFile(atPath: log.path, contents: nil)
            let handle = try FileHandle(forWritingTo: log)
            process.standardOutput = handle; process.standardError = handle
            process.terminationHandler = { process in
                DispatchQueue.main.async {
                    if process.terminationStatus == 0 { NSApp.terminate(nil) }
                    else {
                        self.game = nil
                        self.onNeedsWindow?()
                        self.error = "The game could not start (code \(process.terminationStatus)). Open the game log for details."
                    }
                }
            }
            try process.run()
            game = process
            window?.orderOut(nil)
        } catch {
            onNeedsWindow?(); self.error = "The game could not start: \(error.localizedDescription)"
        }
    }

    var modsFolder: URL { modsFile.deletingLastPathComponent().appendingPathComponent("mods") }

    func configure() {
        loadMods()
        configuring = true
    }

    func loadMods() {
        var list = ModConfig.read(modsFile)
        if let project {
            var seen = Set(list.map { ModConfig.key($0.path) })
            for path in ModConfig.checkoutMods(project) where seen.insert(ModConfig.key(path)).inserted {
                list.append(ModEntry(path: path, enabled: false))
            }
        }
        mods = list
        inspectMods()
    }

    /// Size, presence and contents (`openbg --addon-info`) of each listed file, off the main thread.
    func inspectMods() {
        let paths = mods.map(\.path), engine = resources.appendingPathComponent("bin/openbg")
        DispatchQueue.global(qos: .userInitiated).async {
            var found = [ModInfo](repeating: ModInfo(), count: paths.count)
            found.withUnsafeMutableBufferPointer { results in DispatchQueue.concurrentPerform(iterations: paths.count) { index in
                let path = paths[index]
                var info = ModInfo()
                if let attributes = try? FileManager.default.attributesOfItem(atPath: path),
                   attributes[.type] as? FileAttributeType == .typeRegular {
                    info.exists = true
                    info.size = (attributes[.size] as? NSNumber)?.int64Value ?? 0
                    if FileManager.default.isExecutableFile(atPath: engine.path) {
                        let process = Process(), output = Pipe()
                        process.executableURL = engine
                        process.arguments = ["--addon-info", path]
                        process.standardOutput = output
                        process.standardError = FileHandle.nullDevice
                        if (try? process.run()) != nil {
                            let data = output.fileHandleForReading.readDataToEndOfFile()
                            process.waitUntilExit()
                            info.invalid = process.terminationStatus != 0
                            info.adds = String(decoding: data, as: UTF8.self).split(separator: "\n").map {
                                $0.replacingOccurrences(of: "\t", with: " ")
                            }
                        }
                    }
                }
                results[index] = info
            } }
            DispatchQueue.main.async { self.modInfo.merge(zip(paths, found)) { $1 } }
        }
    }

    func saveMods() {
        do { try ModConfig.write(mods, to: modsFile) } catch {
            self.error = "Your mod list could not be saved: \(error.localizedDescription)"
        }
    }

    func toggleMod(_ index: Int) {
        mods[index].enabled.toggle()
        saveMods()
    }

    func moveMod(_ index: Int, by offset: Int) {
        let target = index + offset
        guard mods.indices.contains(index), mods.indices.contains(target) else { return }
        mods.swapAt(index, target)
        saveMods()
    }

    /// A file in the mods folder would come back on the next read, so it goes to the Trash.
    func removeMod(_ index: Int) {
        let entry = mods.remove(at: index)
        let file = URL(fileURLWithPath: entry.path)
        if ModConfig.key(file.deletingLastPathComponent().path) == ModConfig.key(modsFolder.path),
           FileManager.default.fileExists(atPath: file.path) {
            do { try trash(file) } catch {
                mods.insert(ModEntry(path: entry.path, enabled: false), at: index)
                self.error = "\(file.lastPathComponent) could not be moved to the Trash: \(error.localizedDescription)"
            }
        }
        saveMods()
    }

    func addMod(_ url: URL) {
        let path = url.standardizedFileURL.path
        if let index = mods.firstIndex(where: { ModConfig.key($0.path) == ModConfig.key(path) }) {
            mods[index].enabled = true
        } else {
            mods.append(ModEntry(path: path, enabled: true))
        }
        saveMods()
        inspectMods()
    }

    func chooseMod() {
        let picker = NSOpenPanel()
        picker.title = "Add a Battlegrounds mod"
        picker.message = "Choose add-on files (.DEObjects) made with the content editor."
        picker.allowedContentTypes = [UTType(filenameExtension: "DEObjects") ?? .data]
        picker.allowsMultipleSelection = true
        if picker.runModal() == .OK { picker.urls.forEach(addMod) }
    }

    func openModsFolder() {
        try? FileManager.default.createDirectory(at: modsFolder, withIntermediateDirectories: true)
        NSWorkspace.shared.open(modsFolder)
    }

    var activeMods: Int {
        mods.indices.filter { mods[$0].enabled && ModConfig.problem(mods, $0, info: modInfo[mods[$0].path]) == nil }.count
    }

    func revealFolder() { NSWorkspace.shared.open(home) }
    func openLog() {
        let name = ready ? "game.log" : (FileManager.default.fileExists(atPath: home.appendingPathComponent("install.log").path) ? "install.log" : "bootstrap.log")
        NSWorkspace.shared.open(home.appendingPathComponent(name))
    }
    func cancel() {
        if let worker, worker.isRunning { kill(-worker.processIdentifier, SIGTERM) }
    }
}

struct HazardStripe: View {
    var body: some View {
        GeometryReader { geometry in
            Path { path in
                for x in stride(from: -30.0, to: geometry.size.width + 30, by: 26) {
                    path.move(to: CGPoint(x: x, y: 0)); path.addLine(to: CGPoint(x: x + 13, y: 0))
                    path.addLine(to: CGPoint(x: x - 3, y: geometry.size.height))
                    path.addLine(to: CGPoint(x: x - 16, y: geometry.size.height)); path.closeSubpath()
                }
            }.fill(amber.opacity(0.55))
        }.clipped().frame(height: 10)
    }
}

struct LauncherView: View {
    @ObservedObject var model: LauncherModel
    @State private var help = false
    var body: some View {
        HStack(spacing: 0) {
            sidebar.frame(width: 265)
            Rectangle().fill(amber.opacity(0.5)).frame(width: 1)
            VStack(alignment: .leading, spacing: 0) {
                header
                ScrollView {
                    VStack(alignment: .leading, spacing: 20) {
                        if model.configuring {
                            modsPanel
                            modActions
                        } else {
                            sourceControls
                            stagePanel
                        }
                        if let error = model.error { errorPanel(error) }
                        launchButton
                    }.padding(.horizontal, 38).padding(.top, 20).padding(.bottom, 18)
                }
                footer
            }.background(bayPanel)
        }
        .foregroundStyle(ivory)
        .background(Color.black)
        .preferredColorScheme(.dark)
        .frame(minWidth: 1000, minHeight: 700)
        .sheet(isPresented: $model.showHelp) { helpPanel }
        .sheet(isPresented: $model.showSettings) { settingsPanel }
        .alert("Set up Battlegrounds?", isPresented: Binding(
            get: { model.pendingSource != nil },
            set: { if !$0 { model.pendingSource = nil } }
        )) {
            Button("Cancel", role: .cancel) { model.pendingSource = nil }
            Button("Start setup") { model.confirmInstall() }
        } message: {
            Text("Setup will install \(model.pendingSource?.lastPathComponent ?? "your game disc") using the bundled update and tools. It works offline and needs at least 3 GB of free space in \(model.home.path).")
        }
    }

    var sidebar: some View {
        VStack(alignment: .leading, spacing: 0) {
            Rectangle().fill(amber.opacity(0.65)).frame(height: 1).padding(.top, 31)
            Text("ONE MUST FALL").font(.custom("DINCondensed-Bold", size: 38)).tracking(-0.4).lineLimit(1).minimumScaleFactor(0.7).padding(.top, 11)
            Text("B A T T L E G R O U N D S").font(.system(size: 10, weight: .semibold)).foregroundStyle(amber).padding(.top, 7)
            HStack(spacing: 9) {
                Rectangle().frame(height: 1)
                Text("OPEN ENGINE").font(.system(size: 8, design: .monospaced)).tracking(2).fixedSize()
                Rectangle().frame(height: 1)
            }.foregroundStyle(muted).padding(.top, 21)
            ZStack {
                Canvas { context, size in
                    var grid = Path()
                    for x in stride(from: 0.0, through: size.width, by: 24) { grid.move(to: CGPoint(x: x, y: 0)); grid.addLine(to: CGPoint(x: x, y: size.height)) }
                    for y in stride(from: 0.0, through: size.height, by: 24) { grid.move(to: CGPoint(x: 0, y: y)); grid.addLine(to: CGPoint(x: size.width, y: y)) }
                    context.stroke(grid, with: .color(ivory.opacity(0.04)), lineWidth: 0.5)
                }
                if let image = NSImage(contentsOf: model.resources.appendingPathComponent("assets/robot-blueprint.png")) {
                    Image(nsImage: image).resizable().scaledToFit().scaleEffect(1.15).blendMode(.lighten)
                }
            }.clipped().padding(.top, 30).padding(.bottom, 20)
            HStack(alignment: .firstTextBaseline) {
                Text("ARENA SYSTEMS").foregroundStyle(muted)
                Spacer()
                Text("2.123").foregroundStyle(amber)
            }.font(.system(size: 9, design: .monospaced)).tracking(1)
            Text("COMBAT CHASSIS / STANDBY").font(.system(size: 8, design: .monospaced)).tracking(1).foregroundStyle(muted.opacity(0.7)).padding(.top, 8)
            HazardStripe().padding(.top, 24).padding(.bottom, 22)
        }.padding(.horizontal, 25)
            .background(LinearGradient(colors: [Color(white: 0.10), Color(white: 0.055)], startPoint: .topLeading, endPoint: .bottomTrailing))
            .accessibilityElement(children: .ignore).accessibilityLabel("One Must Fall: Battlegrounds. Open engine, version 2.123.")
    }

    var header: some View {
        VStack(alignment: .leading, spacing: 13) {
            HStack(spacing: 7) {
                Circle().fill(amber).frame(width: 5, height: 5)
                Text(model.configuring ? "MOD CONFIGURATION" : model.ready ? "ARENA SYSTEMS ONLINE" : "FIRST-TIME SETUP").tracking(2.4)
                Spacer()
                Text(model.configuring ? "SERVICE BAY  /  02" : "SERVICE BAY  /  01").foregroundStyle(muted).tracking(1.5)
            }.font(.system(size: 9, weight: .medium, design: .monospaced)).foregroundStyle(amber)
            Text(model.configuring ? "Fit out your arena." : model.ready ? "You're ready for the arena." : "Prepare for the arena.")
                .font(.custom("DINCondensed-Bold", size: 44)).tracking(-0.5).minimumScaleFactor(0.8).lineLimit(1)
            Text(model.configuring ? "Choose the mods to load and their order. Every launch uses this list."
                 : model.busy ? "Setting things up. This may take a few minutes." : "Choose your game disc or ISO. We'll handle the rest.")
                .font(.system(size: 16)).foregroundStyle(ivory.opacity(0.65))
        }.padding(.horizontal, 38).padding(.top, 26).padding(.bottom, 10)
    }

    var sourceControls: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 15) {
                Button(action: model.chooseISO) {
                    VStack(spacing: 12) {
                        Image(systemName: "doc.badge.plus").font(.system(size: 36, weight: .ultraLight))
                        Text("Choose ISO file").font(.custom("DINCondensed-Bold", size: 27))
                        Text("or drop an ISO here").font(.system(size: 12)).foregroundStyle(ivory.opacity(0.5))
                    }.foregroundStyle(amber).frame(maxWidth: .infinity).frame(height: 136)
                        .background(amber.opacity(model.dropping ? 0.10 : 0.025))
                        .overlay(RoundedRectangle(cornerRadius: 2).strokeBorder(amber.opacity(0.8), style: StrokeStyle(lineWidth: 1.5, dash: [7, 5])))
                }.buttonStyle(.plain).disabled(model.busy || model.ready)
                    .onDrop(of: [.fileURL], isTargeted: $model.dropping) { providers in
                        guard !model.busy, !model.ready, let provider = providers.first else { return false }
                        _ = provider.loadObject(ofClass: URL.self) { url, _ in
                            if let url { DispatchQueue.main.async { model.requestInstall(url) } }
                        }
                        return true
                    }
                    .accessibilityHint("Select your original Battlegrounds ISO disc image")
                Button(action: model.useCD) {
                    VStack(spacing: 13) {
                        Image(systemName: "opticaldisc").font(.system(size: 42, weight: .ultraLight))
                        Text("Use game CD").font(.custom("DINCondensed-Bold", size: 24))
                        Text(model.disc == nil ? "Insert your game disc" : "Disc detected · click to set up")
                            .font(.system(size: 11)).foregroundStyle(muted)
                    }.foregroundStyle(model.disc == nil ? ivory.opacity(0.7) : amber)
                        .frame(width: 171, height: 136)
                        .background(ivory.opacity(0.025)).overlay(Rectangle().stroke(ivory.opacity(0.23), lineWidth: 1))
                }.buttonStyle(.plain).disabled(model.busy || model.ready || model.disc == nil)
            }.opacity(model.busy || model.ready ? 0.45 : 1)
            HStack(spacing: 7) {
                Image(systemName: model.sourceName == nil ? "opticaldisc" : "checkmark.circle")
                Text(model.sourceName ?? "Your game CD is detected automatically when inserted.").lineLimit(1).truncationMode(.middle)
            }.font(.system(size: 11)).foregroundStyle(muted)
        }
    }

    var stagePanel: some View {
        VStack(spacing: 0) {
            HStack {
                Text("GAME SETUP").tracking(2)
                Spacer()
                Text("TARGET VERSION").tracking(1.4)
                Text("2.123").foregroundStyle(amber).fontWeight(.semibold)
            }.font(.system(size: 9, design: .monospaced)).foregroundStyle(muted).padding(.bottom, 14)
            ForEach(model.stages.indices, id: \.self) { index in
                let stage = model.stages[index]
                Rectangle().fill(ivory.opacity(0.1)).frame(height: 1)
                HStack(spacing: 15) {
                    Text(String(format: "%02d", index + 1)).font(.system(size: 11, design: .monospaced)).foregroundStyle(muted)
                    ZStack {
                        Circle().stroke(stage.status == "waiting" ? muted.opacity(0.5) : amber.opacity(0.8), lineWidth: 1).frame(width: 25, height: 25)
                        if stage.status == "done" { Image(systemName: "checkmark").font(.system(size: 11, weight: .bold)).foregroundStyle(amber) }
                        if stage.status == "active" { ProgressView().controlSize(.mini).tint(amber) }
                    }
                    VStack(alignment: .leading, spacing: 4) {
                        Text(stage.title).font(.system(size: 13, weight: .semibold))
                        Text(stage.detail).font(.system(size: 11)).foregroundStyle(muted).lineLimit(2)
                    }
                    Spacer(minLength: 3)
                    Text(stage.status == "done" ? "Ready" : stage.status == "active" ? "Working…" : "Waiting")
                        .font(.system(size: 10, design: .monospaced)).foregroundStyle(stage.status == "waiting" ? muted : amber)
                }.padding(.vertical, 11)
                    .accessibilityElement(children: .combine)
            }
        }.padding(16).background(ivory.opacity(0.018)).overlay(Rectangle().stroke(ivory.opacity(0.18), lineWidth: 1))
    }

    var modsPanel: some View {
        VStack(spacing: 0) {
            HStack {
                Text("ADD-ONS").tracking(2)
                Spacer()
                Text("LOAD ORDER").tracking(1.4)
                Text("\(model.activeMods) ACTIVE").foregroundStyle(amber).fontWeight(.semibold)
            }.font(.system(size: 9, design: .monospaced)).foregroundStyle(muted).padding(.bottom, 14)
            if model.mods.isEmpty {
                Rectangle().fill(ivory.opacity(0.1)).frame(height: 1)
                Text("No mods yet. Add a .DEObjects add-on made with the content editor, or put one in your mods folder.")
                    .font(.system(size: 12)).foregroundStyle(muted).frame(maxWidth: .infinity, alignment: .leading).padding(.vertical, 18)
            }
            ForEach(model.mods.indices, id: \.self) { index in
                Rectangle().fill(ivory.opacity(0.1)).frame(height: 1)
                modRow(index)
            }
        }.padding(16).background(ivory.opacity(0.018)).overlay(Rectangle().stroke(ivory.opacity(0.18), lineWidth: 1))
    }

    func modRow(_ index: Int) -> some View {
        let mod = model.mods[index], info = model.modInfo[mod.path]
        let file = URL(fileURLWithPath: mod.path)
        let problem = ModConfig.problem(model.mods, index, info: info)
        let origin = ModConfig.key(file.deletingLastPathComponent().path) == ModConfig.key(model.modsFolder.path) ? "MODS FOLDER"
            : model.project.map { ModConfig.key(mod.path).hasPrefix(ModConfig.key($0.appendingPathComponent("mods").path) + "/") } == true ? "CHECKOUT" : "ADDED"
        var details = [ByteCountFormatter.string(fromByteCount: info?.size ?? 0, countStyle: .file), file.path]
        if info?.exists != true { details.removeFirst() }
        return HStack(alignment: .top, spacing: 15) {
            Text(String(format: "%02d", index + 1)).font(.system(size: 11, design: .monospaced)).foregroundStyle(muted).padding(.top, 5)
            Button { model.toggleMod(index) } label: {
                ZStack {
                    Rectangle().fill(mod.enabled ? amber : Color.clear).frame(width: 18, height: 18)
                    Rectangle().stroke(mod.enabled ? amber : muted.opacity(0.7), lineWidth: 1).frame(width: 18, height: 18)
                    if mod.enabled { Image(systemName: "checkmark").font(.system(size: 10, weight: .heavy)).foregroundStyle(Color.black) }
                }.padding(.top, 2)
            }.buttonStyle(.plain).accessibilityLabel(mod.enabled ? "Disable \(file.lastPathComponent)" : "Enable \(file.lastPathComponent)")
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 8) {
                    Text(file.deletingPathExtension().lastPathComponent).font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(mod.enabled ? ivory : ivory.opacity(0.55))
                    Text(origin).font(.system(size: 8, design: .monospaced)).tracking(1).foregroundStyle(muted)
                        .padding(.horizontal, 5).padding(.vertical, 2).overlay(Rectangle().stroke(muted.opacity(0.4), lineWidth: 1))
                }
                if let adds = info?.adds, !adds.isEmpty {
                    Text("Adds " + adds.joined(separator: " · ")).font(.system(size: 11)).foregroundStyle(ivory.opacity(0.7)).lineLimit(2)
                }
                Text(details.joined(separator: "  ·  ")).font(.system(size: 10, design: .monospaced)).foregroundStyle(muted)
                    .lineLimit(1).truncationMode(.middle).textSelection(.enabled)
                if let problem {
                    Label(problem, systemImage: "exclamationmark.triangle").font(.system(size: 11)).foregroundStyle(amber)
                }
            }
            Spacer(minLength: 3)
            HStack(spacing: 4) {
                modButton("chevron.up", "Move \(file.lastPathComponent) up") { model.moveMod(index, by: -1) }.disabled(index == 0)
                modButton("chevron.down", "Move \(file.lastPathComponent) down") { model.moveMod(index, by: 1) }.disabled(index == model.mods.count - 1)
                modButton("xmark", origin == "MODS FOLDER" ? "Move \(file.lastPathComponent) to the Trash" : "Remove \(file.lastPathComponent) from the list") {
                    model.removeMod(index)
                }
            }
        }.padding(.vertical, 11)
    }

    func modButton(_ symbol: String, _ help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol).font(.system(size: 10, weight: .semibold)).frame(width: 26, height: 24)
                .background(ivory.opacity(0.04)).overlay(Rectangle().stroke(ivory.opacity(0.18), lineWidth: 1))
        }.buttonStyle(.plain).foregroundStyle(ivory.opacity(0.8)).help(help).accessibilityLabel(help)
    }

    var modActions: some View {
        HStack(spacing: 15) {
            Button(action: model.chooseMod) {
                HStack(spacing: 10) {
                    Image(systemName: "plus.square.dashed").font(.system(size: 20, weight: .light))
                    Text("Add mod…").font(.custom("DINCondensed-Bold", size: 22))
                }.foregroundStyle(amber).frame(maxWidth: .infinity).frame(height: 52)
                    .background(amber.opacity(0.025))
                    .overlay(RoundedRectangle(cornerRadius: 2).strokeBorder(amber.opacity(0.8), style: StrokeStyle(lineWidth: 1.5, dash: [7, 5])))
            }.buttonStyle(.plain).accessibilityHint("Choose an add-on file anywhere on your Mac")
            Button(action: model.openModsFolder) {
                HStack(spacing: 9) {
                    Image(systemName: "folder").font(.system(size: 16, weight: .light))
                    Text("Open mods folder").font(.custom("DINCondensed-Bold", size: 20))
                }.foregroundStyle(ivory.opacity(0.75)).frame(width: 220, height: 52)
                    .background(ivory.opacity(0.025)).overlay(Rectangle().stroke(ivory.opacity(0.23), lineWidth: 1))
            }.buttonStyle(.plain).help(model.modsFolder.path)
        }
    }

    func errorPanel(_ message: String) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Label(message, systemImage: "exclamationmark.triangle").font(.system(size: 12)).foregroundStyle(amber).fixedSize(horizontal: false, vertical: true)
            HStack {
                if let source = model.lastSource, !model.ready {
                    Button("Try again") { model.requestInstall(source) }.disabled(model.busy)
                }
                Button("Open setup log", action: model.openLog)
            }.font(.system(size: 11)).buttonStyle(.bordered)
        }.padding(15).frame(maxWidth: .infinity, alignment: .leading).background(amber.opacity(0.06))
            .accessibilityAddTraits(.updatesFrequently)
    }

    var launchTitle: String {
        if model.busy { return "Preparing your game…" }
        guard model.configuring else { return "Launch game" }
        if !model.ready { return "Set up the game first" }
        return model.activeMods == 0 ? "Play without mods" : "Play with \(model.activeMods) mod\(model.activeMods == 1 ? "" : "s")"
    }

    var launchButton: some View {
        Button(action: model.launch) {
            HStack(spacing: 10) {
                Image(systemName: "play.fill").font(.system(size: 14))
                Text(launchTitle).font(.custom("DINCondensed-Bold", size: 24))
            }.frame(maxWidth: .infinity).frame(height: 51)
                .foregroundStyle(model.ready ? Color.black : muted.opacity(0.65))
                .background(model.ready ? amber : ivory.opacity(0.06))
                .overlay(Rectangle().stroke(ivory.opacity(0.14), lineWidth: 1))
        }.buttonStyle(.plain).disabled(!model.ready || model.game != nil)
            .keyboardShortcut(.defaultAction)
    }

    var footer: some View {
        VStack(spacing: 0) {
            Rectangle().fill(ivory.opacity(0.15)).frame(height: 1)
            HStack(spacing: 9) {
                Image(systemName: "folder")
                Text("Everything stays in your game folder.").font(.system(size: 10))
                Spacer()
                if model.configuring {
                    Button { model.configuring = false } label: { Label(model.ready ? "Game" : "Setup", systemImage: "opticaldisc") }
                } else {
                    Button { model.configure() } label: { Label("Mods", systemImage: "puzzlepiece.extension") }
                }
                Rectangle().fill(muted.opacity(0.3)).frame(width: 1, height: 16).padding(.horizontal, 6)
                Button { model.showSettings = true } label: { Label("Settings", systemImage: "gearshape") }
                Rectangle().fill(muted.opacity(0.3)).frame(width: 1, height: 16).padding(.horizontal, 6)
                Button { model.showHelp = true } label: { Label("Help", systemImage: "questionmark.circle") }
            }.buttonStyle(.plain).font(.system(size: 11)).foregroundStyle(muted).padding(.horizontal, 30).padding(.vertical, 20)
        }
    }

    var helpPanel: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("Getting arena-ready").font(.title2.bold()).foregroundStyle(ivory)
            Text("Choose an ISO of your original English Battlegrounds 2.090 disc, or insert the CD and click Use game CD. An inserted game CD is detected automatically. Confirm Start setup to begin. Setup uses the update and tools bundled with this app, then verifies the finished installation.")
            Text("When everything is ready, the game starts. Next time, this screen is skipped automatically. Your original disc and ISO are never changed.")
            Text("First-time setup works offline and needs 3 GB of free space. No administrator password or system-wide packages are required.")
            Text("To choose mods, hold Option while opening Battlegrounds, or click Mods below. The game remembers your list.")
            Button("Done") { model.showHelp = false }.keyboardShortcut(.defaultAction)
        }.font(.system(size: 14)).foregroundStyle(muted).padding(30).frame(width: 480).background(bayPanel)
    }

    var settingsPanel: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("Your game folder").font(.title2.bold()).foregroundStyle(ivory)
            Text(model.home.path).font(.system(size: 12, design: .monospaced)).textSelection(.enabled)
            Text("Your extracted and updated game files stay here. Setup tools and the update are included in the app. Nothing is added to your shell or system package manager. A verified installation opens directly into the game.")
            HStack {
                Button("Open folder", action: model.revealFolder)
                Spacer()
                Button("Done") { model.showSettings = false }.keyboardShortcut(.defaultAction)
            }
        }.font(.system(size: 14)).foregroundStyle(muted).padding(30).frame(width: 480).background(bayPanel)
    }
}

@MainActor final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let model = LauncherModel()
    var window: NSWindow?
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        model.configuring = CommandLine.arguments.contains("--configure")
            || !NSEvent.modifierFlags.intersection([.option, .shift]).isEmpty
        model.onNeedsWindow = { [weak self] in self?.showWindow() }
        model.start()
        let menu = NSMenu()
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Quit Battlegrounds", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let item = NSMenuItem(); item.submenu = appMenu; menu.addItem(item); NSApp.mainMenu = menu
    }
    func showWindow() {
        if let window { window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true); return }
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1160, height: 780),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.title = "One Must Fall: Battlegrounds"
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.backgroundColor = NSColor(red: 0.078, green: 0.09, blue: 0.094, alpha: 1)
        window.minSize = NSSize(width: 1000, height: 730)
        window.contentView = NSHostingView(rootView: LauncherView(model: model).padding(.top, 22).background(bayPanel))
        window.delegate = self
        window.isReleasedWhenClosed = false
        window.center(); window.makeKeyAndOrderFront(nil)
        self.window = window; model.window = window
        NSApp.activate(ignoringOtherApps: true)
        if let index = CommandLine.arguments.firstIndex(of: "--screenshot"), CommandLine.arguments.count > index + 1 {
            let output = CommandLine.arguments[index + 1]
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                guard let view = window.contentView, let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { NSApp.terminate(nil); return }
                view.cacheDisplay(in: view.bounds, to: bitmap)
                try? bitmap.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: output))
                NSApp.terminate(nil)
            }
        }
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if model.busy {
            let alert = NSAlert()
            alert.messageText = "Stop setting up the game?"
            alert.informativeText = "Your original files are safe. You can restart setup whenever you're ready."
            alert.addButton(withTitle: "Keep setting up"); alert.addButton(withTitle: "Stop setup")
            if alert.runModal() == .alertFirstButtonReturn { return .terminateCancel }
            model.cancel()
        }
        return .terminateNow
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool { NSApp.terminate(nil); return false }
}

#if !LAUNCHER_MODEL_TESTS
@main struct Launcher {
    @MainActor static func main() {
        let delegate = AppDelegate()
        NSApplication.shared.delegate = delegate
        withExtendedLifetime(delegate) { NSApplication.shared.run() }
    }
}
#endif
