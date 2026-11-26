---
version: 1
slug: "web-index-html"
primary_target: "web/index.html"
related_targets: []
---

# Gateway web UI

Scope: the whole single-page UI (`web/index.html`). Visitor mode: Operate.
Audience and job: one technical owner now, other N540 owners later. See that the gateway is healthy; set up a port; diagnose a bus; update firmware.
Constraints from the owner: health first; everything on the page today stays available; light and dark; must not be harder to find things or too plain again. Terse, facts only. No internet assets. Used mostly on desktop, phone equally good. Never like the vendor UI or a marketing dashboard.

## Direction contract

THESIS: Every part of the gateway is a station that reports its own condition; pick one and read it as one plain sentence. Refuses the port table with everything in it.

OWN-WORLD: marine area forecasts. Deep sea-blue shell (#0d4a68), pale sea ground (#dcebf1), one white sheet with a single rounded corner, system sans in heavy headings, sea-blue traffic series, state marks by shape (disc, ring, square, triangle).

STORY: the list answers "is it healthy" before anything is clicked; the report says what is happening on a port and what went wrong; settings are one press from the report; every other page is reachable from the list.

FIRST VIEWPORT: header bar with name, address, sign-in. Left 400px: verdict heading, one line, then Ports (1 to 4) and Config (MQTT, Network, Firmware, Hardware, Events), each station with a mark and a reading, in a fixed order; a fault is filled and named in the verdict. Right: the sheet, port name at 40px with Edit port at its right, a 24px sentence, a ten-minute traffic chart, eight readings.

FORM: Station Reports, grounded candidate 4 of 7, chosen by the owner from a requested safer hand. seed:2d38dd5062d28640

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance

Signature interaction: choosing a station swaps the sheet in place and keeps the list still; the report sentence and chart update live without moving anything.
Motion: 150ms colour only.

Unresolved: product name. Traffic history is kept by the gateway (ten minutes, lost on restart).
