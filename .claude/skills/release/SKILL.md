---
name: release
description: Versioning, CHANGELOG entries, release branches and cutting Serika releases. Use when adding a changelog entry, bumping or reading the version, backporting a fix to release/X.Y, or preparing a release.
---

# Versions, changelog and release branches

Policy: [docs/RELEASING.md](../../../docs/RELEASING.md). Tooling: `scripts/release.py`.

## Every change

- User-visible? Add one line under `## [Unreleased]` in `CHANGELOG.md`, in the right section
  (`Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`). Write the user's experience:
  "PSD files from Photoshop opened upside down …", not "fix parseTags".
- Never edit version numbers by hand. `python scripts/release.py current` prints the version (from
  `CMakeLists.txt`); `python scripts/release.py check` verifies every copy and the changelog shape.
- Code reads the version from the generated `<serika/Version.h>` (`SERIKA_VERSION`).

## Branch model

- `main`: next minor release. Topic branches merge into it.
- `release/X.Y`: created at `vX.Y.0`, fixes only. Land the fix on `main`, then
  `git cherry-pick -x <sha>` onto `release/X.Y` and add the changelog line there too.
- Tags: annotated `vX.Y.Z`, never moved.

## Cutting a release (maintainer asks first)

```bash
python scripts/release.py cut 0.2.0 --dry-run   # shows the plan, checks branch and clean tree
python scripts/release.py cut 0.2.0             # bump, date changelog, commit, tag, release/0.2
python scripts/release.py notes 0.2.0           # body for the GitHub release
```

The script never pushes; hand the printed `git push origin …` line to the maintainer. A patch
release (`0.2.1`) must be cut from `release/0.2`. To rehearse safely, copy `CMakeLists.txt`,
`CHANGELOG.md`, `scripts/release.py` and the files listed in `version_files()` into a scratch
folder with the same layout and run `prepare` there.
