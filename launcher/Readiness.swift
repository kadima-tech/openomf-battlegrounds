import Foundation
import CryptoKit
import Darwin

struct GameManifest: Decodable {
    struct File: Decodable { let path: String; let size: Int; let sha256: String }
    let version: String
    let files: [File]
}

/// Shared with install.py through game-files.json. No remembered "first run"
/// flag: actual files, their patch level and the bundled engine determine readiness.
enum Readiness {
    struct Stamp: Codable, Equatable {
        let path: String
        let sha256: String
        let size: Int
        let modified: String
        let changed: String
        let inode: UInt64
    }
    struct Cache: Codable {
        let root: String
        let files: [Stamp]
    }
    static func verify(_ root: URL, manifest: GameManifest, cacheURL: URL) -> Bool {
        // Foundation rewrites /private/var to /var even after resolving links.
        // Use POSIX realpath, as pathlib.Path.resolve() does in install.py.
        guard let resolved = realpath(root.path, nil) else { return false }
        let canonicalRoot = String(cString: resolved)
        free(resolved)
        let cache = (try? Data(contentsOf: cacheURL)).flatMap { try? JSONDecoder().decode(Cache.self, from: $0) }
        let previous = cache?.root == canonicalRoot ? Dictionary(cache!.files.map { ($0.path, $0) }, uniquingKeysWith: { _, last in last }) : [:]
        var stamps: [Stamp] = []
        for file in manifest.files {
            let url = root.appendingPathComponent(file.path)
            var info = stat()
            guard stat(url.path, &info) == 0, (info.st_mode & S_IFMT) == S_IFREG,
                  info.st_size == file.size else { return false }
            let stamp = Stamp(path: file.path, sha256: file.sha256, size: file.size,
                              modified: "\(info.st_mtimespec.tv_sec):\(info.st_mtimespec.tv_nsec)",
                              changed: "\(info.st_ctimespec.tv_sec):\(info.st_ctimespec.tv_nsec)", inode: info.st_ino)
            stamps.append(stamp)
            if previous[file.path] == stamp { continue }
            guard let stream = try? FileHandle(forReadingFrom: url) else { return false }
            var hasher = SHA256()
            do {
                while let data = try autoreleasepool(invoking: { try stream.read(upToCount: 1_048_576) }), !data.isEmpty {
                    hasher.update(data: data)
                }
                try stream.close()
            } catch { try? stream.close(); return false }
            if hasher.finalize().map({ String(format: "%02x", $0) }).joined() != file.sha256 { return false }
        }
        guard !manifest.files.isEmpty else { return false }
        // The cache is an optimization for ordinary local file changes, not an
        // authenticity boundary. A changed inode, size, mtime, ctime or expected
        // digest requires hashing again. Never cache a failed verification.
        if let data = try? JSONEncoder().encode(Cache(root: canonicalRoot, files: stamps)) {
            try? FileManager.default.createDirectory(at: cacheURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? data.write(to: cacheURL, options: .atomic)
        }
        return true
    }
}
