# hud-tryer

Minimal LeviLamina plugin for testing Bedrock HUD layers and client-only map previews.

## Commands

- `/hudtry actionbar`
- `/hudtry palette [textobject]`
- `/hudtry matrix [textobject]`
- `/hudtry maptest checker`
- `/hudtry maptest gradient`
- `/hudtry mapclear`
- `/hudtry subtitle`
- `/hudtry title`
- `/hudtry all`
- `/hudtry clear`
- `/hudtry reset`

All commands are player-only and send packets only to the player who runs the command.

`palette` displays the 28 Bedrock formatting colors. `matrix` sends an explicit `24x12` block-character matrix to compare regular Actionbar with `ActionbarTextObject` behavior, including newlines, wrapping, spacing, background, and lifetime.

`maptest` temporarily replaces only the selected slot in that client's view with a virtual filled map and sends a `128x128` ARGB test image. It does not modify the server-side inventory. The client view is restored after 10 seconds, when the selected slot changes, with `/hudtry mapclear`, or when the plugin is disabled. Death and disconnect discard the preview session because the client will receive a fresh inventory state.

The map preview is experimental. Test it on a dedicated development server before using it with real players. Moving slots or interacting during the preview may make the client hide the virtual map early; the server inventory remains authoritative.

The HUD probes cover:

- position
- wrapping
- background
- lifetime
- overlap behavior

## Build

This repository already includes GitHub Actions workflows for build and release.

The dependency is pinned to LeviLamina `26.10.13` to match the target development server ABI.

`tooth.json` is the single version source for artifact names and the packaged manifest. Preferred verification uses the `Build & Release` workflow with `create_release=false`. Local build, when an equivalent toolchain is available:

1. `xmake repo -u`
2. `xmake f -a x64 -m release -p windows --target_type=server -y`
3. `xmake`
