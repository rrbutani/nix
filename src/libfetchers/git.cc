#include "fetchers.hh"
#include "cache.hh"
#include "globals.hh"
#include "tarfile.hh"
#include "store-api.hh"
#include "url-parts.hh"
#include "pathlocks.hh"
#include "util.hh"
#include "git.hh"
#include "fs-input-accessor.hh"
#include "git-utils.hh"

#include "fetch-settings.hh"

#include <regex>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>

using namespace std::string_literals;

namespace nix::fetchers {

namespace {

// Explicit initial branch of our bare repo to suppress warnings from new version of git.
// The value itself does not matter, since we always fetch a specific revision or branch.
// It is set with `-c init.defaultBranch=` instead of `--initial-branch=` to stay compatible with
// old version of git, which will ignore unrecognized `-c` options.
const std::string gitInitialBranch = "__nix_dummy_branch";

bool isCacheFileWithinTtl(time_t now, const struct stat & st)
{
    return st.st_mtime + settings.tarballTtl > now;
}

bool touchCacheFile(const Path & path, time_t touch_time)
{
    struct timeval times[2];
    times[0].tv_sec = touch_time;
    times[0].tv_usec = 0;
    times[1].tv_sec = touch_time;
    times[1].tv_usec = 0;

    return lutimes(path.c_str(), times) == 0;
}

Path getCachePath(std::string_view key)
{
    return getCacheDir() + "/nix/gitv4/" +
        hashString(htSHA256, key).to_string(Base32, false);
}

std::string getNumJobs(void) {
    if (settings.maxBuildJobs.get() == 0)
        return "1";
    else return settings.maxBuildJobs.to_string();
}

// Returns the name of the HEAD branch.
//
// Returns the head branch name as reported by git ls-remote --symref, e.g., if
// ls-remote returns the output below, "main" is returned based on the ref line.
//
//   ref: refs/heads/main       HEAD
//   ...
std::optional<std::string> readHead(const Path & path)
{
    auto [status, output] = runProgram(RunOptions {
        .program = "git",
        // FIXME: use 'HEAD' to avoid returning all refs
        .args = {"ls-remote", "--symref", path},
    });
    if (status != 0) return std::nullopt;

    std::string_view line = output;
    line = line.substr(0, line.find("\n"));
    if (const auto parseResult = git::parseLsRemoteLine(line)) {
        switch (parseResult->kind) {
            case git::LsRemoteRefLine::Kind::Symbolic:
                debug("resolved HEAD ref '%s' for repo '%s'", parseResult->target, path);
                break;
            case git::LsRemoteRefLine::Kind::Object:
                debug("resolved HEAD rev '%s' for repo '%s'", parseResult->target, path);
                break;
        }
        return parseResult->target;
    }
    return std::nullopt;
}

// Persist the HEAD ref from the remote repo in the local cached repo.
bool storeCachedHead(const std::string & actualUrl, const std::string & headRef)
{
    Path cacheDir = getCachePath(actualUrl);
    try {
        runProgram("git", true, { "-C", cacheDir, "--git-dir", ".", "symbolic-ref", "--", "HEAD", headRef });
    } catch (ExecError &e) {
        if (!WIFEXITED(e.status)) throw;
        return false;
    }
    /* No need to touch refs/HEAD, because `git symbolic-ref` updates the mtime. */
    return true;
}

std::optional<std::string> readHeadCached(const std::string & actualUrl)
{
    // Create a cache path to store the branch of the HEAD ref. Append something
    // in front of the URL to prevent collision with the repository itself.
    Path cacheDir = getCachePath(actualUrl);
    Path headRefFile = cacheDir + "/HEAD";

    time_t now = time(0);
    struct stat st;
    std::optional<std::string> cachedRef;
    if (stat(headRefFile.c_str(), &st) == 0) {
        cachedRef = readHead(cacheDir);
        if (cachedRef != std::nullopt &&
            *cachedRef != gitInitialBranch &&
            isCacheFileWithinTtl(now, st))
        {
            debug("using cached HEAD ref '%s' for repo '%s'", *cachedRef, actualUrl);
            return cachedRef;
        }
    }

    auto ref = readHead(actualUrl);
    if (ref) return ref;

    if (cachedRef) {
        // If the cached git ref is expired in fetch() below, and the 'git fetch'
        // fails, it falls back to continuing with the most recent version.
        // This function must behave the same way, so we return the expired
        // cached ref here.
        warn("could not get HEAD ref for repository '%s'; using expired cached ref '%s'", actualUrl, *cachedRef);
        return *cachedRef;
    }

    return std::nullopt;
}

bool isNotDotGitDirectory(const Path & path)
{
    return baseNameOf(path) != ".git";
}

}  // end namespace

struct GitInputScheme : InputScheme
{
    std::optional<Input> inputFromURL(const ParsedURL & url) const override
    {
        if (url.scheme != "git" &&
            url.scheme != "git+http" &&
            url.scheme != "git+https" &&
            url.scheme != "git+ssh" &&
            url.scheme != "git+file") return {};

        auto url2(url);
        if (hasPrefix(url2.scheme, "git+")) url2.scheme = std::string(url2.scheme, 4);
        url2.query.clear();

        Attrs attrs;
        attrs.emplace("type", "git");

        for (auto & [name, value] : url.query) {
            if (name == "rev" || name == "ref")
                attrs.emplace(name, value);
            else if (name == "shallow" || name == "submodules")
                attrs.emplace(name, Explicit<bool> { value == "1" });
            else
                url2.query.emplace(name, value);
        }

        attrs.emplace("url", url2.to_string());

        return inputFromAttrs(attrs);
    }

    std::optional<Input> inputFromAttrs(const Attrs & attrs) const override
    {
        if (maybeGetStrAttr(attrs, "type") != "git") return {};

        for (auto & [name, value] : attrs)
            if (name != "type" && name != "url" && name != "ref" && name != "rev" && name != "shallow" && name != "submodules" && name != "lastModified" && name != "revCount" && name != "narHash" && name != "allRefs" && name != "name")
                throw Error("unsupported Git input attribute '%s'", name);

        parseURL(getStrAttr(attrs, "url"));
        maybeGetBoolAttr(attrs, "shallow");
        maybeGetBoolAttr(attrs, "submodules");
        maybeGetBoolAttr(attrs, "allRefs");

        if (auto ref = maybeGetStrAttr(attrs, "ref")) {
            if (std::regex_search(*ref, badGitRefRegex))
                throw BadURL("invalid Git branch/tag name '%s'", *ref);
        }

        Input input;
        input.attrs = attrs;
        return input;
    }

    ParsedURL toURL(const Input & input) const override
    {
        auto url = parseURL(getStrAttr(input.attrs, "url"));
        if (url.scheme != "git") url.scheme = "git+" + url.scheme;
        if (auto rev = input.getRev()) url.query.insert_or_assign("rev", rev->gitRev());
        if (auto ref = input.getRef()) url.query.insert_or_assign("ref", *ref);
        if (maybeGetBoolAttr(input.attrs, "shallow").value_or(false))
            url.query.insert_or_assign("shallow", "1");
        return url;
    }

    Input applyOverrides(
        const Input & input,
        std::optional<std::string> ref,
        std::optional<Hash> rev) const override
    {
        auto res(input);
        if (rev) res.attrs.insert_or_assign("rev", rev->gitRev());
        if (ref) res.attrs.insert_or_assign("ref", *ref);
        if (!res.getRef() && res.getRev())
            throw Error("Git input '%s' has a commit hash but no branch/tag name", res.to_string());
        return res;
    }

    void clone(const Input & input, const Path & destDir) const override
    {
        auto repoInfo = getRepoInfo(input);

        Strings args = {"clone"};

        args.push_back(repoInfo.url);

        if (auto ref = input.getRef()) {
            args.push_back("--branch");
            args.push_back(*ref);
        }

        if (input.getRev()) throw UnimplementedError("cloning a specific revision is not implemented");

        args.push_back(destDir);

        runProgram("git", true, args);
    }

    void putFile(
        const Input & input,
        const CanonPath & path,
        std::string_view contents,
        std::optional<std::string> commitMsg) const
    {
        auto repoInfo = getRepoInfo(input);
        if (!repoInfo.isLocal)
            throw Error("cannot commit '%s' to Git repository '%s' because it's not a working tree", path, input.to_string());

        auto absPath = CanonPath(repoInfo.url) + path;

        // FIXME: make sure that absPath is not a symlink that escapes
        // the repo.
        writeFile(absPath.abs(), contents);

        runProgram("git", true,
            { "-C", repoInfo.url, "--git-dir", repoInfo.gitDir, "add", "--intent-to-add", "--", std::string(path.rel()) });

        if (commitMsg)
            runProgram("git", true,
                { "-C", repoInfo.url, "--git-dir", repoInfo.gitDir, "commit", std::string(path.rel()), "-m", *commitMsg });
    }

    struct RepoInfo
    {
        bool shallow = false;
        bool submodules = false;
        bool allRefs = false;

        std::string cacheType;

        /* Whether this is a local, non-bare repository. */
        bool isLocal = false;

        /* Whether this is a local, non-bare, dirty repository. */
        bool isDirty = false;

        /* Whether this repository has any commits. */
        bool hasHead = true;

        /* URL of the repo, or its path if isLocal. */
        std::string url;

        void warnDirty() const
        {
            if (isDirty) {
                if (!fetchSettings.allowDirty)
                    throw Error("Git tree '%s' is dirty", url);

                if (fetchSettings.warnDirty)
                    warn("Git tree '%s' is dirty", url);
            }
        }

        std::string gitDir = ".git";
    };

    bool getSubmodulesAttr(const Input & input) const
    {
        return maybeGetBoolAttr(input.attrs, "submodules").value_or(false);
    }

    RepoInfo getRepoInfo(const Input & input) const
    {
        auto checkHashType = [&](const std::optional<Hash> & hash)
        {
            if (hash.has_value() && !(hash->type == htSHA1 || hash->type == htSHA256))
                throw Error("Hash '%s' is not supported by Git. Supported types are sha1 and sha256.", hash->to_string(Base16, true));
        };

        if (auto rev = input.getRev())
            checkHashType(rev);

        RepoInfo repoInfo {
            .shallow = maybeGetBoolAttr(input.attrs, "shallow").value_or(false),
            .submodules = getSubmodulesAttr(input),
            .allRefs = maybeGetBoolAttr(input.attrs, "allRefs").value_or(false)
        };

        repoInfo.cacheType = "git";
        if (repoInfo.shallow) repoInfo.cacheType += "-shallow";
        if (repoInfo.submodules) repoInfo.cacheType += "-submodules";
        if (repoInfo.allRefs) repoInfo.cacheType += "-all-refs";

        // file:// URIs are normally not cloned (but otherwise treated the
        // same as remote URIs, i.e. we don't use the working tree or
        // HEAD). Exception: If _NIX_FORCE_HTTP is set, or the repo is a bare git
        // repo, treat as a remote URI to force a clone.
        static bool forceHttp = getEnv("_NIX_FORCE_HTTP") == "1"; // for testing
        auto url = parseURL(getStrAttr(input.attrs, "url"));
        bool isBareRepository = url.scheme == "file" && !pathExists(url.path + "/.git");
        repoInfo.isLocal = url.scheme == "file" && !forceHttp && !isBareRepository;
        repoInfo.url = repoInfo.isLocal ? url.path : url.base;

        // If this is a local directory and no ref or revision is
        // given, then allow the use of an unclean working tree.
        if (!input.getRef() && !input.getRev() && repoInfo.isLocal) {
            repoInfo.isDirty = true;

            auto env = getEnv();
            /* Set LC_ALL to C: because we rely on the error messages
               from git rev-parse to determine what went wrong that
               way unknown errors can lead to a failure instead of
               continuing through the wrong code path. */
            env["LC_ALL"] = "C";

            /* Check whether HEAD points to something that looks like
               a commit, since that is the ref we want to use later
               on. */
            auto result = runProgram(RunOptions {
                .program = "git",
                .args = { "-C", repoInfo.url, "--git-dir", repoInfo.gitDir, "rev-parse", "--verify", "--no-revs", "HEAD^{commit}" },
                .environment = env,
                .mergeStderrToStdout = true
            });
            auto exitCode = WEXITSTATUS(result.first);
            auto errorMessage = result.second;

            if (errorMessage.find("fatal: not a git repository") != std::string::npos) {
                throw Error("'%s' is not a Git repository", repoInfo.url);
            } else if (errorMessage.find("fatal: Needed a single revision") != std::string::npos) {
                // indicates that the repo does not have any commits
                // we want to proceed and will consider it dirty later
            } else if (exitCode != 0) {
                // any other errors should lead to a failure
                throw Error("getting the HEAD of the Git tree '%s' failed with exit code %d:\n%s", repoInfo.url, exitCode, errorMessage);
            }

            repoInfo.hasHead = exitCode == 0;

            try {
                if (repoInfo.hasHead) {
                    // Using git diff is preferrable over lower-level operations here,
                    // because it's conceptually simpler and we only need the exit code anyways.
                    auto gitDiffOpts = Strings({ "-C", repoInfo.url, "--git-dir", repoInfo.gitDir, "diff", "HEAD", "--quiet"});
                    if (!repoInfo.submodules) {
                        // Changes in submodules should only make the tree dirty
                        // when those submodules will be copied as well.
                        gitDiffOpts.emplace_back("--ignore-submodules");
                    }
                    gitDiffOpts.emplace_back("--");
                    runProgram("git", true, gitDiffOpts);

                    repoInfo.isDirty = false;
                }
            } catch (ExecError & e) {
                if (!WIFEXITED(e.status) || WEXITSTATUS(e.status) != 1) throw;
            }
        }

        return repoInfo;
    }

    std::set<CanonPath> listFiles(const RepoInfo & repoInfo) const
    {
        auto gitOpts = Strings({ "-C", repoInfo.url, "--git-dir", repoInfo.gitDir, "ls-files", "-z" });
        if (repoInfo.submodules)
            gitOpts.emplace_back("--recurse-submodules");

        std::set<CanonPath> res;

        for (auto & p : tokenizeString<std::set<std::string>>(
                runProgram("git", true, gitOpts), "\0"s))
            res.insert(CanonPath(p));

        return res;
    }

    Hash updateRev(Input & input, const RepoInfo & repoInfo, const std::string & ref) const
    {
        if (auto r = input.getRev())
            return *r;
        else {
            auto rev = Hash::parseAny(chomp(runProgram("git", true, { "-C", repoInfo.url, "--git-dir", repoInfo.gitDir, "rev-parse", ref })), htSHA1);
            input.attrs.insert_or_assign("rev", rev.gitRev());
            return rev;
        }
    }

    uint64_t getLastModified(const RepoInfo & repoInfo, const std::string & repoDir, const std::string & ref) const
    {
        return
            repoInfo.hasHead
            ? std::stoull(
                runProgram("git", true,
                    { "-C", repoDir, "--git-dir", repoInfo.gitDir, "log", "-1", "--format=%ct", "--no-show-signature", ref }))
            : 0;
    }

    uint64_t getLastModified(const RepoInfo & repoInfo, const std::string & repoDir, const Hash & rev) const
    {
        if (!repoInfo.hasHead) return 0;

        auto key = fmt("git-%s-last-modified", rev.gitRev());

        auto cache = getCache();

        if (auto lastModifiedS = cache->queryFact(key)) {
            if (auto lastModified = string2Int<uint64_t>(*lastModifiedS))
                return *lastModified;
        }

        auto lastModified = getLastModified(repoInfo, repoDir, rev.gitRev());

        cache->upsertFact(key, std::to_string(lastModified));

        return lastModified;
    }

    uint64_t getRevCount(const RepoInfo & repoInfo, const std::string & repoDir, const Hash & rev) const
    {
        if (!repoInfo.hasHead) return 0;

        auto key = fmt("git-%s-revcount", rev.gitRev());

        auto cache = getCache();

        if (auto revCountS = cache->queryFact(key)) {
            if (auto revCount = string2Int<uint64_t>(*revCountS))
                return *revCount;
        }

        Activity act(*logger, lvlChatty, actUnknown, fmt("getting Git revision count of '%s'", repoInfo.url));

        auto revCount = std::stoull(
            runProgram("git", true,
                { "-C", repoDir, "--git-dir", repoInfo.gitDir, "rev-list", "--count", rev.gitRev() }));

        cache->upsertFact(key, std::to_string(revCount));

        return revCount;
    }

    std::string getDefaultRef(const RepoInfo & repoInfo) const
    {
        auto head = repoInfo.isLocal
            ? readHead(repoInfo.url)
            : readHeadCached(repoInfo.url);
        if (!head) {
            warn("could not read HEAD ref from repo at '%s', using 'master'", repoInfo.url);
            return "master";
        }
        return *head;
    }

    static MakeNotAllowedError makeNotAllowedError(std::string url)
    {
        return [url{std::move(url)}](const CanonPath & path) -> RestrictedPathError
        {
            if (nix::pathExists(path.abs()))
                return RestrictedPathError("access to path '%s' is forbidden because it is not under Git control; maybe you should 'git add' it to the repository '%s'?", path, url);
            else
                return RestrictedPathError("path '%s' does not exist in Git repository '%s'", path, url);
        };
    }

    std::pair<ref<InputAccessor>, Input> getAccessorFromCommit(
        ref<Store> store,
        RepoInfo & repoInfo,
        Input && input) const
    {
        assert(!repoInfo.isDirty);

        auto origRev = input.getRev();

        std::string name = input.getName();

        auto getLockedAttrs = [&]()
        {
            return Attrs({
                {"type", repoInfo.cacheType},
                {"name", name},
                {"rev", input.getRev()->gitRev()},
            });
        };

        auto makeResult2 = [&](const Attrs & infoAttrs, ref<InputAccessor> accessor) -> std::pair<ref<InputAccessor>, Input>
        {
            assert(input.getRev());
            assert(!origRev || origRev == input.getRev());
            if (!repoInfo.shallow)
                input.attrs.insert_or_assign("revCount", getIntAttr(infoAttrs, "revCount"));
            input.attrs.insert_or_assign("lastModified", getIntAttr(infoAttrs, "lastModified"));

            accessor->setPathDisplay("«" + input.to_string() + "»");
            return {accessor, std::move(input)};
        };

        auto makeResult = [&](const Attrs & infoAttrs, const StorePath & storePath) -> std::pair<ref<InputAccessor>, Input>
        {
            // FIXME: remove?
            //input.attrs.erase("narHash");
            auto narHash = store->queryPathInfo(storePath)->narHash;
            input.attrs.insert_or_assign("narHash", narHash.to_string(SRI, true));

            auto accessor = makeStorePathAccessor(store, storePath, makeNotAllowedError(repoInfo.url));

            return makeResult2(infoAttrs, accessor);
        };

        if (input.getRev()) {
            if (auto res = getCache()->lookup(store, getLockedAttrs()))
                return makeResult(res->first, std::move(res->second));
        }

        auto originalRef = input.getRef();
        auto ref = originalRef ? *originalRef : getDefaultRef(repoInfo);
        input.attrs.insert_or_assign("ref", ref);

        Attrs unlockedAttrs({
            {"type", repoInfo.cacheType},
            {"name", name},
            {"url", repoInfo.url},
            {"ref", ref},
        });

        Path repoDir;

        if (repoInfo.isLocal) {
            updateRev(input, repoInfo, ref);
            repoDir = repoInfo.url;
        } else {
            if (auto res = getCache()->lookup(store, unlockedAttrs)) {
                auto rev2 = Hash::parseAny(getStrAttr(res->first, "rev"), htSHA1);
                if (!input.getRev() || input.getRev() == rev2) {
                    input.attrs.insert_or_assign("rev", rev2.gitRev());
                    return makeResult(res->first, std::move(res->second));
                }
            }

            Path cacheDir = getCachePath(repoInfo.url);
            repoDir = cacheDir;
            repoInfo.gitDir = ".";

            createDirs(dirOf(cacheDir));
            PathLocks cacheDirLock({cacheDir + ".lock"});

            if (!pathExists(cacheDir)) {
                runProgram("git", true, { "-c", "init.defaultBranch=" + gitInitialBranch, "init", "--bare", repoDir });
            }

            Path localRefFile =
                ref.compare(0, 5, "refs/") == 0
                ? cacheDir + "/" + ref
                : cacheDir + "/refs/heads/" + ref;

            bool doFetch;
            time_t now = time(0);

            /* If a rev was specified, we need to fetch if it's not in the
               repo. */
            if (input.getRev()) {
                try {
                    runProgram("git", true, { "-C", repoDir, "--git-dir", repoInfo.gitDir, "cat-file", "-e", input.getRev()->gitRev() });
                    doFetch = false;
                } catch (ExecError & e) {
                    if (WIFEXITED(e.status)) {
                        doFetch = true;
                    } else {
                        throw;
                    }
                }
            } else {
                if (repoInfo.allRefs) {
                    doFetch = true;
                } else {
                    /* If the local ref is older than ‘tarball-ttl’ seconds, do a
                       git fetch to update the local ref to the remote ref. */
                    struct stat st;
                    doFetch = stat(localRefFile.c_str(), &st) != 0 ||
                        !isCacheFileWithinTtl(now, st);
                }
            }

            // if we want an unshallow repo but only have a shallow git dir
            // we need to do a fetch:
            {
                bool isShallow = chomp(runProgram("git", true, { "-C", repoDir, "--git-dir", repoInfo.gitDir, "rev-parse", "--is-shallow-repository" })) == "true";
                if (isShallow && !repoInfo.shallow) {
                    doFetch = true;
                }
            }

            // TODO: really should disable GC in the cache git dirs on init...

            if (doFetch) {
                Activity act(*logger, lvlTalkative, actUnknown, fmt("fetching Git repository '%s'", repoInfo.url));

                // FIXME: git stderr messes up our progress indicator, so
                // we're using --quiet for now. Should process its stderr.
                try {
                    auto fetchRef = repoInfo.allRefs
                        ? "refs/*"
                        : ref.compare(0, 5, "refs/") == 0
                            ? ref
                            : ref == "HEAD"
                                ? ref
                                : "refs/heads/" + ref;

                    auto fetchOpts = Strings({
                        "-C", repoDir,
                        "--git-dir", repoInfo.gitDir,
                        "fetch", "--quiet", "--force",
                        "--jobs", getNumJobs()
                    });

                    if (repoInfo.shallow) {
                        fetchOpts.emplace_back("--depth=1");
                    } else {
                        // If the cached git dir is already shallow and we've
                        // been asked to do a full-depth clone, unshallow it.
                        bool isShallow = chomp(runProgram("git", true, { "-C", repoDir, "--git-dir", repoInfo.gitDir, "rev-parse", "--is-shallow-repository" })) == "true";
                        if (isShallow) {
                            fetchOpts.emplace_back("--unshallow");
                        }
                    }

                    // TODO: for shallow clones this will not actually check that
                    // the rev is on the ref...
                    //
                    // also this may modify stuff (problematic for the local source
                    // case)?
                    fetchOpts.insert(fetchOpts.end(), {
                        "--", repoInfo.url,
                        fmt("%s:%s", repoInfo.shallow ? input.getRev()->gitRev() : fetchRef, fetchRef)
                    });
                    runProgram("git", true, fetchOpts);
                } catch (Error & e) {
                    if (!pathExists(localRefFile)) throw;
                    warn("could not update local clone of Git repository '%s'; continuing with the most recent version", repoInfo.url);
                }

                if (!touchCacheFile(localRefFile, now))
                    warn("could not update mtime for file '%s': %s", localRefFile, strerror(errno));
                if (!originalRef && !storeCachedHead(repoInfo.url, ref))
                    warn("could not update cached head '%s' for '%s'", ref, repoInfo.url);
            }

            if (!input.getRev())
                input.attrs.insert_or_assign("rev", Hash::parseAny(chomp(readFile(localRefFile)), htSHA1).gitRev());

            // cache dir lock is removed at scope end; we will only use read-only operations on specific revisions in the remainder
        }

        bool isShallow = chomp(runProgram("git", true, { "-C", repoDir, "--git-dir", repoInfo.gitDir, "rev-parse", "--is-shallow-repository" })) == "true";

        // TODO: incompatibility with existing `nix` versions because if we make
        // a shallow clone older `nix` versions won't know to unshallow it...
        if (isShallow && !repoInfo.shallow)
            throw Error("'%s' is a shallow Git repository, but shallow repositories are only allowed when `shallow = true;` is specified", repoInfo.url);

        // FIXME: check whether rev is an ancestor of ref.

        auto rev = *input.getRev();

        Attrs infoAttrs({
            {"rev", rev.gitRev()},
            {"lastModified", getLastModified(repoInfo, repoDir, rev)},
        });

        if (!repoInfo.shallow)
            infoAttrs.insert_or_assign("revCount",
                getRevCount(repoInfo, repoDir, rev));

        printTalkative("using revision %s of repo '%s'", rev.gitRev(), repoInfo.url);

        /* Now that we know the rev, check again whether we have it in
           the store. */
        if (auto res = getCache()->lookup(store, getLockedAttrs()))
            return makeResult(res->first, std::move(res->second));

        if (!repoInfo.submodules) {
            auto accessor = makeGitInputAccessor(CanonPath(repoDir), rev);
            return makeResult2(infoAttrs, accessor);
        }

        Path tmpDir = createTempDir();
        AutoDelete delTmpDir(tmpDir, true);
        PathFilter filter = defaultPathFilter;

        auto result = runProgram(RunOptions {
            .program = "git",
            .args = { "-C", repoDir, "--git-dir", repoInfo.gitDir, "cat-file", "commit", rev.gitRev() },
            .mergeStderrToStdout = true
        });
        if (WEXITSTATUS(result.first) == 128
            && result.second.find("bad file") != std::string::npos)
        {
            throw Error(
                "Cannot find Git revision '%s' in ref '%s' of repository '%s'! "
                "Please make sure that the " ANSI_BOLD "rev" ANSI_NORMAL " exists on the "
                ANSI_BOLD "ref" ANSI_NORMAL " you've specified or add " ANSI_BOLD
                "allRefs = true;" ANSI_NORMAL " to " ANSI_BOLD "fetchGit" ANSI_NORMAL ".",
                rev.gitRev(),
                ref,
                repoInfo.url
            );
        }

        Activity act(*logger, lvlChatty, actUnknown, fmt("copying Git tree '%s' to the store", input.to_string()));

        if (repoInfo.submodules) {
            // at this point, if our source is a local directory, repoDir points
            // to a dir that we *cannot modify*
            //
            // if our source is external, repoDir points to a cache directory
            // which we can and should modify directly

            // TODO: we only use this tmp dir if we've got a local directory
            // source that we happen to need to update; we should gate the
            // creation of this dir on that use case (a little complicated
            // because of the scoping we want for this dir...)
            //
            // TODO: we should maybe actually create a cache dir and use that
            // instead for local sources (in the event that we do actually end
            // up having to fetch stuff)?
            Path tmpGitDirForLocalSource = createTempDir();
            AutoDelete delTmpGitDir(tmpGitDirForLocalSource, true);

            auto pathToGitFolder = repoDir;
            if (repoInfo.isLocal) {
                // can't modify `repoDir` directly so we use another git dir:
                pathToGitFolder = tmpGitDirForLocalSource;

                // note that we add `repoDir` as a _reference_; this means we
                // will use objects from the local repo but will not modify its
                // object store (i.e. it adds the local dir as an alternate)
                //
                // we also set `submodule.alternateLocation` to `superproject`
                // meaning that it will inherit the alternates of the parent
                // repo
                runProgram("git", true, {
                    "-c", "init.defaultBranch=" + gitInitialBranch,
                    "init", tmpDir,
                    "--separate-git-dir", pathToGitFolder,
                    // https://git-scm.com/docs/git-clone#Documentation/git-clone.txt---reference-if-ableltrepositorygt
                    "--reference", repoDir,
                    // https://github.com/git/git/blob/d15644fe0226af7ffc874572d968598564a230dd/Documentation/config/submodule.txt#L96-L101
                    "-c", "submodule.alternateLocation=superproject"
                });

            } else {
                // TODO: should we disable GC on the cache repos?
                // TODO: locking

                // use `repoDir` directly:
                pathToGitFolder = repoDir + "/" + repoInfo.gitDir;
            }

            /* Ensure that we use the correct origin for fetching
               submodules. This matters for submodules with relative
               URLs. */
            if (repoInfo.isLocal) {
                writeFile(pathToGitFolder + "/config", readFile(repoDir + "/" + repoInfo.gitDir + "/config"));

                /* Restore the config.bare setting we may have just
                   copied erroneously from the user's repo. */
                runProgram("git", true, {
                    "--git-dir", pathToGitFolder,
                    "--work-tree", tmpDir,
                    "config", "core.bare", "false"
                });
            } else {
                runProgram("git", true, {
                    "--git-dir", pathToGitFolder,
                    "--work-tree", tmpDir,
                    "config", "remote.origin.url", repoInfo.url
                });
            };

            // {
            //     // TODO: repoDir might lack the ref (it only checks if rev
            //     // exists, see FIXME above) so use a big hammer and fetch
            //     // everything to ensure we get the rev.
            //     Activity act(*logger, lvlTalkative, actUnknown, fmt("making temporary clone of '%s'", repoDir));
            //     runProgram("git", true, { "-C", tmpDir, "fetch", "--quiet", "--force",
            //             "--update-head-ok", "--", repoDir, "refs/*:refs/*" });
            // }

            // Check out the repo:
            // TODO: logging
            runProgram("git", true, {
                "--git-dir", pathToGitFolder,
                "--work-tree", tmpDir,
                "checkout", "--quiet",
                input.getRev()->gitRev(), ".",
            });

            // /* propagate shallow clone option to submodules */
            // runProgram("git", true, {
            //     "--git-dir", pathToGitFolder,
            //     "--work-tree", tmpDir,
            //     "-C", tmpDir, // necessary for `git-submodule` to work...
            //     "submodule", "foreach", "--recursive",
            //     (shallow
            //         ? "cd $toplevel; git config -f .gitmodules submodule.$sm_path.shallow true"
            //         : "cd $toplevel; git config -f .gitmodules submodule.$sm_path.shallow false"
            //     )
            // }); // TODO: is this sufficient to unshallow? TODO: add test..
            // TODO: fix; this only applies to checked out submodules and thus doesn't work..

            if (!repoInfo.shallow) {
                // in case the repo's submodules were previously initialized as
                // shallow:
                //
                // if the submodules have not yet been initialized this is a
                // no-op.
                runProgram("git", true, {
                    "--git-dir", pathToGitFolder,
                    "--work-tree", tmpDir,
                    "-C", tmpDir, // necessary for `git-submodule` to work...
                    "submodule", "foreach", "--recursive",
                    ("git fetch --unshallow --jobs=" + getNumJobs())
                }); // TODO: add test..

                // TODO: not sure this actually works... (need to specify remote?); not sure it even matters.
            }

            // And then checkout the submodules:
            {
                Activity act(*logger, lvlTalkative, actUnknown, fmt("fetching submodules of '%s'", repoInfo.url));

                Strings args = {
                    "--git-dir", pathToGitFolder,
                    "--work-tree", tmpDir,
                    "-C", tmpDir, // necessary for `git-submodule` to work...
                    "submodule", "update",
                    "--init", "--recursive", "--quiet", "--recommend-shallow",
                    "--jobs", getNumJobs(),
                };

                if (repoInfo.shallow) {
                    args.emplace_back("--depth=1");
                }

                // try checking out submodules without fetching first since
                // otherwise git seems to unnecessarily do a fetch when the
                // submodule commit is present but not reachable
                try {
                    Strings argsNoFetch = Strings(args.begin(), args.end());
                    argsNoFetch.emplace_back("--no-fetch");

                    runProgram2({
                        .program = "git",
                        .args = argsNoFetch,
                    });
                } catch (ExecError &) {
                    // TODO: cache these fetches for local sources
                    runProgram("git", true, args);
                }
            }

            filter = isNotDotGitDirectory;
        } else {
            // FIXME: should pipe this, or find some better way to extract a
            // revision.
            auto source = sinkToSource([&](Sink & sink) {
                runProgram2({
                    .program = "git",
                    .args = { "-C", repoDir, "--git-dir", repoInfo.gitDir, "archive", rev.gitRev() },
                    .standardOut = &sink
                });
            });

            unpackTarfile(*source, tmpDir);
        }

        auto storePath = store->addToStore(name, tmpDir, FileIngestionMethod::Recursive, htSHA256, filter);

        if (!origRev)
            getCache()->add(
                store,
                unlockedAttrs,
                infoAttrs,
                storePath,
                false);

        getCache()->add(
            store,
            getLockedAttrs(),
            infoAttrs,
            storePath,
            true);

        return makeResult(infoAttrs, std::move(storePath));
    }

    std::pair<ref<InputAccessor>, Input> getAccessorFromCheckout(
        RepoInfo & repoInfo,
        Input && input) const
    {
        if (!repoInfo.isDirty) {
            auto ref = getDefaultRef(repoInfo);
            input.attrs.insert_or_assign("ref", ref);

            auto rev = updateRev(input, repoInfo, ref);

            input.attrs.insert_or_assign(
                "revCount",
                getRevCount(repoInfo, repoInfo.url, rev));

            input.attrs.insert_or_assign(
                "lastModified",
                getLastModified(repoInfo, repoInfo.url, rev));
        } else {
            repoInfo.warnDirty();

            // FIXME: maybe we should use the timestamp of the last
            // modified dirty file?
            input.attrs.insert_or_assign(
                "lastModified",
                getLastModified(repoInfo, repoInfo.url, "HEAD"));
        }

        return {
            makeFSInputAccessor(CanonPath(repoInfo.url), listFiles(repoInfo), makeNotAllowedError(repoInfo.url)),
            std::move(input)
        };
    }

    std::pair<ref<InputAccessor>, Input> getAccessor(ref<Store> store, const Input & _input) const override
    {
        Input input(_input);

        auto repoInfo = getRepoInfo(input);

        if (input.getRef() || input.getRev() || !repoInfo.isLocal)
            return getAccessorFromCommit(store, repoInfo, std::move(input));
        else
            return getAccessorFromCheckout(repoInfo, std::move(input));

    }

    bool isLocked(const Input & input) const override
    {
        return (bool) input.getRev();
    }

    std::optional<std::string> getFingerprint(ref<Store> store, const Input & input) const override
    {
        if (auto rev = input.getRev()) {
            return fmt("%s;%s", rev->gitRev(), getSubmodulesAttr(input) ? "1" : "0");
        } else
            return std::nullopt;
    }

};

static auto rGitInputScheme = OnStartup([] { registerInputScheme(std::make_unique<GitInputScheme>()); });

}
