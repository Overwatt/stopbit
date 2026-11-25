---
name: N540 gateway
description: A bench instrument for four RS485 ports, read and operated in a browser.
colors:
  solder-mask: "#0d6658"
  solder-mask-deep: "#0a5247"
  solder-mask-press: "#073f37"
  on-mask: "#ffffff"
  bench: "#f3f5f3"
  panel: "#ffffff"
  ink: "#18201d"
  ink-soft: "#55605b"
  line: "#d5dbd7"
  rule: "#edf0ee"
  well: "#ebefec"
  key: "#e2e8e5"
  key-hover: "#d5ddd9"
  key-press: "#c6d0cb"
  field-line: "#76827c"
  ok: "#17703d"
  warn: "#8a5300"
  warn-ground: "#fdf1dc"
  bad: "#b3261e"
  bad-ground: "#fcebe9"
  idle: "#68736e"
  solder-mask-dark: "#4fb8a3"
  solder-mask-deep-dark: "#6ccab7"
  solder-mask-press-dark: "#3d9a88"
  on-mask-dark: "#0b1a16"
  bench-dark: "#131816"
  panel-dark: "#1b211f"
  ink-dark: "#e6ebe8"
  ink-soft-dark: "#a3afa9"
  line-dark: "#313a36"
  rule-dark: "#262d2a"
  well-dark: "#121715"
  key-dark: "#2b3430"
  key-hover-dark: "#36403c"
  key-press-dark: "#222a27"
  field-line-dark: "#74827b"
  ok-dark: "#62c98b"
  warn-dark: "#e7b45e"
  warn-ground-dark: "#32270f"
  bad-dark: "#f2877e"
  bad-ground-dark: "#391a18"
  idle-dark: "#8a968f"
typography:
  headline:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: "1.25rem"
    fontWeight: 650
    lineHeight: 1.3
  title:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: "1.0625rem"
    fontWeight: 650
  body:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: "15px"
    fontWeight: 400
    lineHeight: 1.5
    fontFeature: "tnum"
  label:
    fontFamily: "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
    fontSize: ".875rem"
    fontWeight: 400
  mono:
    fontFamily: "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"
    fontSize: ".875em"
rounded:
  inner: "4px"
  control: "6px"
  panel: "8px"
spacing:
  xs: "4px"
  sm: "10px"
  md: "16px"
  lg: "20px"
  section: "36px"
components:
  button:
    backgroundColor: "{colors.key}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    padding: "9px 18px"
    height: "44px"
  button-hover:
    backgroundColor: "{colors.key-hover}"
  button-active:
    backgroundColor: "{colors.key-press}"
  button-primary:
    backgroundColor: "{colors.solder-mask}"
    textColor: "{colors.on-mask}"
    rounded: "{rounded.control}"
    padding: "9px 18px"
    height: "44px"
  button-primary-hover:
    backgroundColor: "{colors.solder-mask-deep}"
  button-primary-active:
    backgroundColor: "{colors.solder-mask-press}"
  button-danger:
    backgroundColor: "{colors.bad-ground}"
    textColor: "{colors.bad}"
  field:
    backgroundColor: "{colors.well}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    padding: "9px 12px"
    height: "44px"
  panel:
    backgroundColor: "{colors.panel}"
    rounded: "{rounded.panel}"
    padding: "18px 20px"
  nav-link:
    textColor: "{colors.ink-soft}"
    padding: "11px 12px"
  nav-link-current:
    textColor: "{colors.ink}"
  notice-warn:
    backgroundColor: "{colors.warn-ground}"
    textColor: "{colors.warn}"
    rounded: "{rounded.panel}"
    padding: "12px 16px"
  notice-bad:
    backgroundColor: "{colors.bad-ground}"
    textColor: "{colors.bad}"
    rounded: "{rounded.panel}"
    padding: "12px 16px"
---

# Design System: N540 gateway

## Overview

**Creative North Star: "The Bench Instrument"**

A well-made meter on a workbench. Readings line up, nothing decorates, and a fault is
obvious. The interface is plain and exact, calm, and friendly in the way a good tool is:
short words that help, with no chatter.

Everything sits on one faint grey-green bench. One teal, the colour of the coating on a
circuit board, marks what can be operated and what is current. Green, amber and red belong
to state and nothing else, so when one appears it means something.

It must never look like the vendor's old web UI (frames and grey forms), and it must never
look like a marketing dashboard (big number tiles, gradients, charts for show).

**Key Characteristics:**
- System type with tabular figures; numbers align and hold still.
- Light and dark follow the viewer's setting, with the same roles in both.
- Tables for readings, plain forms for settings, one column of controls at a time.
- Colour and motion only when something is operable, current, or wrong.
- Panels are lifted; inside them, tone does the work of lines.
- Controls are sturdy and deliberate: 44px, clear shapes, a visible press.

## Colors

Solder-mask teal and cool grey: one saturated colour on neutrals that lean slightly green.

### Primary
- **Solder-mask teal** (#0d6658, dark #4fb8a3): the primary button, the current page's
  underline, a hovered or focused field's edge, focus outlines, text selection, checkbox
  fill and the caret. Hover deepens it (#0a5247; in dark it lightens, #6ccab7); pressing
  goes further (#073f37, dark #3d9a88).

### Neutral
- **Bench** (#f3f5f3, dark #131816): the page.
- **Panel** (#ffffff, dark #1b211f): header, panels, tables.
- **Ink** (#18201d, dark #e6ebe8): readings, names, values.
- **Soft ink** (#55605b, dark #a3afa9): labels, table heads, second lines, inactive tabs.
- **Well** (#ebefec, dark #121715): anything recessed. Fields, table heads, an open editor.
- **Key** (#e2e8e5, dark #2b3430): secondary buttons, with a hover and a pressed step.
- **Rule** (#edf0ee, dark #262d2a): the faint line between table rows.
- **Line** (#d5dbd7, dark #313a36): the edge of a disabled button, only.
- **Field line** (#76827c, dark #74827b): the 2px foot of a field, at least 3:1.

### State
- **Running green** (#17703d, dark #62c98b): connected, listening, ready, saved.
- **Caution amber** (#8a5300, dark #e7b45e) on its own ground: connecting, on trial,
  sign-in needed.
- **Fault red** (#b3261e, dark #f2877e) on its own ground: errors, offline, destructive
  confirmation.
- **Idle grey** (#68736e, dark #8a968f): off. Readable as text (4.5:1) in both themes.

### Named Rules
**The State-Only Rule.** Green, amber and red appear only for state, never as decoration
or category.
**The Dot-and-Word Rule.** State is never colour alone: a dot and a word carry it together.

## Typography

**Body Font:** system-ui (with -apple-system, Segoe UI, Roboto, sans-serif)
**Mono Font:** ui-monospace (with SFMono-Regular, Menlo, Consolas), for commands and the chip
serial only

**Character:** one family at close sizes, separated by weight and ink. No web fonts: the
gateway must work with no internet.

### Hierarchy
- **Headline** (650, 1.25rem, 1.3): the device name. One per page.
- **Title** (650, 1.0625rem): section headings above panels.
- **Body** (400, 15px, 1.5): readings, values, messages. Prose stops at 70ch.
- **Emphasis** (600): port and part names in tables, fieldset legends.
- **Label** (400, .875rem, soft ink): field labels, second lines, hints. Table heads use 550.

### Named Rules
**The Steady Figures Rule.** Figures are tabular everywhere, and a changing number never
moves its column.

## Layout

One centred column, 1080px at most, with 20px gutters (16px when narrow). A header carries
the device name, address and uptime, sign-in, and four tabs. Below it, sections are a title
and one panel: 36px above a title, 10px below it. Where two sections belong together they
sit in two equal columns 20px apart.

Tables have fixed layouts with stated column widths. Form fields fill a grid of columns at
least 150px wide, 16px apart across and 12px down.

There is one breakpoint, 1100px, the width the full table needs. Below it the columns stack, each table row becomes a
two-column block with its headings repeated in soft ink, and the row's button spans its cell.
Tap targets are 44px at every width.

### Named Rules
**The Still Table Rule.** Live values change in place. Column widths and row positions do
not react to them.

## Elevation & Depth

Two levels and a recess. Panels and the header are lifted off the bench by one soft shadow
and have no border. Inside a panel there are no boxes: fields, table heads and open editors
sink to the well tone. In an open editor the relation flips, and its fields and buttons rise
to the panel tone.

Focus is the one ring: 2px of teal, 2px clear of the control.

### Shadow Vocabulary
- **Lift** (`box-shadow: 0 1px 2px rgba(24,32,29,.08), 0 4px 14px rgba(24,32,29,.07)`; dark
  `0 1px 2px rgba(0,0,0,.35), 0 4px 14px rgba(0,0,0,.28)`): panels and the header. Nothing
  else casts a shadow.

### Named Rules
**The One Lift Rule.** There is a single shadow and a single raised level. Nothing is lifted
off something already lifted.
**The Tone-Not-Line Rule.** Inside a panel, areas are told apart by tone. Lines are for the
rule between rows and the foot of a field.

## Shapes

Gently rounded rectangles: panels 8px, buttons 6px, the file picker's inner button 4px.
Fields are 6px at the top and 2px at the foot, where they carry their 2px edge. Circles
appear only as state dots (9px). Tabs are text with a 3px underline, not boxes.

## Components

### Buttons
Sturdy and deliberate, like switches on equipment.
- **Shape:** at least 44px tall, 6px corners, 9px by 18px (9px by 12px in a table row), weight
  600, no border.
- **Primary:** teal with white ink (dark ink on light teal in dark mode). One per form.
- **Secondary:** key tone; panel tone inside an open editor or a notice.
- **Hover / Active:** one tone deeper, then another while pressed. 150ms colour change only.
- **Danger:** fault red on its ground, for the second press of a two-press confirm. The
  button renames itself ("Restart" to "Confirm restart") for four seconds.
- **Disabled:** no fill, soft ink, a 1px line, and a message beside it saying what to do.

### Cards / Containers
- **Corner Style:** 8px.
- **Background:** panel on bench. Never nested.
- **Shadow Strategy:** the Lift. No border.
- **Internal Padding:** 18px by 20px for forms; tables run edge to edge.

### Inputs / Fields
Recessed, with a firm foot.
- **Style:** label above in soft ink; well tone, no side edges, a 2px foot, at least 44px tall.
- **Hover / Focus:** the foot turns teal; focus adds the 2px ring and a teal caret.
- **Disabled:** 60% opacity.
- **Checkbox:** native, 20px, teal, with a title and an optional second line stating the
  consequence.

### Navigation
Four text tabs under the device name, at least 44px tall. Soft ink; ink and a grey underline
on hover; ink and a 3px teal underline when current. Same on a phone.

### Reading table
The signature component. A well-toned head, then one row per port or part: a bold name, then
readings. Numbers are
right-aligned with a second line in soft ink (the rate under a total). State is a dot and a
word. A row's editor opens beneath it, in the well, inside the table.

### Reading list
Term in soft ink, value in ink, two columns. Used for settings in effect, firmware, memory.

### Notice
Full-width, 8px corners, amber or red ink on its own ground, no edge, with its action on the
right. For things that will happen unless the viewer acts, or that have stopped working.

## Do's and Don'ts

### Do:
- **Do** give every live number a fixed column and tabular figures.
- **Do** pair every state colour with a word.
- **Do** keep one primary button per form, and say beside a disabled one what unlocks it.
- **Do** use the same roles in light and dark; change values, not meaning.
- **Do** ship with system fonts and inline drawing only.

### Don't:
- **Don't** look like the vendor's old web UI: frames, grey forms.
- **Don't** look like a marketing dashboard: big number tiles, gradients, charts for show.
- **Don't** use green, amber or red for anything but state.
- **Don't** nest panels, or add a second shadow.
- **Don't** box things inside a panel with borders.
- **Don't** animate anything but colour on hover.
