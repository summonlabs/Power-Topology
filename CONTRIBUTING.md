# Contributing to Power Topology

Power Topology is part of Data Center Control Plane (DCCP), Tranche 3
(Electrical Infrastructure Control), and is maintained by Summon Software Labs.

## Licensing of contributions

This project is licensed under the Apache License, Version 2.0 (see `LICENSE`).

By submitting a contribution you agree that it is licensed under the terms of
that license, as described in section 5 of the license text. There is **no
Contributor License Agreement** to sign, and no copyright assignment is
required: you keep the copyright in your contribution and grant the project the
license described in `LICENSE`.

Please do not add co-author trailers or attribution lines that name tools,
assistants or intermediate processes; commit authorship is the responsibility of
the human contributor.

## What belongs in this repository

Power Topology owns the generation-bound structural graph of facility electrical
connectivity: utility feeds, switchgear, transformers, UPS systems, buses,
PDUs/RPPs, branch circuits, transfer/tie connections, load attachment points,
redundancy groups and failure-domain references, containment, provenance,
immutable topology generations, deterministic canonical serialization, durable
generations with an authoritative head manifest, verification, diffing and
conservative recovery.

It deliberately does **not** own, and must not grow:

* current electrical operating mode, switching state or breaker position;
* switching permission, feed-serving authority or authorization decisions;
* PDU, UPS or generator lifecycle control (PDU Control, UPS Control,
  Generator Control);
* load shedding or shedding policy (Load Shedding);
* power-capacity arithmetic or headroom accounting (Power Capacity, Rack
  Capacity, Facility Capacity);
* energy accounting (Energy Ledger);
* physical actuation of any kind;
* detailed asset inventory (Asset Registry), rack membership (Rack Registry),
  stable coordinates (Physical Location Registry), failure-domain definitions
  (Failure-Domain Registry) or general dependency policy (Facility Dependency
  Registry).

External facility, rack, asset, location, consumer, failure-domain and registry
identities may be referenced only through the opaque `ExternalRef` type. Those
bytes are preserved exactly and are never resolved, normalized or interpreted
here.

Dynamic telemetry and mutable device operating state must not be added to the
canonical topology. A generation is a structural claim about what exists and how
it is connected, not an observation of what is energized.

## Building and testing

The project is a portable C++20 CMake project with no third-party dependencies:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requirements: CMake 3.20 or newer and a C++20 compiler (MSVC 19.30+, GCC 11+ or
Clang 14+).

Useful options:

* `-DPOWER_TOPOLOGY_WARNINGS_AS_ERRORS=ON` (default) - first-party warnings are
  errors; do not add warning suppressions, fix the cause;
* `-DPOWER_TOPOLOGY_ENABLE_ASAN=ON` - AddressSanitizer build;
* `-DPOWER_TOPOLOGY_BUILD_TESTS/TOOLS/EXAMPLES/BENCHMARKS=OFF` - skip optional
  components when embedding the library.

The test suite is a single executable that runs every proof obligation to
completion; it has no timing logic and no timeouts. Run it plainly:

```sh
./build/power_topology_tests
./build/power_topology_tests --filter=store --seed=12345
```

Property and state-machine cases derive their seed from the run seed and the case
name; a failure prints the reproduction seed, and `--seed=<n>` reproduces it
exactly. Never weaken a case to make a build pass: a failing case is a defect to
diagnose and repair.

## Code quality expectations

* C++20, standard library only. A new third-party dependency must be justified in
  the pull request, packaged, and validated; the default answer is no.
* The public headers are a long-lived contract: add, do not renumber. Error codes,
  tokens and file-format fields are stable once released.
* Every state-dependent mutation states the authority and base generation it was
  planned against and must refuse stale authority rather than merge it.
* Deterministic outcomes only: no wall-clock values, no randomness, no
  process-specific values and no unordered iteration order in canonical content.
* Bound every externally influenced size before allocation and use checked
  arithmetic for authoritative calculations.
* Keep the library single-threaded internally; do not add concurrency without a
  documented lock order and a proof of safety.
* Keep the safety boundary explicit in the API and in the documentation: this
  project answers structural questions and never claims energization,
  authorization, capacity or actuation.

## Before opening a pull request

1. `cmake --build build` is warning-free with warnings-as-errors enabled.
2. The full test suite passes, including the new obligations your change adds.
3. Examples and the `ptopctl` CLI still build and run.
4. `cmake --install` plus an out-of-tree `find_package(power_topology)` consumer
   still works when you changed anything public.
5. Public documentation (`README.md`) describes only implemented and verified
   behaviour.
