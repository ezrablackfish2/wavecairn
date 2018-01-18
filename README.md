# WaveCairn

WaveCairn is a C++20 library and command-line tool for inspecting, validating, repairing, comparing, and planning the rendering of digital-audio-workstation session data without launching a DAW. It is intended for post-production facilities, archival teams, migration engineers, and build systems that need a deterministic view of tracks, clips, media references, tempo maps, automation, plug-in routing, render jobs, history, and package metadata.

WaveCairn uses two compact interchange formats:

- **WCTP command tapes** record ordered session operations. They are useful for migration pipelines, audit logs, reproducible edits, and regression fixtures.
- **WCPK packages** bundle command tapes with metadata and auxiliary sections in a self-describing container.

The library can ingest either format into the same in-memory session model. Applications may then validate references, identify missing media, inspect routing cycles, generate manifests, estimate render cost, map media paths, compare two sessions, or create a non-destructive merge plan.

## Practical uses

- Audit a delivered session before it enters a render farm.
- Detect clips that point to unavailable media or tracks.
- Produce a portable media manifest for archival storage.
- Normalize paths while moving a project between operating systems.
- Generate a routing graph for technical review.
- Estimate output size and processing work before rendering stems.
- Compare revisions and create an explicit merge decision report.
- Export session inventories as text, Markdown, JSON, or CSV.

## Build

WaveCairn has no runtime dependencies outside the C++ standard library.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The executable is written to `build/wavecairn` on single-configuration generators.

## Command-line examples

Inspect a command tape or package:

```sh
wavecairn inspect session.wctp
wavecairn inspect delivery.wcpk --format markdown
```

Validate references and structural rules:

```sh
wavecairn validate delivery.wcpk
```

Create an archive manifest:

```sh
wavecairn manifest delivery.wcpk > manifest.txt
```

Export dependency and occupancy views:

```sh
wavecairn graph delivery.wcpk > routing.dot
wavecairn occupancy delivery.wcpk --bucket 96000 > occupancy.csv
```

Canonicalize the sections of a package:

```sh
wavecairn canonicalize-package input.wcpk output.wcpk
```

The `examples/make_demo.cpp` program creates a valid command tape that can be used with these commands.

## Library example

```cpp
#include <wavecairn/report.h>
#include <wavecairn/session.h>

wavecairn::Session session;
wavecairn::Diagnostics diagnostics;
wavecairn::ProcessingOptions options;
options.canonicalize_at_end = true;

auto result = wavecairn::process_input(session, bytes, diagnostics, options);
if (!result) {
    throw std::runtime_error(result.status().message());
}

wavecairn::ReportOptions report_options;
report_options.include_manifest = true;
auto report = wavecairn::build_report(session, &diagnostics, report_options);
std::cout << wavecairn::render_report(report, wavecairn::ReportFormat::markdown);
```

## Data model

A `Session` owns six cooperating subsystems:

1. `MediaPool` stores source assets, aliases, format data, and availability.
2. `Timeline` stores tracks, clips, tempo segments, automation, markers, and takes.
3. `RoutingGraph` stores plug-ins, edges, sends, listeners, and query results.
4. `RenderingSystem` stores jobs, worker slices, sample blocks, waveform pyramids, and export paths.
5. `HistoryStore` stores snapshots, journal records, undo frames, and project records.
6. `PackageWorkspace` stores WCPK sections and package metadata.

Commands are applied in transaction order. Optional barriers let an importer validate cross-subsystem state at meaningful lifecycle boundaries. Canonicalization sorts stable collections and rebuilds indexes so repeated imports produce comparable output.

## Formats

The format details are documented in [`docs/formats.md`](docs/formats.md). Both formats use little-endian integers and checksums that are verified before commands reach the session model. The parser applies configurable byte, command-count, section-count, and payload limits.

## Validation and repair

`SessionValidator` checks metadata, paths, media references, clip ranges, routing references and cycles, render ranges, history digests, and package sections. Diagnostics carry severity, transaction number, component, code, and a human-readable message.

`SessionRepairer` performs conservative normalization such as label cleanup, path normalization, duplicate-marker removal, dangling-edge removal, and removal of empty package sections. It does not invent missing media or silently rewrite creative timing decisions.

## Merge workflow

`plan_merge` compares a base session with an incoming session and returns a command tape plus a decision list. The policy can keep the base, prefer the incoming revision, or prefer the item with the newer revision. Conflicting operations that cannot be represented safely as an in-place command are reported rather than hidden.

## Source layout

```text
include/wavecairn/   public headers
src/                 library implementation
cli/                 command-line application
examples/            small integration example
fuzz/                structured input harnesses and seed corpora
tests/               unit and integration tests
.clusterfuzzlite/    reproducible sanitizer build
```

## Development checks

```sh
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DWAVECAIRN_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

The ClusterFuzzLite build compiles every harness into `$OUT` and does not access the network. Each harness drives the same public import API with a different processing profile so format parsing and the relevant domain subsystem are exercised together.

## License

WaveCairn is licensed under the Apache License 2.0. See [`LICENSE`](LICENSE).

## Repository Reconstruction Notice

This repository was reconstructed from recovered local project files and available timestamp metadata after the original repository was lost. Commit dates reflect the recovered timestamp data where available. The history is intended to preserve the original project timeline as accurately as possible, not to fabricate or inflate contribution activity.

