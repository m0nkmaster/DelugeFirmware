# Fuzzy Filtering Feature Implementation Plan

## Overview
This plan details the steps to implement the fuzzy filtering feature for the Deluge firmware, specifically for browsing synths, kits, and samples. The goal is to add real-time, non-destructive filtering of file lists as the user types, following all safety and minimal-change protocols.

---

## 1. Analysis Summary
- **Current search is prefix-based** and does not hide non-matching items.
- **Filtering must be fuzzy** (case-insensitive, substring, whitespace/special-char tolerant).
- **Only synth, kit, and sample browsing** should be affected.
- **There are no dedicated KitBrowser or SynthBrowser classes/files.**
- **Must not break** navigation, selection, or folder logic.
- **Filtering must be non-destructive**: always able to restore the full file list.

---

## 2. Affected Code/Classes (Double-Checked)
- `src/deluge/gui/ui/browser/browser.cpp` / `browser.h` (base browser logic)
- `src/deluge/gui/ui/browser/sample_browser.cpp` / `sample_browser.h` (specialized for audio/sample browsing)
- QWERTY input handling (for filter text)
- File display/rendering logic

**Note:**
- There are no `kit_browser.cpp` or `synth_browser.cpp` files. Kit and synth browsing are handled via the base `Browser` and possibly through configuration or mode within the same class.
- The fuzzy filter must be scoped to only activate when the browser is in synth, kit, or sample mode. This may require checking the current directory, file type, or output type.

---

## 3. Step-by-Step Implementation Plan

### Step 1: Preparation & Safety
- [ ] Identify all locations where prefix-based search is invoked in the relevant browsers (base and sample browser).
- [ ] Ensure changes are limited to synth, kit, and sample browsing only (e.g., by checking directory or output type).
- [ ] Add debug logging to all entry points for search/filter logic for traceability.

### Step 2: Filter State Management
- [ ] Add a new member (e.g., `String filterText`) to the relevant browser class(es) to store the current filter string.
- [ ] Ensure this filter text is updated on QWERTY input, backspace, and cleared on folder navigation/exit as per requirements.
- [ ] Add logic to persist filter text within the same directory, and clear it when changing directories or exiting.

### Step 3: Fuzzy Filter Algorithm
- [ ] Implement a fuzzy match function (case-insensitive, substring, whitespace/special-char tolerant).
- [ ] On each filter text change, generate a filtered index or view of `fileItems` (do not destructively modify the main array).
- [ ] Ensure performance: filtering must complete within 50ms.

### Step 4: Display Logic
- [ ] Update the file display/rendering logic to only show items matching the filter (using the filtered index/view).
- [ ] Ensure original file order is preserved.
- [ ] Handle empty filter (show all), no matches (show empty state), and backspace/clear (restore all).

### Step 5: Navigation & Selection
- [ ] Update navigation logic (scrolling, selection) to operate on the filtered view.
- [ ] Ensure selection index is always valid (reset if current selection is filtered out).
- [ ] Test edge cases: backspace, clear, folder navigation, empty/full lists.

### Step 6: Visual Feedback
- [ ] Update QWERTY pad lighting to indicate which letters are in the filter text (green for active, standard otherwise).
- [ ] Add a clear visual indicator when the filter is active.

### Step 7: Safety & Testing
- [ ] Add debug logs for filter application and state changes.
- [ ] Build and verify after every change (per protocol).
- [ ] Test all edge cases and ensure no regressions in navigation, selection, or file operations.

---

## 4. Rollback & Safety
- All changes must be easily revertible.
- No core file/folder logic should be destructively modified.
- If any build fails, revert immediately and investigate.

---

## 5. Next Steps
- [ ] Review this plan for completeness and safety.
- [ ] Proceed to code location identification and minimal implementation.

---

*Prepared according to Deluge Firmware Safety Protocols. All file/class references have been double-checked for accuracy.*
