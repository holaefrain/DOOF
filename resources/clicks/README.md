# Factory click samples

Drop `.wav` files here to fill DOOF's four click sample slots.

**This folder is empty on purpose, and an empty folder is the normal state of this repo.**
The build, the tests and the plugin all work with nothing in it — a sample slot with no file
behind it is simply silent. Nothing here is required to build or ship DOOF.

## The licensing requirement

§8 of `project-architecture.md` lists factory content licensing as a project risk, and it is the
reason no audio is committed here. **Every file in this folder must be license-clear for
commercial redistribution inside a paid plugin.** That means one of:

- recorded or synthesised by you from scratch, or
- public domain / CC0, or
- bought under a licence that explicitly permits redistribution *as part of a plugin's factory
  content* — an ordinary sample-pack licence usually does **not**, because it covers use in
  finished music, not resale as an instrument's built-in content.

If you cannot point at the specific licence that permits redistribution, the file does not belong
here. Keep the licence text or receipt alongside your own records for anything you add.

## How the build picks these up

`CMakeLists.txt` globs `resources/clicks/*.wav` at configure time:

- **Files present** — they are embedded via `juce_add_binary_data` into a `DOOFClickSamples`
  target, and `DOOF_HAS_CLICK_SAMPLES` is defined as `1`.
- **Folder empty** — no binary-data target is created and `DOOF_HAS_CLICK_SAMPLES` is `0`.

The glob uses `CONFIGURE_DEPENDS`, so adding or removing a `.wav` re-runs CMake on the next build
rather than needing a manual reconfigure.

## Slot order

Slots map to the last four entries of the `layerN.click.type` parameter (`Sample 1` … `Sample 4`),
filled in **alphabetical order by filename**. That order is part of what a saved preset means, so
renaming a file after presets exist re-points every preset that used it. Prefix filenames with a
number (`1-...wav`, `2-...wav`) to keep the mapping explicit and stable.

Files past the fourth are ignored — the parameter's choice list is permanently eight entries wide
(see `ParamIDs.h` for why it can never grow).
