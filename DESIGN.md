---
name: Order Book
description: An exchange order book and matching engine on real Nasdaq data, presented as a playable working paper in market microstructure.
colors:
  paper: "#ffffff"
  shade: "#f4f4f1"
  ink: "#111111"
  ink-2: "#333333"
  muted: "#5c5c5c"
  rule: "#111111"
  rule-soft: "#e1e1dc"
  pencil: "#a8231a"
  series-none: "#7a7a7a"
  series-imbalance: "#8f6414"
  series-next-move: "#111111"
  series-latency: "#3d5f94"
  series-toxicity: "#6e4f8f"
  bid: "#2f5d9a"
  ask: "#a8402a"
  better: "#1f6b3a"
typography:
  display:
    fontFamily: "Source Serif 4, Iowan Old Style, Charter, Georgia, serif"
    fontSize: "clamp(30px, 3.7vw, 44px)"
    fontWeight: 600
    lineHeight: 1.12
  headline:
    fontFamily: "Source Serif 4, Iowan Old Style, Charter, Georgia, serif"
    fontSize: "23px"
    fontWeight: 600
  prose:
    fontFamily: "Source Serif 4, Iowan Old Style, Charter, Georgia, serif"
    fontSize: "16-17px"
    lineHeight: 1.6
  ui:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "13.5-15px"
  figures:
    fontFamily: "Archivo Figures, Archivo, sans-serif"
    fontWeight: 600
    fontFeature: "tabular-nums"
rounded:
  sm: "2px"
spacing:
  gap: "20px"
  section: "52px"
  page-x: "clamp(16px, 3vw, 40px)"
---

# Design System: Order Book

## North Star: "The Working Paper"

The site reads like a market-microstructure paper you can operate. A visitor, usually an
engineer or researcher deciding whether the author knows what they are doing, should get the
claim and its evidence from the first screen: a title, an abstract that states the result with
its baseline, four headline numbers, then Figure 1, which they can change.

## Rules

- **Paper and ink.** White page, black type, black rules. No cards, no shadows, no fills behind
  sections; whitespace and rules do the separating.
- **Every figure is numbered and captioned.** The caption states what is drawn and what it shows.
  Figure 1's caption is rewritten live for the current settings.
- **Booktabs tables.** A heavy rule above and below, a light rule under the header, nothing between rows.
- **Serif for argument, sans for instruments.** Prose, titles and captions are serif; controls,
  table cells, axis labels and figures are sans with tabular numerals.
- **One red pencil.** The accent (#a8231a) marks annotations only: the Fed statement, the selected
  setting's ring in Figure 2, and the note on the earlier result. Never decoration.
- **Muted series, told apart twice.** Each filter has a colour and also a line weight or dash
  (no filter is dashed grey; next move is the heaviest black line), and every line is labelled
  directly at its end rather than through a legend.
- **Honesty is part of the design.** Picks come from the tuning hour; results that did not hold
  up are reported next to the ones that did.

## Files

- `docs/site.css`: tokens and shared components for both pages.
- `docs/playground.html`: the paper. `docs/index.html` is generated from `cpp/itch/viewer_template.html`.
- Archivo is self-hosted in `docs/fonts/`; Source Serif 4 loads from Google Fonts with Charter/Georgia fallbacks.
