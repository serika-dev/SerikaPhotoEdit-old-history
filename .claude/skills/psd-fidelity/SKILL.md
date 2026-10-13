---
name: psd-fidelity
description: Diagnose and fix PSD/PSB import or export problems (wrong layer order, broken groups, missing effects, noisy import reports). Use when a Photoshop file opens or saves wrong in Serika.
---

# PSD fidelity

Code: `src/io/psd/PsdFormat.cpp` (`readLayerInfo`, `parseTags`, the group builder in the reader,
`emitChildren` in the writer). Spec: the public Adobe File Formats Specification.

## Compare against Photoshop's own answer

```bash
python -I scripts/psd_inspect.py file.psd reference.png 4   # raw records in file order + Photoshop composite
build/serika_psdcheck file.psd serika.png                   # Serika's tree, report and composite
```

Read both PNGs. The reference is the flattened image Photoshop stored in the file, so any
difference is Serika's. `psd_inspect.py` shares no code with Serika on purpose.

## Facts that bit us

- Layer records are stored **bottom-to-top**. A group is: end marker (`lsct` section 3, named
  `</Layer group>`), its children, then the folder record (section 1 open / 2 closed). Serika
  builds before 2026-10 wrote the reverse; the reader detects old files by the first group record
  and still opens them.
- Serika's own PSDs carry private `sPEd`/`sPEi`/`sPEb` blocks; strip them (rename the keys) in a
  test to exercise the pure Photoshop path, as `psdRecordsAreBottomToTopLikePhotoshop` does.
- Unknown blocks are preserved in `_psdTaggedBlocks` for round-tripping. Only blocks that change
  what the user sees belong in the import report, summarised once per layer.

## Fix checklist

1. Reproduce with the two tools above; keep the user's file out of the repo.
2. Write the failing test in `tests/io_tests.cpp` with a synthetic file (see the builders at the
   top of that file) before changing the reader.
3. Check the writer too: Photoshop must open what Serika saves.
4. Changelog `Fixed` entry describing what users saw.
