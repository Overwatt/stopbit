---
name: N540 gateway
description: A bench instrument for four RS485 ports, read and operated in a browser.
colors:
  solder-mask: "#0d6658"
  solder-mask-deep: "#0a5247"
  solder-mask-tint: "#e4efec"
  on-mask: "#ffffff"
  bench: "#f3f5f3"
  panel: "#ffffff"
  ink: "#18201d"
  ink-soft: "#55605b"
  line: "#d5dbd7"
  field: "#ffffff"
  field-line: "#8b9791"
  ok: "#17703d"
  warn: "#8a5300"
  warn-ground: "#fdf1dc"
  bad: "#b3261e"
  bad-ground: "#fcebe9"
  idle: "#7b8781"
  solder-mask-dark: "#4fb8a3"
  solder-mask-deep-dark: "#6ccab7"
  solder-mask-tint-dark: "#1f2f2b"
  on-mask-dark: "#0b1a16"
  bench-dark: "#131816"
  panel-dark: "#1b211f"
  ink-dark: "#e6ebe8"
  ink-soft-dark: "#a3afa9"
  line-dark: "#313a36"
  field-dark: "#141917"
  field-line-dark: "#66736d"
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
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    padding: "7px 14px"
  button-hover:
    backgroundColor: "{colors.solder-mask-tint}"
  button-primary:
    backgroundColor: "{colors.solder-mask}"
    textColor: "{colors.on-mask}"
    rounded: "{rounded.control}"
    padding: "7px 14px"
  button-primary-hover:
    backgroundColor: "{colors.solder-mask-deep}"
  button-danger:
    textColor: "{colors.bad}"
  field:
    backgroundColor: "{colors.field}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    padding: "7px 9px"
    height: "38px"
  panel:
    backgroundColor: "{colors.panel}"
    rounded: "{rounded.panel}"
    padding: "18px 20px"
  nav-link:
    textColor: "{colors.ink-soft}"
    padding: "9px 12px"
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

**Agreed direction, not built yet (2026-11-25):** panels lifted on a soft shadow without
borders; inside them, tone instead of lines, with recessed fields and toned secondary
buttons; controls sturdier and more deliberate than today's (44px instead of about 38px).

## Colors

Solder-mask teal and cool grey: one saturated colour on neutrals that lean slightly green.

### Primary
- **Solder-mask teal** (#0d6658, dark #4fb8a3): the primary button, the current page's
  underline, focus outlines, text selection, checkbox fill and the caret. Hover deepens it
  (#0a5247; in dark it lightens, #6ccab7).
- **Mask tint** (#e4efec, dark #1f2f2b): hover on secondary buttons, the file picker's
  button.

### Neutral
- **Bench** (#f3f5f3, dark #131816): the page, and the ground of an open row editor.
- **Panel** (#ffffff, dark #1b211f): header, panels, tables.
- **Ink** (#18201d, dark #e6ebe8): readings, names, values.
- **Soft ink** (#55605b, dark #a3afa9): labels, table heads, second lines, inactive tabs.
- **Line** (#d5dbd7, dark #313a36): panel edges and row rules.
- **Field line** (#8b9791, dark #66736d): edges of fields and buttons, 3:1 on panel.

### State
- **Running green** (#17703d, dark #62c98b): connected, listening, ready, saved.
- **Caution amber** (#8a5300, dark #e7b45e) on its own ground: connecting, on trial,
  sign-in needed.
- **Fault red** (#b3261e, dark #f2877e) on its own ground: errors, offline, destructive
  confirmation.
- **Idle grey** (#7b8781, dark #8a968f): off. At 3.7:1 on a light panel it is below 4.5:1
  for text. Known gap.

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

There is one breakpoint, 960px. Below it the columns stack, each table row becomes a
two-column block with its headings repeated in soft ink, tap targets grow to 44px, and the
row's button spans its cell.

### Named Rules
**The Still Table Rule.** Live values change in place. Column widths and row positions do
not react to them.

## Elevation & Depth

Flat today. There are no shadows. Depth is a 1px line around each panel and the step from
bench to panel. An open editor sinks back to the bench tone inside its table.

Focus is the one ring: 2px of teal, 2px clear of the control.

## Shapes

Gently rounded rectangles: panels 8px, buttons and fields 6px, the file picker's inner
button 4px. Edges are 1px. Circles appear only as state dots (9px). Tabs are text with a 2px
underline, not boxes.

## Components

### Buttons
Quiet today; agreed to become sturdier.
- **Shape:** 6px corners, 1px field-line edge, 7px by 14px, weight 550.
- **Primary:** teal with white ink (dark ink on light teal in dark mode). One per form.
- **Hover:** mask tint, or deeper teal on the primary. 150ms colour change only.
- **Danger:** red text and edge, used for the second press of a two-press confirm. The
  button renames itself ("Restart" to "Confirm restart") for four seconds.
- **Disabled:** no fill, soft ink, a message beside it saying what to do.

### Cards / Containers
- **Corner Style:** 8px.
- **Background:** panel on bench. Never nested.
- **Border:** 1px line. No shadow.
- **Internal Padding:** 18px by 20px for forms; tables run edge to edge.

### Inputs / Fields
- **Style:** label above in soft ink, 1px field-line edge, 6px corners, at least 38px tall.
- **Focus:** the 2px teal ring; teal caret.
- **Disabled:** 60% opacity.
- **Checkbox:** native, 17px, teal, with a title and an optional second line stating the
  consequence.

### Navigation
Four text tabs under the device name. Soft ink; ink and a field-line underline on hover; ink
and a teal underline when current. Same on a phone, with taller targets.

### Reading table
The signature component. One row per port or part: a bold name, then readings. Numbers are
right-aligned with a second line in soft ink (the rate under a total). State is a dot and a
word. A row's editor opens beneath it, on the bench tone, inside the table.

### Reading list
Term in soft ink, value in ink, two columns. Used for settings in effect, firmware, memory.

### Notice
Full-width, 8px corners, amber or red ink and edge on its own ground, with its action on the
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
- **Don't** nest panels.
- **Don't** animate anything but colour on hover.
