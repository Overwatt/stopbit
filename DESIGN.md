---
name: N540 gateway
description: Every part of the gateway is a station that reports its own condition.
colors:
  shell: "#0d4a68"
  shell-soft: "#bfe0ee"
  on-shell: "#ffffff"
  bad-on-shell: "#ffd0ca"
  accent: "#0d4a68"
  accent-h: "#0a3b53"
  accent-p: "#072c3f"
  accent-ink: "#ffffff"
  sea: "#1683b8"
  ground: "#dcebf1"
  sheet: "#ffffff"
  ink: "#0b2230"
  soft: "#2d5468"
  rule: "#c3d7e1"
  well: "#eaf3f7"
  band: "#f1f7fa"
  foot: "#5b7f91"
  key: "#dfebf1"
  key-h: "#cfe0e8"
  key-p: "#bed3de"
  ok: "#17703d"
  warn: "#7d4b00"
  warn-bg: "#fdf0d6"
  bad: "#a3211a"
  bad-bg: "#ffe1dc"
  idle: "#4e6f7f"
  shell-dark: "#0d3a50"
  shell-soft-dark: "#a9cfdf"
  accent-dark: "#5bb8e2"
  accent-h-dark: "#7cc8eb"
  accent-p-dark: "#46a3cd"
  accent-ink-dark: "#06212d"
  sea-dark: "#3f9fd0"
  ground-dark: "#05121a"
  sheet-dark: "#173443"
  ink-dark: "#e6f2f7"
  soft-dark: "#a9c7d4"
  rule-dark: "#2c5061"
  well-dark: "#102733"
  band-dark: "#132d3a"
  foot-dark: "#86a9b9"
  key-dark: "#254a5c"
  key-h-dark: "#305a6e"
  key-p-dark: "#1d3d4c"
  ok-dark: "#62c98b"
  warn-dark: "#e7b45e"
  warn-bg-dark: "#352a10"
  bad-dark: "#ff9a8f"
  bad-bg-dark: "#451c18"
  idle-dark: "#8fb0be"
typography:
  display:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: "2.5rem"
    fontWeight: 750
    lineHeight: 1.1
    letterSpacing: "-.02em"
  headline:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: "2.125rem"
    fontWeight: 750
    lineHeight: 1.1
    letterSpacing: "-.02em"
  report:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: "1.5rem"
    fontWeight: 400
    lineHeight: 1.4
  title:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: "1.25rem"
    fontWeight: 750
    lineHeight: 1.1
    letterSpacing: "-.01em"
  reading:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: "1.25rem"
    fontWeight: 650
    fontFeature: "tnum"
  station:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: "1.0625rem"
    fontWeight: 650
  body:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: "16px"
    fontWeight: 400
    lineHeight: 1.45
    fontFeature: "tnum"
  label:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: ".9375rem"
    fontWeight: 400
  small:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: ".875rem"
    fontWeight: 400
    lineHeight: 1.4
  mono:
    fontFamily: "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"
    fontSize: ".875em"
rounded:
  inner: "4px"
  control: "6px"
  row: "8px"
  chart: "10px"
  sheet: "16px"
spacing:
  station-gap: "4px"
  action-gap: "10px"
  field-gap: "14px"
  gutter-phone: "16px"
  reading-gap: "20px"
  gutter: "32px"
  section: "40px"
  column-gap: "48px"
components:
  button:
    backgroundColor: "{colors.key}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    padding: "9px 18px"
    height: "44px"
  button-hover:
    backgroundColor: "{colors.key-h}"
  button-active:
    backgroundColor: "{colors.key-p}"
  button-primary:
    backgroundColor: "{colors.accent}"
    textColor: "{colors.accent-ink}"
    rounded: "{rounded.control}"
    padding: "9px 18px"
    height: "44px"
  button-primary-hover:
    backgroundColor: "{colors.accent-h}"
  button-primary-active:
    backgroundColor: "{colors.accent-p}"
  button-on-shell:
    backgroundColor: "{colors.on-shell}"
    textColor: "{colors.shell}"
    rounded: "{rounded.control}"
    padding: "9px 18px"
    height: "44px"
  button-on-shell-hover:
    backgroundColor: "{colors.shell-soft}"
  button-danger:
    backgroundColor: "{colors.bad-bg}"
    textColor: "{colors.bad}"
  field:
    backgroundColor: "{colors.well}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    padding: "9px 12px"
    height: "44px"
  station-group:
    textColor: "{colors.soft}"
    typography: "{typography.small}"
    padding: "22px 16px 6px"
  station:
    textColor: "{colors.ink}"
    typography: "{typography.station}"
    rounded: "{rounded.row}"
    padding: "8px 16px"
    height: "50px"
  station-current:
    backgroundColor: "{colors.sheet}"
  station-warn:
    textColor: "{colors.warn}"
  station-bad:
    backgroundColor: "{colors.bad-bg}"
    textColor: "{colors.bad}"
  sheet:
    backgroundColor: "{colors.sheet}"
    textColor: "{colors.ink}"
    padding: "34px 44px 56px"
  chart:
    backgroundColor: "{colors.well}"
    rounded: "{rounded.chart}"
    padding: "16px 18px 10px"
  notice-warn:
    backgroundColor: "{colors.warn-bg}"
    textColor: "{colors.warn}"
    rounded: "{rounded.row}"
    padding: "12px 16px"
  notice-bad:
    backgroundColor: "{colors.bad-bg}"
    textColor: "{colors.bad}"
    rounded: "{rounded.row}"
    padding: "12px 16px"
---

# Design System: N540 gateway

## Overview

**Creative North Star: "Station Reports"**

Marine area forecasts: a fixed list of stations, each with its condition, and a report you
read in one sentence. Every part of the gateway is a station that reports its own condition.
The list says whether the gateway is healthy before anything is clicked; the sheet says what
one part is doing and what went wrong.

The page is plain and exact, calm, and friendly but brief. Three flat sea-blue surfaces carry
it: a deep shell, a pale ground, and one sheet. Saturated colour on top of them means a state,
something that can be operated, or measured traffic.

It must never look like the vendor's old web UI (frames and grey forms), and it must never
look like a marketing dashboard (big number tiles, gradients, charts for show).

**Key Characteristics:**
- A verdict, then a list in a fixed order, then one report.
- State marked by shape, word and colour together.
- One heavy system sans with tabular figures; live values never move the layout.
- Flat. One sheet on one ground, told apart by tone.
- Light and dark follow the viewer's setting, with the same roles in both.
- Controls are sturdy and deliberate: 44px, clear shapes.
- Terse, facts only. No internet assets.

## Colors

Sea blues from near-black to pale, with green, amber and red kept for state.

### Primary
- **Deep Sea** (#0d4a68, dark #0d3a50): the shell. White on it for the name;
  **Spray** (#bfe0ee, dark #a9cfdf) for everything quieter there.
- **Operable Blue** (#0d4a68, dark #5bb8e2): the primary button, checkbox fill and text
  selection. In light it is Deep Sea itself; in dark it turns bright with dark ink (#06212d).
  Hover #0a3b53, pressed #072c3f; dark #7cc8eb and #46a3cd.
- **Signal Sea** (#1683b8, dark #3f9fd0): the traffic line, the focus ring, a hovered or
  focused field's foot, and the caret. Lines and marks only, not text: it is under 4.5:1.

### Neutral
- **Pale Sea Ground** (#dcebf1, dark #05121a): the page, under the list and the notices.
- **Sheet** (#ffffff, dark #173443): the report. Also the selected station.
- **Ink** (#0b2230, dark #e6f2f7): names, readings, sentences.
- **Soft Ink** (#2d5468, dark #a9c7d4): labels, readings in the list, second lines, hints.
- **Well** (#eaf3f7, dark #102733): fields and the chart.
- **Key** (#dfebf1, dark #254a5c): secondary buttons, with a hover (#cfe0e8, dark #305a6e) and
  a pressed step (#bed3de, dark #1d3d4c).
- **Rule** (#c3d7e1, dark #2c5061): between table rows, and chart grid lines.
- **Foot** (#5b7f91, dark #86a9b9): a field's 2px foot and a disabled button's edge; at least
  3:1.

### State
- **Running green** (#17703d, dark #62c98b): connected, listening, kept, saved. A disc.
- **Caution amber** (#7d4b00, dark #e7b45e): errors counted, retrying, on trial, sign-in
  needed. A triangle. Ground #fdf0d6 (dark #352a10), notices only.
- **Fault red** (#a3211a, dark #ff9a8f): not working, or the second press of a confirm. A
  square, on its own ground (#ffe1dc, dark #451c18). On the shell, #ffd0ca.
- **Idle slate** (#4e6f7f, dark #8fb0be): off, no client, quiet, or not known. A ring. Marks
  only; words beside it are Soft Ink.

The logo is the green four-way screw terminal on the box, with a wire in port 1, and has its
own palette, separate from the page: terminal green (#1f8f4e, its lower edge #17763f), wire
holes (#0c3a20), screw heads (#d4dade, slots #7a8790), and wire orange (#ff8a1f) for the live
part. No tile; one drawing at every size. It does not change between themes.

### Named Rules
**The State-Only Rule.** Green, amber and red appear only for state, never as decoration or
category. Colour belongs to state and to what can be operated.
**The Mark-and-Word Rule.** State is never colour alone. Every station has a mark, and a word
or reading beside it: disc running, ring idle, triangle caution, square fault. Elsewhere a
state is stated in words, and colour repeats it.
**The Logo Wire Rule.** The orange wire in the logo is the live part. The terminal's green is
not the page's running green, and neither palette borrows from the other.

## Typography

**Display and Body Font:** system-ui (with -apple-system, Segoe UI, Roboto, sans-serif)
**Mono Font:** ui-monospace (with SFMono-Regular, Menlo, Consolas), for commands and the chip
serial only

**Character:** one family at a wide range of sizes: heavy tight headings, and report
sentences large enough to read from a step away. Nothing is fetched; the gateway must work
with no internet.

### Hierarchy
- **Display** (750, 2.5rem, 1.1, -.02em; 2rem on a phone): the sheet's name.
- **Headline** (750, 2.125rem, 1.1, -.02em): the verdict.
- **Report** (400, 1.5rem, 1.4; 1.25rem on a phone): the sentence under a sheet's name. Stops
  at 44ch.
- **Title** (750, 1.25rem, -.01em): sections and fieldsets in the sheet.
- **Reading** (650, 1.25rem): a value under its label in the report.
- **Station** (650, 1.0625rem): names in the list. The device name is 700, 1.1875rem.
- **Body** (400, 16px, 1.45): list readings, table cells, messages. Prose stops at 66ch.
  Buttons are 600.
- **Label** (400, .9375rem, soft ink): field and reading labels. Table heads use 500.
- **Small** (400, .875rem, soft ink): second lines and hints. Chart axes are 13px.

### Named Rules
**The Steady Figures Rule.** Figures are tabular everywhere.
**The Plain Sentence Rule.** A report is short declarative sentences, facts only: who it is
connected to, what it hears, what went wrong. The register is plain and a little formal: no
idioms ("all good", "by itself"), "responding" not "answering", "reverts" not "goes back".
The product's own terms stay: heard, sent, turned away, station.

## Layout

A shell bar across the top (72px): logo, name, address and uptime, sign-in. Notices
come under it, on the ground. Then two columns: the list (400px) and the sheet, which runs to
the right and bottom edges and starts 26px below whatever is above it. Sheet content stops at
1000px, inside 34px by 44px padding. The outer gutter is 32px.

The list opens with the verdict and one line, in a block at least 152px tall. Stations are
50px rows, 4px apart, in two groups in a fixed order: Ports (1 to 4), then Config (MQTT,
Network, Firmware, Hardware, Events), each under a small soft label.

In the sheet: the name with its one action at the right and the sentence (two lines reserved)
sit in the band, a tone between sheet and ground (#f1f7fa, dark #132d3a) that runs to the
sheet's top and side edges and fades into the sheet at its foot, the page's one gradient.
What follows starts 30px below the sentence. Then
charts, then readings four across, 24px by 20px apart. Sections sit 40px apart; pairs sit in
two equal columns with 48px between. Fields fill columns at least 160px wide, 14px by 18px
apart. Tables have fixed layouts.

Below 1240px the list narrows to 320px, pairs stack, and readings go three across. Below 860px
it is one column and one view at a time: the list, or a sheet with "All stations" above its
name. Gutters become 16px, readings two across, the sentence reserves three lines, and table
rows become two-column blocks with their headings repeated.

### Named Rules
**The Still List Rule.** Live values change in place. Stations never reorder or move by
severity: a fault fills its own row and is named in the verdict.

## Elevation & Depth

Flat: no shadows, and nothing is lifted. Depth is tone, in order: shell, ground, band, sheet, well.
The sheet lies on the ground; fields and charts are cut into the sheet. In dark the sheet is
lighter than the ground and the well sits between them.

The selected station takes the sheet's tone, so list and report read as one surface. Inset
rings are edges, not depth: 1px on a disabled button, 2px of red on a selected fault. Focus is
a 2px ring of Signal Sea, 2px clear of the control; on the shell it is white and inset.

### Named Rules
**The One Sheet Rule.** There is one sheet. Nothing inside it is another card; sections are
separated by space and a heading.

## Shapes

The sheet has a single rounded corner, top left (16px), and runs off the other edges; on a
phone both top corners round. Stations and notices are 8px, charts 10px, buttons 6px, the file
picker's inner button 4px. Fields are 6px at the top and 2px at the foot, where they carry
their 2px edge.

State marks are 14px: a filled disc, an open ring, a triangle, a square with barely eased
corners. Those shapes are reserved for state. The same triangle marks errors on the chart.

## Components

### Buttons
Sturdy and deliberate.
- **Shape:** at least 44px tall, 6px corners, 9px by 18px, weight 600, no border.
- **Primary:** Operable Blue. One per form; on a report, the one action it has.
- **Secondary:** Key. Sheet tone inside a notice.
- **On the shell:** white with Deep Sea ink in both themes; Spray on hover.
- **Hover / Active:** one tone further, then another while pressed. 150ms, colour only,
  and none under reduced motion.
- **Danger:** Fault red on its ground, for the second press of a two-press confirm. The button
  renames itself ("Restart" to "Confirm restart") for four seconds.
- **Disabled:** no fill, soft ink, a 1px inset edge. A form's disabled save has a message
  beside it saying what unlocks it.

### Cards / Containers
There are no cards. The sheet is the only container (see The One Sheet Rule); wells hold
fields and charts.

### Inputs / Fields
Cut in, with a firm foot.
- **Style:** label above in soft ink; well tone, no side edges, a 2px foot, at least 44px tall.
- **Hover / Focus:** the foot turns Signal Sea; focus adds the ring.
- **Disabled:** 60% opacity.
- **Checkbox:** native, 20px, Operable Blue, with a title and an optional second line stating
  the consequence.

### Navigation
The station list is the navigation; the shell carries no tabs. Its two groups, Ports and
Config, are labelled in Small weight 650, soft ink. Below 860px every sheet starts with "All
stations", back to the list. In-page links are ink with an underline, not coloured.

### Station list
The signature component. A mark, a bold name, and a reading at the right, each on one line;
long ones are cut with an ellipsis.
- **Hover:** sheet tone at 55%. **Selected:** sheet tone.
- **Caution:** amber mark, name and reading, no fill.
- **Fault:** red on its filled ground. Selected, it keeps the fill and gains the 2px red ring.
- **Choosing one** swaps the sheet in place; the list holds still.
- **Not answering:** every mark becomes a ring, readings go italic, the chart line fades, and
  the verdict says so with how long ago the last answer came. Old readings never look live.

### Verdict
"All normal", or the worst fault by name, as a link to its station. One line under it says what
else is wrong.

### Traffic chart
In a well, 200px tall (170px on a phone). One series per chart: a 2px Signal Sea line over a
14% fill, three grid lines, three time labels. The caption names the series and its peak. A
gap is labelled in words. Errors are amber triangles on the baseline. Hover, or the arrow keys
when focused, shows a rule, a point and a tooltip in ink. The same data is available as a table below.

### Reading grid and reading list
The report's eight readings are label, value and optional second line, with no boxes. A
compact two-column list (soft term, 600 value) carries settings in effect, firmware and memory.

### Reading table
No head fill, 1px rules between rows, names at 650, problems as coloured words in the cell.

### Notice
Full width on the ground, 8px corners, amber or red ink on its own ground, no edge, its action
on the right. For things that will happen unless the viewer acts, or that have stopped working.

## Do's and Don'ts

### Do:
- **Do** pair every state colour with a word, and in the list with its mark too.
- **Do** name a fault in words, in the verdict and on its station.
- **Do** reserve space for live text, and use tabular figures.
- **Do** keep one primary button per form, and say beside a disabled save what unlocks it.
- **Do** use the same roles in light and dark; change values, not meaning.
- **Do** put everything in the page itself: inline drawing, no internet assets.
- **Do** stay terse: facts, and the consequences that matter.

### Don't:
- **Don't** look like the vendor's old web UI: frames, grey forms.
- **Don't** look like a marketing dashboard: big number tiles, gradients, charts for show.
- **Don't** use green, amber or red for anything but state.
- **Don't** use the disc, ring, triangle or square as decoration.
- **Don't** reorder the list by severity.
- **Don't** add shadows, or a card inside the sheet.
- **Don't** bring the logo's green or orange into the page.
- **Don't** animate anything but colour.
