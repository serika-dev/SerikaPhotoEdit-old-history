#!/usr/bin/env python3
"""Versioning, changelog and release-branch tooling for Serika PhotoEdit.

The version lives in CMakeLists.txt `project(... VERSION X.Y.Z ...)`; every other copy (Windows
resource, macOS Info.plist, Nix flake, release workflow default) is kept in step by this script,
and CI runs `check` so they can never drift. See docs/RELEASING.md for the branch model.

  python scripts/release.py current                 print the version
  python scripts/release.py check                   verify version copies and CHANGELOG structure
  python scripts/release.py bump X.Y.Z              rewrite every version copy
  python scripts/release.py prepare X.Y.Z [--date YYYY-MM-DD]
                                                    bump + move [Unreleased] into a dated section
  python scripts/release.py notes [X.Y.Z]           print a version's changelog section (release body)
  python scripts/release.py cut X.Y.Z [--dry-run]   prepare, commit, tag vX.Y.Z and, for X.Y.0,
                                                    create release/X.Y; prints the push command

Pure standard library; run with any Python 3.9+.
"""
import argparse
import datetime
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CHANGELOG = ROOT / "CHANGELOG.md"
REPOSITORY = "https://github.com/serika-dev/SerikaPhotoEdit"
SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
SECTIONS = ("Added", "Changed", "Deprecated", "Removed", "Fixed", "Security")


def version_files(version):
    """(path, pattern, replacement) for every place the version is written."""
    major, minor, patch = version.split(".")
    return [
        ("CMakeLists.txt", r"(project\(SerikaPhotoEdit VERSION )\d+\.\d+\.\d+", rf"\g<1>{version}"),
        ("packaging/windows/SerikaPhotoEdit.rc", r"(FILEVERSION )\d+,\d+,\d+,0", rf"\g<1>{major},{minor},{patch},0"),
        ("packaging/windows/SerikaPhotoEdit.rc", r"(PRODUCTVERSION )\d+,\d+,\d+,0",
         rf"\g<1>{major},{minor},{patch},0"),
        ("packaging/windows/SerikaPhotoEdit.rc", r'("FileVersion", ")\d+\.\d+\.\d+', rf"\g<1>{version}"),
        ("packaging/windows/SerikaPhotoEdit.rc", r'("ProductVersion", ")\d+\.\d+\.\d+', rf"\g<1>{version}"),
        ("packaging/macos/Info.plist", r"(<key>CFBundleShortVersionString</key><string>)\d+\.\d+\.\d+",
         rf"\g<1>{version}"),
        ("packaging/macos/Info.plist", r"(<key>CFBundleVersion</key><string>)\d+\.\d+\.\d+", rf"\g<1>{version}"),
        ("packaging/nix/flake.nix", r'(version = ")\d+\.\d+\.\d+', rf"\g<1>{version}"),
        (".github/workflows/release-unix.yml", r"(default: v)\d+\.\d+\.\d+", rf"\g<1>{version}"),
    ]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def write(path, text):
    # Preserve each file's existing line endings.
    raw = (ROOT / path).read_bytes()
    newline = "\r\n" if b"\r\n" in raw else "\n"
    (ROOT / path).write_bytes(text.replace("\r\n", "\n").replace("\n", newline).encode("utf-8"))


def current_version():
    match = re.search(r"project\(SerikaPhotoEdit VERSION (\d+\.\d+\.\d+)", read("CMakeLists.txt"))
    if not match:
        sys.exit("CMakeLists.txt has no project(SerikaPhotoEdit VERSION X.Y.Z ...).")
    return match.group(1)


def require_semver(version):
    if not SEMVER.match(version):
        sys.exit(f"{version!r} is not a SemVer version like 1.4.2.")


def git(*args, capture=True):
    result = subprocess.run(["git", *args], cwd=ROOT, text=True, capture_output=capture)
    if result.returncode:
        sys.exit(f"git {' '.join(args)} failed:\n{result.stderr}")
    return result.stdout.strip() if capture else ""


def check(_args=None):
    problems = []
    version = current_version()
    for path, pattern, replacement in version_files(version):
        found = [m.group(0) for m in re.finditer(pattern, read(path))]
        if not found:
            problems.append(f"{path}: no version matching {pattern}")
        # Each copy must already read exactly what `bump` would write for the CMake version.
        for text in found:
            if re.sub(pattern, replacement, text) != text:
                problems.append(f"{path}: {text!r} does not match {version}")
    changelog = CHANGELOG.read_text(encoding="utf-8").replace("\r\n", "\n")
    if "\n## [Unreleased]\n" not in changelog:
        problems.append("CHANGELOG.md: missing '## [Unreleased]' section")
    headings = re.findall(r"^## \[([^\]]+)\](?: - (\d{4}-\d{2}-\d{2}))?$", changelog, re.M)
    for name, date in headings:
        if name != "Unreleased" and (not SEMVER.match(name) or not date):
            problems.append(f"CHANGELOG.md: heading [{name}] needs a SemVer version and ' - YYYY-MM-DD'")
        if f"\n[{name}]: " not in changelog:
            problems.append(f"CHANGELOG.md: no link reference for [{name}]")
    for heading in re.findall(r"^### (.+)$", changelog, re.M):
        if heading not in SECTIONS:
            problems.append(f"CHANGELOG.md: '### {heading}' is not one of {', '.join(SECTIONS)}")
    released = [name for name, _ in headings if name != "Unreleased"]
    if released and released[0] != version and version not in released:
        problems.append(f"CHANGELOG.md: latest release {released[0]} but CMake says {version} "
                        "(bump only through `release.py prepare`)")
    if problems:
        print("Release check failed:\n  " + "\n  ".join(problems))
        return 1
    print(f"Release check passed: version {version} is consistent and CHANGELOG.md is well formed.")
    return 0


def bump(version):
    require_semver(version)
    for path, pattern, replacement in version_files(version):
        text = read(path)
        updated, count = re.subn(pattern, replacement, text)
        if not count:
            sys.exit(f"{path}: pattern {pattern} not found")
        write(path, updated)
    print(f"Version set to {version} in {len({p for p, *_ in version_files(version)})} files.")


def unreleased_body(changelog):
    match = re.search(r"\n## \[Unreleased\]\n(.*?)(?=\n## \[|\n\[Unreleased\]: )", changelog, re.S)
    return match.group(1) if match else None


def prepare(version, date):
    require_semver(version)
    previous = current_version()
    changelog = CHANGELOG.read_text(encoding="utf-8").replace("\r\n", "\n")
    body = unreleased_body(changelog)
    if body is None or not re.search(r"^- ", body, re.M):
        sys.exit("CHANGELOG.md [Unreleased] has no entries; nothing to release.")
    released = re.findall(r"^## \[(\d+\.\d+\.\d+)\]", changelog, re.M)
    if version in released:
        sys.exit(f"{version} is already in CHANGELOG.md.")
    base = released[0] if released else previous
    changelog = changelog.replace("\n## [Unreleased]\n" + body,
                                  f"\n## [Unreleased]\n\n## [{version}] - {date}\n" + body.lstrip("\n"), 1)
    changelog = re.sub(r"^\[Unreleased\]: .*$", f"[Unreleased]: {REPOSITORY}/compare/v{version}...HEAD", changelog,
                       flags=re.M)
    changelog = changelog.replace(f"[Unreleased]: {REPOSITORY}/compare/v{version}...HEAD",
                                  f"[Unreleased]: {REPOSITORY}/compare/v{version}...HEAD\n"
                                  f"[{version}]: {REPOSITORY}/compare/v{base}...v{version}", 1)
    write("CHANGELOG.md", changelog)
    bump(version)
    print(f"CHANGELOG.md: [Unreleased] became [{version}] - {date}.")


def notes(version):
    changelog = CHANGELOG.read_text(encoding="utf-8").replace("\r\n", "\n")
    match = re.search(rf"\n## \[{re.escape(version)}\][^\n]*\n(.*?)(?=\n## \[|\n\[Unreleased\]: )", changelog, re.S)
    if not match:
        sys.exit(f"No CHANGELOG.md section for {version}.")
    return match.group(1).strip()


def cut(version, dry_run):
    require_semver(version)
    major, minor, patch = version.split(".")
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    release_branch = f"release/{major}.{minor}"
    expected = "main" if patch == "0" else release_branch
    plan = [f"prepare {version} (bump version files, date the changelog section)",
            f"commit 'Release {version}'",
            f"annotated tag v{version}"]
    if patch == "0":
        plan.append(f"create branch {release_branch} at v{version} for {major}.{minor}.x fixes")
    print(("Dry run. " if dry_run else "") + f"On {branch}; release {version} will:\n  - " + "\n  - ".join(plan))
    if branch != expected:
        sys.exit(f"Release {version} must be cut from {expected}, not {branch}.")
    if git("status", "--porcelain"):
        sys.exit("The working tree has uncommitted changes.")
    if git("tag", "--list", f"v{version}"):
        sys.exit(f"Tag v{version} already exists.")
    if dry_run:
        return
    prepare(version, datetime.date.today().isoformat())
    if check():
        sys.exit("The prepared release does not pass `release.py check`.")
    git("add", "-A", "CHANGELOG.md", *{p for p, *_ in version_files(version)})
    git("commit", "-m", f"Release {version}")
    git("tag", "-a", f"v{version}", "-m", f"Serika PhotoEdit {version}\n\n{notes(version)}")
    pushed = [branch, f"v{version}"]
    if patch == "0":
        git("branch", release_branch, f"v{version}")
        pushed.insert(1, release_branch)
    print(f"Released {version} locally. Review, then publish with:\n  git push origin {' '.join(pushed)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("current")
    commands.add_parser("check")
    commands.add_parser("bump").add_argument("version")
    prepare_parser = commands.add_parser("prepare")
    prepare_parser.add_argument("version")
    prepare_parser.add_argument("--date", default=datetime.date.today().isoformat())
    commands.add_parser("notes").add_argument("version", nargs="?")
    cut_parser = commands.add_parser("cut")
    cut_parser.add_argument("version")
    cut_parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.command == "current":
        print(current_version())
    elif args.command == "check":
        return check()
    elif args.command == "bump":
        bump(args.version)
    elif args.command == "prepare":
        prepare(args.version, args.date)
    elif args.command == "notes":
        print(notes(args.version or current_version()))
    elif args.command == "cut":
        cut(args.version, args.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
