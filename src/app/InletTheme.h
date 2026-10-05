// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

// Inlet's theme: the Nocturne design system's tokens as JUCE values, plus the
// app's status roles and layout metrics. Every page and the LookAndFeel take
// their colours, sizes and metrics from here: no colour literals elsewhere.

#include <JuceHeader.h>

namespace inlet::app::theme
{

// ---- colour ----------------------------------------------------------------------------------

namespace colour
{
// grounds
inline const juce::Colour bg        { 0xff161826 };  // color-bg: window ground
inline const juce::Colour rail      { 0xff1c1e2c };  // sidebar ground (bg lifted 1/3 toward surface)
inline const juce::Colour surface   { 0xff232532 };  // color-surface: inputs, cards, action bars, menus
inline const juce::Colour section   { 0xff262a60 };  // color-section: About hero gradient start only

// text
inline const juce::Colour text      { 0xffe9e9ed };  // color-text
inline const juce::Colour text70    { 0xb3e9e9ed };  // field labels
inline const juce::Colour text60    { 0x99e9e9ed };  // table header labels

// neutral ramp
inline const juce::Colour n100 { 0xfff3f5fe }, n200 { 0xffe4e7f5 }, n300 { 0xffcfd3e5 },
                          n400 { 0xffb2b6ca }, n500 { 0xff9397ab }, n600 { 0xff75798c },
                          n700 { 0xff595d6c }, n800 { 0xff3f424d }, n900 { 0xff292b31 };

// accent ramp (mono scheme: one accent voice)
inline const juce::Colour accent    { 0xff9184d9 };  // color-accent
inline const juce::Colour a100 { 0xfff5f4ff }, a200 { 0xffe7e5fe }, a300 { 0xffd2cefd },
                          a400 { 0xffb5abfc }, a500 { 0xff968ae0 }, a600 { 0xff796cbf },
                          a700 { 0xff5d5294 }, a800 { 0xff423a6a }, a900 { 0xff2b2741 };

// lines (opaque equivalents of color-text at 16% / 8% / 7% over the grounds)
inline const juce::Colour divider       { 0xff383946 };  // text @16% on bg
inline const juce::Colour dividerOnSurf { 0xff434450 };  // text @16% on surface
inline const juce::Colour rowRule       { 0xff272936 };  // text @8% on bg
inline const juce::Colour railEdge      { 0xff2a2c39 };  // text @7% on rail: sidebar right edge
inline const juce::Colour edgeSm        { 0xff3f424d };  // shadow-sm: 1 px ring around cards/bars
inline const juce::Colour edgeMd        { 0xff595d6c };  // shadow-md ring: menus, app icon
inline const juce::Colour edgeLg        { 0xff9397ab };  // shadow-lg ring: dialogs

// interaction tints (alpha over whatever is beneath)
inline const juce::Colour hoverNeutral  { 0x12e9e9ed };  // text @7%: secondary button / nav hover
inline const juce::Colour pressNeutral  { 0x24e9e9ed };  // text @14%
inline const juce::Colour hoverAccent   { 0x1f9184d9 };  // accent @12%: primary button hover
inline const juce::Colour pressAccent   { 0x389184d9 };  // accent @22%
inline const juce::Colour selection     { 0x4d9184d9 };  // accent @30%: text selection highlight
inline const juce::Colour rowSelected   { 0xff2b2741 };  // a900: selected table row, active nav item

// status roles (replace kOk / kWarn / kBad and Colours::grey)
inline const juce::Colour ok   { 0xff86d0aa };
inline const juce::Colour warn { 0xffe8bd78 };
inline const juce::Colour bad  { 0xffef8f88 };
inline const juce::Colour idle { 0xff75798c };

// tray dot on a LIGHT taskbar (the dark-taskbar dot uses ok/warn/idle)
inline const juce::Colour okOnLight   { 0xff2f8a5e };
inline const juce::Colour warnOnLight { 0xffa8701c };
} // namespace colour

// ---- type ------------------------------------------------------------------------------------
// Inter Regular (400) and Medium (500), bundled from design/fonts as BinaryData.
// Headings and button labels use Medium; never heavier.

namespace type
{
constexpr float pageTitle   = 20.0f;  // Medium, tracking -0.01em
constexpr float heroTitle   = 22.0f;  // Medium: clock state on Status
constexpr float statValue   = 20.0f;  // Medium: summary card numbers
constexpr float body        = 14.0f;  // controls, nav, inputs, buttons
constexpr float table       = 13.5f;
constexpr float secondary   = 13.0f;  // page subtitles, descriptions
constexpr float label       = 12.0f;  // field labels (text70)
constexpr float kicker      = 11.0f;  // UPPERCASE, +0.10em tracking (table heads, section legends)
constexpr float micro       = 10.0f;  // UPPERCASE, +0.12em (card kickers, "CLOCK")
constexpr float wordmark    = 14.0f;  // "INLET" in the sidebar: Medium, UPPERCASE, +0.22em
} // namespace type

// ---- metrics ---------------------------------------------------------------------------------

namespace metric
{
constexpr int windowW = 960, windowH = 640;     // default size (min stays 720 x 460)
constexpr int sidebarW = 216;
constexpr int pagePadX = 30, pagePadTop = 26, pagePadBottom = 20;
constexpr int sectionGap = 18;                  // between blocks on a page
constexpr int controlH = 36;                    // buttons, inputs, combo boxes
constexpr int navItemH = 38;
constexpr int tableRowH = 32, tableHeaderH = 28;
constexpr float radiusSm = 4.0f, radiusMd = 8.0f, radiusLg = 14.0f, radiusTag = 6.0f;
constexpr float hairline = 1.0f;
constexpr float focusRing = 2.0f, focusOffset = 2.0f;
constexpr float disabledAlpha = 0.45f;
constexpr float markStroke = 1.75f;             // Inlet mark, on its 24-unit grid
} // namespace metric

} // namespace inlet::app::theme
