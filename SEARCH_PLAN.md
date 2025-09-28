# Pad Search Enhancement Plan

## Current Browser Behaviour
- Browsers (`src/deluge/gui/ui/browser/`) inherit from `QwertyUI`, storing the active prefix in `enteredText` and cursor in `enteredTextEditPos`.
- Each pad press maps to a character, appended through `QwertyUI::padAction`, which then calls `Browser::predictExtendedText()`.
- `predictExtendedText()` adds a temporary `~` suffix to the prefix and performs a binary search (`CStringArray::search`) over the sorted `FileItem` list. It reloads chunks from storage if the match falls outside the cached window.
- When a match is found, the browser jumps to the nearest item whose name begins with the typed prefix and updates scroll state; backspace shortens the prefix and re-runs the same prefix search.
- 7SEG/OLED displays simply show the current prefix; typing stops once the prefix ceases to match any entry, reverting to the last valid selection.

## Requirements
- Allow incremental multi-letter substring search via pad typing across all browsers that currently expose prefix jumps (songs, synths, kits, samples, clips).
- Reduce the visible list to only matching items while the query is non-empty; encoder scrolling operates within that filtered set.
- Match case-insensitively against the full name; items starting with the query should rank ahead of other substring matches.
- Pad delete/backspace must remove the last character and immediately refresh results; empty query restores the full list for the active folder.
- Preserve encoder scrolling while a query is active and reset query state on exit, inactivity timeout, or after changing directories/folders.
- Support both OLED and 7SEG render paths; ensure feedback for the active query fits within each display’s constraints.

## Edge Cases
- Names with spaces, punctuation, numerals, or mixed case must match normally.
- Handle rapid key bursts without starving the audio thread—avoid large allocations per keystroke.
- When no matches remain, render `NO RESULTS` on OLED and `- - - -` on 7SEG, while keeping the UI responsive for backspace.
- Timeout or navigation away from the browser—and any folder change—must clear the query buffer.
- Confirm behaviour when folders and files share names, respecting existing culling logic.

## Architectural Approach
- Introduce a lightweight search controller/helper (likely in `browser.cpp`) that maintains the active query, normalized name buffers, and the filtered index list presented to the UI layer.
- Extend `FileItem` preparation to cache uppercase (or otherwise normalized) display names to minimise repeated transforms.
- Implement ranking: prefix matches first (stable order), followed by other substring matches; keep overall deterministic order.
- Replace the prefix-only binary search in `predictExtendedText()` with logic that rebuilds the filtered list and selection based on the active query; existing predictive naming flows must be preserved.
- Ensure deletion/backspace, timeout, folder-change, and other navigation paths call the same routine so the filtered view stays consistent.
- Surface the active query via the existing `enteredText` UI plumbing, so OLED/7SEG rendering continues to work without major refactoring, and inject the `NO RESULTS`/`- - - -` placeholders as appropriate when the filtered set is empty.

## Files to Inspect / Modify
- `src/deluge/gui/ui/browser/browser.cpp` & `.h` (search logic, state management).
- Specific browser subclasses (`sample_browser.cpp`, `slot_browser.cpp`, etc.) for any overridden behaviour.
- `src/deluge/gui/ui/qwerty_ui.cpp` & `.h` (pad input / backspace / timeout handling).
- Display resources (`g_english.cpp`, `g_seven_segment.cpp`) if new prompts are required.
- Tests under `tests/spec` or `tests/unit` to cover string matching and ranking (add new coverage if none exists).

## Gotchas & Risks
- Filtering large directories each keystroke could be expensive; consider incremental filtering or caching between keystrokes.
- Need to avoid breaking existing predictive naming paths (e.g., auto-suffix when saving), which also rely on `predictExtendedText()`.
- Ensure device memory constraints are respected—no unbounded allocations for filtered lists or uppercase copies.
- Confirm that favourites/QWERTY overlays keep functioning when the browser query changes state.
- Backwards compatibility: single-letter behaviour should feel the same, just extended when additional characters are typed.

## Open Questions / Follow-Ups
- None outstanding at this stage; proceed with implementation details once data-model constraints and performance characteristics are validated on device.
