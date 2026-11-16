---
name: Order Book
description: An exchange order book and matching engine, shown on real Nasdaq data, in the identity system of the Chicago trading pits.
colors:
  card-stock: "#e8e9e4"
  card: "#f6f6f2"
  ink: "#15171a"
  ink-2: "#3f444a"
  muted: "#575d64"
  rule: "#c6c8c1"
  rule-soft: "#d8d9d3"
  kelly-jacket: "#17803d"
  royal-jacket: "#1d4dbf"
  goldenrod-jacket: "#e3a50f"
  vermilion-jacket: "#c8361a"
  graphite-jacket: "#3b4046"
  better: "#146b33"
  worse: "#b23415"
typography:
  display:
    fontFamily: "Archivo Badge, Archivo, Arial Narrow, sans-serif"
    fontSize: "clamp(40px, 5.2vw, 64px)"
    fontWeight: 900
    lineHeight: 0.92
    letterSpacing: "0.005em"
  headline:
    fontFamily: "Archivo Badge, Archivo, Arial Narrow, sans-serif"
    fontSize: "26px"
    fontWeight: 800
    lineHeight: 1
    letterSpacing: "0.01em"
  title:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "14.5px"
    fontWeight: 600
    lineHeight: 1.5
  body:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "15px"
    fontWeight: 400
    lineHeight: 1.5
    fontFeature: "tnum"
  figures:
    fontFamily: "Archivo Figures, Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "clamp(22px, 2.3vw, 30px)"
    fontWeight: 600
    lineHeight: 1.1
    letterSpacing: "-0.01em"
  label:
    fontFamily: "Archivo, Helvetica Neue, Arial, sans-serif"
    fontSize: "12.5px"
    fontWeight: 500
    lineHeight: 1.3
  badge:
    fontFamily: "Archivo Badge, Archivo, Arial Narrow, sans-serif"
    fontSize: "13px"
    fontWeight: 900
    lineHeight: 1
    letterSpacing: "0.06em"
rounded:
  badge: "3px"
  sm: "4px"
spacing:
  gap: "16px"
  section: "44px"
  page-x: "clamp(16px, 3vw, 36px)"
components:
  button-primary:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.card}"
    rounded: "{rounded.sm}"
    padding: "7px 14px"
    height: "36px"
  button-quiet:
    backgroundColor: "{colors.card}"
    textColor: "{colors.ink}"
    rounded: "{rounded.sm}"
    padding: "7px 14px"
    height: "36px"
  segment-selected:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.card}"
    height: "34px"
  panel:
    backgroundColor: "{colors.card}"
    rounded: "{rounded.sm}"
    padding: "16px 18px"
  badge-imb:
    backgroundColor: "{colors.goldenrod-jacket}"
    textColor: "{colors.ink}"
    typography: "{typography.badge}"
    rounded: "{rounded.badge}"
  badge-nxt:
    backgroundColor: "{colors.kelly-jacket}"
    textColor: "#ffffff"
    typography: "{typography.badge}"
    rounded: "{rounded.badge}"
  badge-lat:
    backgroundColor: "{colors.royal-jacket}"
    textColor: "#ffffff"
    typography: "{typography.badge}"
    rounded: "{rounded.badge}"
  badge-tox:
    backgroundColor: "{colors.vermilion-jacket}"
    textColor: "#ffffff"
    typography: "{typography.badge}"
    rounded: "{rounded.badge}"
  badge-off:
    backgroundColor: "{colors.graphite-jacket}"
    textColor: "#ffffff"
    typography: "{typography.badge}"
    rounded: "{rounded.badge}"
---

# Design System: Order Book

## Overview

**Creative North Star: "The Pit, Rebuilt"**

Electronic matching engines replaced the Chicago open-outcry pits. This site shows one of those engines, and it wears the pit's identity system: traders in solid firm-coloured jackets, each carrying a badge with a short block-lettered acronym, on a floor of paper trading cards. The world lends four things and only four: the card-stock ground, the jacket colours, the heavy condensed badge lettering, and the badge itself as a recurring label. Layout, navigation and controls stay those of a clean working web tool.

Density is a working screen's density: tables with tabular figures, charts with direct labels, controls a visitor already knows how to use. Colour does jobs. Each strategy owns one jacket colour everywhere it appears, so a visitor learns the cast once and reads every chart and table after that by colour and acronym.

The category default this system refuses is the dark trading terminal of green and red on charcoal. The ground is light because the work is read in daylight, from a resume link on a laptop or a phone.

**Key Characteristics:**
- Card-stock grey ground with ink-black rules and type; panels a shade lighter than the ground.
- Four saturated jacket colours plus graphite, each bound to one strategy for the life of the site.
- Heavy extra-condensed capitals for badges and headings; a plain grotesque for everything else.
- Pit badges as the signature: the same chip in controls, legends, table headers, table rows and chart labels.
- Flat surfaces with hairline borders; no shadows.

## Colors

A full palette of four jacket colours on a neutral card-stock field, with ink for everything structural.

### Primary
- **Kelly Jacket** (#17803d): the next-move model (NXT). Badge fills, its chart lines, its importance bars.
- **Royal Jacket** (#1d4dbf): the latency-trained model (LAT), and the bid side of the book (prices, depth bars).
- **Goldenrod Jacket** (#e3a50f): the imbalance baseline (IMB), the active tab underline, auction crosses, text selection, and the focus ring on a selected segment. Always carries ink text, never white.
- **Vermilion Jacket** (#c8361a): the fill-toxicity model (TOX), and the ask side of the book.
- **Graphite Jacket** (#3b4046): no filter (OFF) and other neutral series.

### Neutral
- **Card Stock** (#e8e9e4): the page ground.
- **Card** (#f6f6f2): panels, the board, inputs and unselected segments.
- **Ink** (#15171a): type, rules under table headers and chart axes, the top bar, primary buttons and selected segments.
- **Ink Two** (#3f444a): secondary text and ledes.
- **Muted** (#575d64): labels, axis text, notes. 5.4:1 on the ground.
- **Rule** (#c6c8c1) and **Soft Rule** (#d8d9d3): panel borders, row dividers, chart grid lines.

### States
- **Better** (#146b33) and **Worse** (#b23415): deltas against the no-filter baseline, always alongside a sign.

### Named Rules
**The One Jacket Rule.** A strategy's colour never changes and is never borrowed. Kelly is NXT, royal is LAT, goldenrod is IMB, vermilion is TOX, graphite is OFF, on every page and in every chart.

**The Sides Rule.** On the order book, bids are royal and asks are vermilion. Green-versus-red for buy and sell is the terminal default this world refuses.

## Typography

**Display Font:** Archivo Extra Condensed, ExtraBold and Black (with Arial Narrow)
**Body Font:** Archivo, Regular to SemiBold (with Helvetica Neue, Arial)
**Figures Font:** Archivo Semi Condensed SemiBold, for large numbers

**Character:** One family at three widths. The extra-condensed black is the badge lettering on a trader's jacket; the regular width is a quiet working grotesque; the semi-condensed figures keep big numbers compact without losing legibility. All six files are self-hosted under `docs/fonts/` (SIL OFL).

### Hierarchy
- **Display** (900, clamp(40px, 5.2vw, 64px), 0.92, uppercase): page titles only.
- **Headline** (800, 26px, 1, uppercase): section and panel headings.
- **Title** (600, 14.5px): panel sub-headings and filter names.
- **Body** (400, 15px, 1.5): running text, held to 68–75ch.
- **Figures** (600, clamp(22px, 2.3vw, 30px)): the result strip and the replay board.
- **Label** (500, 12.5px): field labels, table headers, axis text.
- **Badge** (900, 13px, 0.06em tracking, uppercase): pit badges; 19px in the filter list, 11.5px in tables and legends.

### Named Rules
**The Tabular Rule.** Every number on the site uses tabular figures (`font-variant-numeric: tabular-nums` on the body), so columns align and live numbers don't jitter.

**The Caps Belong To Badges Rule.** Uppercase is for display titles, headings and badges. Labels, buttons and body text stay in sentence case.

## Layout

A 1440px maximum page with clamp(16px, 3vw, 36px) side padding and a 16px gap between panels. Sections are separated by 44px, with the heading closer to its content than to the section above.

The playground is a desk of three columns: a 286px settings rail, a flexible results column (result strip, the afternoon chart, the edge chart), and a 340px scorecard. Below 1240px the scorecard moves under the desk; below 860px everything stacks, results first and settings second, so the outcome is the first thing a phone shows. The replay is a five-cell board, a control row, a full-width day chart, then ladder, tape and notes in three columns, collapsing to two and then one.

## Elevation & Depth

Flat. Depth comes from tone: a card panel a shade lighter than the card-stock ground, edged by a 1px rule. The only overlays are the sticky top bar (ink) and the chart tooltip (ink). There are no shadows.

### Named Rules
**The Flat Floor Rule.** Surfaces never lift. A state change shows as an ink border, an ink fill or a goldenrod mark, never as a shadow.

## Shapes

Small, square-shouldered corners: 3px on badges, 4px on panels, buttons, inputs and segments. Segmented controls and selects use a full 1px ink border. Charts use a 1px ink baseline and soft grid lines, with no frame.

## Components

### Pit badge (signature)
A solid jacket-coloured rectangle (3px corners) holding a 3–5 letter acronym in extra-condensed black capitals: OFF, IMB, NXT, LAT, TOX for strategies; LIT, HID, CROSS for trade types; OPEN, FED, LOW, HIGH, CLOSE for replay moments. White text on kelly, royal, vermilion and graphite; ink text on goldenrod. The paper variant (card fill, 1.5px ink inset) marks the site name and hidden trades. Charts draw the same badge at line ends instead of a legend.

### Buttons
- **Shape:** gently squared (4px).
- **Primary:** ink fill, card-white text, 600 weight, 36px tall (Play).
- **Quiet:** card fill, ink text and 1px ink border (Use the tuning-hour pick).
- **Hover / Focus:** primary lightens to #2b2e33; quiet darkens to #e2e3dc. Focus is a 2px royal outline at 2px offset.

### Segmented controls
- **Style:** a row of buttons inside one 1px ink border, divided by ink rules, on card fill.
- **State:** the selected segment is ink-filled with white text; hover is #e2e3dc; disabled text is #9a9e98. Keyboard focus is drawn inset (−4px offset), goldenrod on the selected segment.
- Built as a radio group with arrow-key movement.

### Filter list
Rows of large badge, name and one-line description. The selected row takes a card fill and a 1px ink border; hover takes #e2e3dc. On phones the descriptions drop and the list becomes two columns.

### Panels
Card fill, 1px rule border, 4px corners, 16px × 18px padding.

### Tables
Header row in label type with a 1px ink underline; body rows divided by soft rules; numbers right-aligned in tabular figures. A confidence interval sits in a smaller muted line under its estimate. The current row gets a #e4e5de fill.

### Navigation
An ink top bar, 56px tall and sticky. On the left, the paper "Order book" badge and the owner's name; then tabs in 500-weight text whose active tab carries a 4px goldenrod underline; owner links on the right. On phones the tabs move to their own row.

### Motion
One authored moment: when a setting changes, the selected strategy's afternoon line draws in from 1:30 over 520ms with a quartic ease-out. Otherwise the depth ladder flashes goldenrod behind a changed quantity during playback, and controls ease their background over 120ms. Reduced-motion turns all of it off.

## Do's and Don'ts

### Do:
- **Do** give every new strategy or series its own jacket colour and badge, and use that pair everywhere it appears.
- **Do** label chart lines directly with badges at their ends rather than with a separate legend.
- **Do** keep numbers in tabular figures and right-aligned in tables.
- **Do** put goldenrod badges on ink text only (8.3:1); white on goldenrod fails contrast.
- **Do** keep the ground card-stock grey (#e8e9e4); panels one step lighter (#f6f6f2).

### Don't:
- **Don't** use green and red for buy and sell; bids are royal, asks are vermilion.
- **Don't** add shadows, gradients or glass; the floor is flat.
- **Don't** set labels, buttons or body text in capitals; caps belong to display, headings and badges.
- **Don't** let the pit world supply layout or controls: no jacket textures, no trading-card frames, no hand-signal pictograms on the working screens.
