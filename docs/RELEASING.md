# Versioning, branches and releases

Serika PhotoEdit uses [Semantic Versioning](https://semver.org/) and a
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) history. `scripts/release.py` does the
mechanical parts; CI (`.github/workflows/release-metadata.yml`) verifies them on every push and pull
request.

## Version numbers

`MAJOR.MINOR.PATCH`, defined once in `CMakeLists.txt` (`project(SerikaPhotoEdit VERSION …)`). The
build generates `serika/Version.h` from it, so the About box, `--version` and the MCP `serverInfo`
always agree. The Windows resource, macOS `Info.plist`, Nix flake and the Unix release workflow keep
their own copies; never edit those by hand — `release.py bump` rewrites all of them and
`release.py check` fails CI if any copy drifts.

Before 1.0, a **minor** release (0.**2**.0) carries features and may change behaviour or file
compatibility; a **patch** release (0.1.**3**) only fixes bugs and never changes the SPE format.
From 1.0 on, the usual SemVer contract applies: breaking changes need a major release.

## Branches

```
main ──●──●──●──●──●──●──●──●──●──▶   development of the next minor release
            \ v0.1.0          \ v0.2.0
             release/0.1        release/0.2
              ●──●  v0.1.1        ●  v0.2.1          fixes only, tagged patch releases
```

- **`main`** is the development line for the next minor release. It must always build and pass
  every test. Work happens on short-lived topic branches (`fix/psd-group-order`,
  `feature/live-mcp`) merged through pull requests.
- **`release/X.Y`** is created at the `vX.Y.0` tag and receives only fixes for that minor line.
  Land a fix on `main` first, then `git cherry-pick -x` it onto every supported `release/X.Y`
  (cherry-pick in the other direction only for release-specific fixes).
- **Tags** are annotated `vX.Y.Z` on the commit named `Release X.Y.Z`. Tags are never moved or
  reused; a broken release gets a new patch version.
- The newest two `release/X.Y` branches are supported; older ones are frozen.

## Changelog

Every user-visible change adds a line under `## [Unreleased]` in `CHANGELOG.md` in the **same pull
request**, under one of `Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`. Write for
users ("PSD files from Photoshop opened upside down …"), not for reviewers ("refactor parseTags").
Pull requests that touch `src/`, `resources/`, `packaging/`, `third_party/` or `CMakeLists.txt`
without a changelog entry fail CI unless labelled `skip-changelog` (refactors, tests, tooling).

## Cutting a release

1. Make sure `main` (or `release/X.Y` for a patch) is green and `[Unreleased]` reads well.
2. Preview: `python scripts/release.py cut X.Y.Z --dry-run`.
3. Release: `python scripts/release.py cut X.Y.Z`. This
   - bumps every version copy and turns `[Unreleased]` into `## [X.Y.Z] - YYYY-MM-DD` with compare
     links,
   - commits `Release X.Y.Z` and creates the annotated tag `vX.Y.Z` (its message is the changelog
     section),
   - for `X.Y.0`, creates `release/X.Y` at the tag.
   It never pushes. Review the commit, then run the `git push origin …` command it prints.
4. The tag push triggers the release workflows; attach packages and paste
   `python scripts/release.py notes X.Y.Z` as the release body. Record package validation in
   [RELEASE-VALIDATION.md](RELEASE-VALIDATION.md).
5. For a patch: cherry-pick fixes onto `release/X.Y`, add their changelog lines there, and cut
   `X.Y.(Z+1)` from that branch. Then merge the release's changelog section back into `main`.

## Commits

Imperative subject of at most 72 characters ("Read PSD layer records bottom-to-top"), a blank line,
then a body explaining **why** and what the user sees. Reference issues as
`serika-dev/SerikaPhotoEdit#123`. Agents add their `Co-Authored-By:` trailer. Commits use the
repository-local Git identity (`git config --local user.name` / `user.email`); check it before the
first commit on a new machine.
