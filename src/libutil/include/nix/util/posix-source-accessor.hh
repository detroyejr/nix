#pragma once

#include "nix/util/source-accessor.hh"

namespace nix {

struct SourcePath;

/**
 * A source accessor that uses the Unix filesystem.
 */
struct PosixSourceAccessor : virtual SourceAccessor
{
    /**
     * Optional root path to prefix all operations into the native file
     * system. This allows prepending funny things like `C:\` that
     * `CanonPath` intentionally doesn't support.
     */
    const std::filesystem::path root;

    PosixSourceAccessor();
    PosixSourceAccessor(std::filesystem::path && root);

    /**
     * The most recent mtime seen by lstat(). This is a hack to
     * support dumpPathAndGetMtime(). Should remove this eventually.
     */
    time_t mtime = 0;

    void readFile(const CanonPath & path, Sink & sink, std::function<void(uint64_t)> sizeCallback) override;

    bool pathExists(const CanonPath & path) override;

    std::optional<Stat> maybeLstat(const CanonPath & path) override;

    DirEntries readDirectory(const CanonPath & path) override;

    std::string readLink(const CanonPath & path) override;

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;

    /**
     * Create a `PosixSourceAccessor` and `SourcePath` corresponding to
     * some native path.
     *
     * The `PosixSourceAccessor` is rooted as far up the tree as
     * possible, (e.g. on Windows it could scoped to a drive like
     * `C:\`). This allows more `..` parent accessing to work.
     *
     * @note When `path` is trusted user input, canonicalize it using
     * `std::filesystem::canonical`, `makeParentCanonical`, `std::filesystem::weakly_canonical`, etc,
     * as appropriate for the use case. At least weak canonicalization is
     * required for the `SourcePath` to do anything useful at the location it
     * points to.
     *
     * @note A canonicalizing behavior is not built in `createAtRoot` so that
     * callers do not accidentally introduce symlink-related security vulnerabilities.
     * Furthermore, `createAtRoot` does not know whether the file pointed to by
     * `path` should be resolved if it is itself a symlink. In other words,
     * `createAtRoot` can not decide between aforementioned `canonical`, `makeParentCanonical`, etc. for its callers.
     *
     * See
     * [`std::filesystem::path::root_path`](https://en.cppreference.com/w/cpp/filesystem/path/root_path)
     * and
     * [`std::filesystem::path::relative_path`](https://en.cppreference.com/w/cpp/filesystem/path/relative_path).
     */
    static SourcePath createAtRoot(const std::filesystem::path & path);

private:

    /**
     * Throw an error if `path` or any of its ancestors are symlinks.
     */
    void assertNoSymlinks(CanonPath path);

    std::optional<struct stat> cachedLstat(const CanonPath & path);

    std::filesystem::path makeAbsPath(const CanonPath & path);
};

} // namespace nix
