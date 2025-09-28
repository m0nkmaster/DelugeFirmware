# Repository Guidelines

## Project Structure & Module Organization
Firmware sources live in `src/deluge`, with reusable vendors and utilities under `src/lib` and `lib`. Tooling and helper scripts are grouped in `scripts/` with task entry points consumed through `./dbt`. Hardware assets and presets sit in `assets/`, while documentation and website content reside in `docs/` and `website/`. Tests are arranged by discipline inside `tests/` (notably `unit`, `spec`, `integration`, `qemu`, and `32bit_unit_tests` for memory-constrained mocks).

## Build, Test, and Development Commands
- `./dbt build debug` – generate a debug firmware image via CMake + Ninja.
- `./dbt build release` – produce the optimized image that CI publishes.
- `./dbt test` – configure (if needed), build, and run CTest across all suites.
- `./dbt format -c` – check formatting using the toolchain’s pinned clang-format.
- `./dbt clean` – remove build outputs (`build/`, `build/tests/`).

## Coding Style & Naming Conventions
C and C++ sources target C23/C++23 with LLVM-style clang-format settings (4-space indentation, tabs permitted for leading indentation, 120-column limit). Prefer modern C++ patterns over legacy single-return C constructs. Run `./dbt format` before commits, and keep `.clang-tidy` clean when adding new code. File names follow lowercase with underscores; classes and structs use PascalCase, while functions and variables use lowerCamelCase aligned with existing modules.

## Testing Guidelines
Unit tests rely on CppUTest in `tests/unit`, while behavioral specs use CppSpec under `tests/spec`. Hardware-emulation and memory-footprint scenarios live in `tests/qemu` and `tests/32bit_unit_tests`. Add new cases beside the code they exercise (`*_spec.cpp` or `*_tests.cpp`) and prefer descriptive `TEST_GROUP`/`TEST` names matching the module. Execute `./dbt test` locally; use `./dbt test --no-run` when you only need to ensure the suite builds.

## Commit & Pull Request Guidelines
Commits should stay focused, use one-line Conventional Commit subjects (`type(scope): summary`), and include the tracking issue or PR reference when available (e.g. `fix(clock): smooth MIDI jitter (#1234)`). Draft pull requests are expected once work starts so reviewers can coordinate. Upgrade branches with the latest `develop` before requesting review, document feature flags or configuration impacts, and note which hardware (OLED vs 7-seg) you exercised. Ready PRs must pass CI, include reproduction or validation steps, and link any user-facing documentation updates.
