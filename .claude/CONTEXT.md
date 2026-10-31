# CrossPoint Reader — Durable Context

Keep this file focused on repo-specific gotchas that are worth reusing in future sessions.

## FreeInk SDK

Refer to https://freeink.org/llms.txt for guidance.

## Simulator

- Simulator patches belong in the adjacent `crossink-simulator` repo.
- The valid local simulator env in this repo is `simulator`, and `pio run -e simulator` currently builds cleanly.
- The simulator `PNGdec` stub in `crossink-simulator/src/PNGdec.h` needs to mirror the real API shape used by app code, including `hasAlpha()` and `getTransparentColor()`, even though decode still fails intentionally.
- Known simulator limits:
  - No image rendering: `platformio.ini` ignores `hal`, `PNGdec`, and `JPEGDEC`, so image decoders are intentionally absent.
  - JPEGDEC stub always fails; `JPEGDEC fallback: open failed (err=-1)` is expected in simulator.
  - `esp_deep_sleep_start()` is a no-op in simulator.
  - `HalStorage` uses POSIX file access under `./fs_` and allows multiple readers, unlike real hardware.
  - `lib/HalClockSim` always reports no date, so RTC-dependent stats paths (daily minutes, Reading Rhythm backfill) never run in the simulator; cover them with host tests.
  - `x4-pro-simulator` has an RTC backed by the host clock (simulator lib `HalClock`), so Reading Rhythm/Book Rhythm pages render there. README screenshots come from it with hand-built `global_stats.bin`, `daily_minutes.bin`, `finished_books.bin` and per-book `stats_v5.bin`/`rhythm_days.bin` under `./fs_/.crosspoint/`, driven by `CROSSPOINT_SIM_INPUT_SCRIPT` + `CROSSPOINT_SIM_SCREENSHOTS`.

## Real Hardware / Storage

- SdFat on hardware allows only one open reader per file path at a time. If a fallback needs to reopen the same file, close the first handle before reopening.

## Rendering / Reader Pipeline

- `lib/Epub/Epub/Page.cpp`: images must render only in `GfxRenderer::BW`; grayscale passes are text anti-aliasing passes only.
- Kindle EPUBs may contain paired high-res and old-Kindle fallback images. `ChapterHtmlSlimParser` should skip `<img>` nodes with `data-AmznRemoved-M8` to avoid duplicate stacked images.
- After image/layout pipeline changes that affect cached EPUB output, clear the affected `.crosspoint/epub_<hash>/` cache if behavior looks stale.

## UI Consistency

- Use FreeInkUI SDK components and input routing for list-style screens where possible. Row rendering, touch targets,
  hit testing, and pagination should share the same FreeInkUI list configuration instead of custom touch scaling.

## Heap Baselines (X4 hardware, SD card font)

- A normal resume-into-partial reading session runs at ~85-90KB free / ~49KB maxAlloc by
  the first watermark crossing (Epub metadata + x-locations + resident glyph caches).
  Do not read mid-range heap numbers as session degradation without checking the scenario.
- SD-font section builds cost ~38-50KB at cold start; the 4-style advance-table prewarm
  (~30KB incl. 16KB contiguous scratch) dominates and is skipped below 80KB free.

## Misc Repo Gotchas

- Reading Rhythm / Books Finished data lives only in sidecars (`daily_minutes.bin`, `finished_books.bin`, code in `src/activities/reader/rhythm/`); never bump `global_stats.bin` (v3) or `stats_v5.bin` for them.
- `pio run` needs Python >= 3.10 and PlatformIO 6.1.x; PlatformIO 6.2.0 swaps tool-scons and breaks the ESP32-C3 link (`No module named 'SCons.Tool.FortranCommon'`).
- Never run two `pio run` builds of this repo at once (two sessions, two worktrees, anything). `custom_sdkconfig` in `[firmware_tuned]` rebuilds the ESP-IDF libraries into the *shared* `~/.platformio/packages/framework-arduinoespressif32-libs/` tree, so concurrent builds overwrite each other's archives mid-write. The symptom is a link failure in whatever file lost the race, e.g. `libefuse.a: error adding symbols: file format not recognized`, with a batch of `lib*.a` files freshly re-dated. Recover by running one build alone; it rewrites them.
- The repo's `.clang-format` uses the `SortIncludes: {Enabled: ...}` struct form, which needs clang-format 21 or newer. clang-format 20 (current Homebrew llvm) rejects the file outright, so the format command in AGENTS.md fails on every file until a newer binary is used.

- POSIX TZ signs are inverted from ISO 8601 in `TimeStore::applyTimezone()`: `"UTC-1"` means UTC+1.
- `LyraTheme::drawHeader()` does not call `BaseTheme::drawHeader()`, so header changes in the base theme must be duplicated in Lyra if needed.

## Indic Shaping

- Shaping tables are NOT in firmware. `gen_indic_shaping.py --script <name>` writes `lib/EpdFont/scripts/shaping/<script>.cpshape`; `build-sd-fonts.py` copies it into the family folder as `shaping.cpshape`; `SdCardFontSystem::ensureShapingLoaded()` loads it from every installed family that has one, once per registry revision, and the family becomes a script fallback. The user requirement is: copy font folders to SD, nothing else -- no build flags, no per-script firmware data. Do not move tables back into flash.
- Loaded files sit in PSRAM on S3. On C3 a file loads only if internal RAM keeps 64 KB headroom, which during normal use means it does not, so C3 draws Indic unshaped.
- The generator refuses to emit unless its tables reproduce HarfBuzz for every cluster, and `IndicShapeTest` loads the committed `.cpshape` files through the device parser, so a wrong model or a stale file fails tests rather than shipping wrong text.
- How well the factored model (base unit + attached form + akhand pair, literal exceptions for the rest) fits each script against Noto Sans: Kannada 98.1% reproduced / 25KB file, Malayalam 94.5% / 43KB, Telugu 90.8% / 56KB, Oriya 84.1% / 107KB, Devanagari 80.1% / 140KB, Tamil 79.5% / 24KB, Bengali 24.4% / 253KB.
  `base_last` (which conjunct consonant carries the matra) is per script and measured, not assumed: it took Malayalam from 90% exceptions to 5.5% and Oriya from 79% to 16%, while Devanagari and Tamil measured cheaper base-first with these fonts. Bengali barely moves either way -- its conjuncts are one-off ligatures; shrinking it needs a different model.
- Pre-base reordering for a *single*-consonant cluster already works without model changes: the base table stores HarfBuzz's full output order, so Tamil's split vowel signs come out right.
- Each script owns a private-use range (Kannada 0xE000, Telugu 0xE400, Tamil 0xE800, Devanagari 0xEA00, Bengali 0xEE00, Oriya 0xF200, Malayalam 0xF600). BMP private use (0xE000-0xF8FF) is the whole budget because the table codepoints are `uint16_t`. The ranges are published in generated `.cpfont` files, so changing one invalidates every font pack already built for that script.
- Shaped forms exist only in `.cpfont` files. A scalable `.ttf` family resolves glyphs through the cmap, which has no entry for them, so Indic text in a TTF reader font renders unshaped.
- Shaped forms are never preloaded into the SD advance table, so each distinct one costs an uncached `SdCardFont::readAdvance()` the first time it is measured (Telugu has 539 forms, Kannada 194). Widths stay correct -- `resolveSdCardAdvanceFP()` in `GfxRenderer.cpp` falls through getAdvance -> readAdvance -> built-in glyph -- it is only slower, and shows up as a slow first page turn in an Indic book on SPI SD.
  `ensureSdCardFontReady()` warms the table for RTL via `appendShapedRtlTokens()`, but an Indic equivalent only helps when the Indic font IS the selected reader font: layout calls it with the primary font id, while `resolveTextFontId()` routes shaped runs to the *fallback* font, whose table nothing warms. Fixing the advertised case (keep your Latin font, Indic falls back) means teaching the preload path about the fallback id, in `ParsedText.cpp` and `ChapterHtmlSlimParser.cpp`.
