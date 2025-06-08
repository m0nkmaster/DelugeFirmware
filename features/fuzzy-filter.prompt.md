# Fuzzy Filtering Feature Requirements

## Feature Overview
Add real-time filtering when choosing a synthesizer, kit or sample file. To narrow down the visible file list as the user types, showing only items that match the search text anywhere in their filenames. They can then scroll to choose the desired item. This functionality only is for choosing a synthesizer kit or sample. No other menus.

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
- The letter's keyboard pad lights up standard when it is not being used in the text filter (search text)
- Filtered items remain in their original positions (no re-ordering)

### Filter Persistence
- Filter text persists when browsing/scrolling within same directory
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

## Test data

Here are the synthesizer files which are on the device I am testing with. It includes five subfolders.

| Name | Size |
|------|------|
| 📁 Robs | |
| Rob Lead.XML | 4 KiB |
| Rob Hollow Bells.XML | 6 KiB |
| Rob Exp.XML | 5 KiB |
| Rob Deep Atmos.XML | 5 KiB |
| Rob Bass.XML | 5 KiB |
| Rob Atmos.XML | 5 KiB |
| Rob Arp.XML | 5 KiB |
| R849 OPZ Sparkling Pluck.XML | 4 KiB |
| R848 Broken Machine Pad.XML | 6 KiB |
| R847 OPZ Tape Organ.XML | 3 KiB |
| R846 VHS MC Pluck and Arp.XML | 5 KiB |
| R845 VHS MC Bass and Lead.XML | 5 KiB |
| R844 You Know Pluck.XML | 5 KiB |
| R843 Zen Dreamy Drips.XML | 6 KiB |
| R842 Zen Wide Strings.XML | 6 KiB |
| R841 VHS MC Pad1.XML | 5 KiB |
| R840 Moving Pad2.XML | 5 KiB |
| R839 Moving Pad1.XML | 5 KiB |
| R838 Boutique Gated Pad.XML | 5 KiB |
| R837 Imposter Lead Pad.XML | 5 KiB |
| R836 Noisy Square Pad.XML | 5 KiB |
| R835 Boutique Wide Pad4.XML | 5 KiB |
| R834 Boutique Wide Pad3.XML | 5 KiB |
| R833 Boutique Wide Pad2.XML | 5 KiB |
| R832 Boutique Wide Pad1.XML | 6 KiB |
| R831 Slow Choir Pluck.XML | 5 KiB |
| R830 FM Gated Pad.XML | 5 KiB |
| R829 Boutique Slow Pluck2.XML | 6 KiB |
| R828 Boutique Slow Pluck1.XML | 5 KiB |
| R827 Vocal Pluck.XML | 5 KiB |
| R826 Zen Lead.XML | 6 KiB |
| R825 Boutique Pluck3.XML | 5 KiB |
| R824 Boutique Pluck2.XML | 5 KiB |
| R823 Boutique Pluck1.XML | 5 KiB |
| R822 InSpire Arp.XML | 5 KiB |
| R821 Roland Arp.XML | 6 KiB |
| R820 SQ8 Arp.XML | 6 KiB |
| R819 Wave Arp.XML | 6 KiB |
| R818 Blooming Arp.XML | 6 KiB |
| R817 SpeakAndSpell Base.XML | 7 KiB |
| R816 MonoPoly Base2.XML | 7 KiB |
| R815 MonoPoly Base1.XML | 7 KiB |
| R814 VHS MC Base.XML | 3 KiB |
| R813 EBM Base.XML | 5 KiB |
| R812 OP1 Base.XML | 3 KiB |
| R811 Music Box.XML | 6 KiB |
| R810 Intimate Choir.XML | 4 KiB |
| R809 Evolving Choir.XML | 4 KiB |
| R808 Hopes Granular Voice.XML | 3 KiB |
| R807 OPZ Synth Strings.XML | 3 KiB |
| R806 Modern Tron Strings.XML | 7 KiB |
| R805 OP1 Organ.XML | 3 KiB |
| R804 Organ.XML | 7 KiB |
| R803 Electric Piano.XML | 8 KiB |
| R802 Rhodes.XML | 6 KiB |
| R801 Piano and Strings.XML | 8 KiB |
| R800 Blooming Field Piano.XML | 9 KiB |
| 📁 Factory | |
| 📁 DEL-1092 | |
| 📁 Boards of Deluge 3 | |
| 📁 Boards of Deluge 2 | |
| 9. Dt Fm Gate Loop - Rephazer DigitalTones Soundpack.XML | 6 KiB |
| 8. Dt Pad - Rephazer DigitalTones Soundpack.XML | 7 KiB |
| 7. Ad Bass Arp - Rephazer AnalogBelour Soundpack.XML | 6 KiB |
| 6. A4 Interval Signal Pluck - Rephazer AnalogBelour Soundpack.XML | 6 KiB |
| 5. Boc Pad - Rephazer Rust Soundpack.XML | 4 KiB |
| 4. Dark Choir - Rephazer Afterlife Soundpack.XML | 4 KiB |
| 3. Electric Guitar - Rephazer Afterlife Soundpack.XML | 13 KiB |
| 2. Modern Tron Strings - Rephazer Blooming Field Soundpack.XML | 7 KiB |
| 10. Synth Arp - Rephazer Rust Soundpack.XML | 4 KiB |
| 1. Lite Piano - Rephazer X-Selected.XML | 7 KiB |
