# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.0-alpha.2] - 2026-08-10

### Added

- Added `/hudtry palette [textobject]` for all 28 Bedrock formatting colors
- Added `/hudtry matrix [textobject]` for an explicit `24x12` Actionbar probe
- Added client-only `/hudtry maptest checker`, `/hudtry maptest gradient`, and `/hudtry mapclear` experiments

### Changed

- Pinned LeviLamina to `26.10.13` for the target development server ABI
- Pinned LeviBuildScript to `0.6.1`
- Pinned the CI xmake toolchain to the previously verified `3.0.9`
- Sourced the packaged mod version from `tooth.json`
- Removed the workflow version override so artifact and manifest versions cannot diverge
- Removed the extra parameterless `Times` packet from title timing tests

## [0.1.0] - 2026-06-06

### Added

- Initial HUD test plugin commands for `actionbar`, `subtitle`, `title`, `all`, `clear`, and `reset`
