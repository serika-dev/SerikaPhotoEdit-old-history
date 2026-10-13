---
name: build-and-test
description: Build Serika PhotoEdit and run its QtTest suites, screenshots and developer tools. Use before compiling, running tests, reproducing a bug, or verifying a UI change visually.
---

# Build and test Serika

## Windows with the bundled toolchain

```bash
cd <repo>
R=$PWD/.tools
export PATH="$R/Qt/6.8.3/mingw_64/bin:$R/Qt/Tools/mingw1310_64/bin:$R/deps/install/bin:$R/python/cmake/data/bin:$PATH"
[ -f build/build.ninja ] || powershell -File scripts/build.ps1          # first configure only
cmake --build build --parallel 8 2>&1 | grep -E " error|FAILED"         # quiet unless broken
(cd build && ctest -j4 2>&1 | grep -E "tests passed|\(Failed\)")
```

- Re-run `cmake -S . -B build` after adding source files or changing `CMakeLists.txt`; a
  reconfigure that changes compile flags rebuilds everything (several minutes, so run it in the
  background).
- One suite: `cd build && QT_QPA_FONTDIR=C:/Windows/Fonts ./ui_tests.exe -platform offscreen -o out.txt,txt`
  then `grep -E "^FAIL|Totals|Loc:" out.txt`. GUI-subsystem executables print nothing to a Git Bash
  console: always use `-o file,txt`.
- `ld: cannot open output file SerikaPhotoEdit.exe: Permission denied` means the app is running.
  Check its title for `*` (unsaved edits) with
  `powershell -c "Get-Process SerikaPhotoEdit | Select MainWindowTitle"`, then close it with
  `$p.CloseMainWindow()`. A window that ignores close usually has a modal dialog up.

## macOS / Linux

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/<kit> -DSERIKA_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel && ctest --test-dir build --output-on-failure
```

## Look at the result

- Window capture: `build/SerikaPhotoEdit.exe --demo --screenshot out.png` (uses the user's saved
  settings and workspace layout). Read the PNG to inspect it.
- Every tool icon: `build/serika_iconsheet sheet.png` (`primary` for one per toolbar slot).
- PSD import: `build/serika_psdcheck file.psd composite.png` and
  `python -I scripts/psd_inspect.py file.psd reference.png 4` (Photoshop's embedded composite).
- Panel geometry or other live state: write a tiny harness under the scratchpad that links the
  static libraries (copy the `LINK_LIBRARIES` line for `ui_tests.exe` from `build/build.ninja`).

## Before you call it done

1. `cmake --build` clean with `-Werror`; all 17 suites pass.
2. Format touched files: `.tools/format/clang_format/data/bin/clang-format.exe -i <files>`.
3. New behaviour has a test; user-visible behaviour has a `CHANGELOG.md` entry.
4. UI changes: take a screenshot and look at it.
