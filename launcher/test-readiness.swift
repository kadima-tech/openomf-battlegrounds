import Foundation
import CryptoKit

@main struct ReadinessTests {
    static func main() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let file = root.appendingPathComponent("game-file")
        let cache = root.appendingPathComponent("verified.json")
        let good = Data("patched".utf8)
        let hash = SHA256.hash(data: good).map { String(format: "%02x", $0) }.joined()
        let manifest = GameManifest(version: "2.123", files: [.init(path: "game-file", size: good.count, sha256: hash)])
        try good.write(to: file)
        precondition(Readiness.verify(root, manifest: manifest, cacheURL: cache), "Initial checksums must pass")
        precondition(FileManager.default.fileExists(atPath: cache.path), "Successful verification must save its cache")
        precondition(Readiness.verify(root, manifest: manifest, cacheURL: cache), "Unchanged verified files must be ready")

        // Exercise the actual Python writer, including /var -> /private/var
        // aliases on macOS. Both implementations must share the same cache.
        let native = try JSONDecoder().decode(Readiness.Cache.self, from: Data(contentsOf: cache))
        let writer = Process()
        writer.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        writer.arguments = ["python3", "-c", """
        import sys
        sys.path.insert(0, sys.argv[1])
        import install
        from pathlib import Path
        root = Path(sys.argv[2])
        install.record_verified(root, {"files": [{"path": "game-file", "size": 7,
            "sha256": install.digest(root / "game-file")}]}, Path(sys.argv[3]))
        """, URL(fileURLWithPath: #filePath).deletingLastPathComponent().path, root.path, cache.path]
        try writer.run()
        writer.waitUntilExit()
        precondition(writer.terminationStatus == 0, "Python cache writer must run successfully")
        let python = try JSONDecoder().decode(Readiness.Cache.self, from: Data(contentsOf: cache))
        precondition(native.root == python.root, "Python and Swift must use the same canonical root")
        precondition(native.files == python.files, "Python and Swift file identities must match")
        precondition(Readiness.verify(root, manifest: manifest, cacheURL: cache), "Python cache must be accepted by Swift")

        try Data("damaged".utf8).write(to: file)
        precondition(!Readiness.verify(root, manifest: manifest, cacheURL: cache), "A same-size edit must invalidate readiness")
        try good.write(to: file)
        precondition(Readiness.verify(root, manifest: manifest, cacheURL: cache), "Restored files must be reverified")

        let differentPatch = GameManifest(version: "next", files: [.init(path: "game-file", size: good.count, sha256: String(repeating: "0", count: 64))])
        precondition(!Readiness.verify(root, manifest: differentPatch, cacheURL: cache), "Expected checksum changes must invalidate the cache")
        try Data("invalid json".utf8).write(to: cache)
        precondition(Readiness.verify(root, manifest: manifest, cacheURL: cache), "A malformed cache must fall back to real checksums")
        try FileManager.default.removeItem(at: file)
        precondition(!Readiness.verify(root, manifest: manifest, cacheURL: cache), "Missing game files must not count as ready")
        print("Native readiness checks passed (verification, shared Python cache, same-size edits, patch changes, corrupt cache, missing files)")
    }
}
