# Fuzzy Filtering Feature Requirements

## Feature Overview
Add real-time filtering to the Deluge's file browser that narrows down the visible file list as the user types, showing only items that match the search text anywhere in their filenames.

## Current Behavior
- File browser displays all files/folders in current directory
- Basic search capability
- QWERTY pad input only used for exact filename entry
- All items remain visible regardless of typing

## Desired Behavior
- Start with full file list visible
- As user types, list filters down to matching items only
- Non-matching items are hidden in real-time
- Backspacing reveals previously hidden items
- Clearing search shows all items again

## Technical Specifications

### Filtering Algorithm
1. **Case-insensitive comparison** - typing "bass" keeps "Bass.xml", "BASS.xml", "bAsS.xml" visible
2. **Substring matching** - typing "arp" keeps "Arpeggiator.xml", "Sharp Lead.xml", "Warp Bass.xml" visible
3. **Whitespace tolerant** - typing "bass lead" keeps "Bass Lead.xml", "Bass  Lead.xml", "BassLead.xml" visible
4. **Special character agnostic** - typing "bass lead" keeps "bass-lead.xml", "bass_lead.xml", "bass.lead.xml" visible

### Visual Feedback
- The letter's keyboard pad lights up green when it is part of the text filter in play (search text)
- The lett'ers keyboard pad lights up standard when it is not being used in the text filter (search text)
- Filtered items remain in their original positions (no re-ordering)

### Filter Persistence
- Filter text persists while browsing within same directory
- Filter automatically clears when:
  - Entering a subdirectory
  - Going up to parent directory
  - Exiting file browser

## File Types Affected
- Synth presets: `/SYNTHS/*.XML`
- Kit presets: `/KITS/*.XML`
- Sample files: `/SAMPLES/**/*.*`
- All subdirectories in samples folder

## Example Filtering Scenarios

### Scenario 1: Progressive Filtering in Synths
Initial view shows all 170 synth presets

User types "b":
- List narrows to ~40 presets containing "b"

User types "ba":
- List narrows to ~15 presets containing "ba"

User types "bas":
- List narrows to ~12 presets containing "bas"

User types "bass":
- List shows only: Acid Bass, Bass Lead, Dubstep Bass, Trap Bass 1, Trap Bass 2

### Scenario 2: Backspace Behavior
Current filter: "bass" (showing 5 items)

User backspaces to "bas":
- Additional items reappear (12 items visible)

User clears all text:
- All 170 presets visible again
- Dot indicator disappears

### Scenario 3: Folder Navigation
1. In `/SAMPLES/` with 50 items visible
2. User types "kick" - list filters to 8 items
3. User selects "Kick Drums/" folder
4. Filter clears automatically
5. All contents of "Kick Drums/" folder shown
6. Dot indicator disappears

## Performance Requirements
- Filtering must update within 50ms of keypress
- Scrolling through filtered results remains smooth
- No lag when typing quickly
- No audio interruption during filtering

## Edge Cases
- Empty filter text shows all items (default state)
- No matches shows empty list with clear indication
- Single character filters work correctly
- Very long search strings handled gracefully
- Special characters in search text don't break filtering

## Success Criteria
1. Users can quickly narrow down large file lists
2. Filter response feels instantaneous
3. Clear visual indication when filter is active
4. Intuitive behavior when navigating folders
5. No impact on browser performance or stability
