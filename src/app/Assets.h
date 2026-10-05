// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

// The app's bundled assets: Inter (SIL OFL 1.1), the Phosphor UI icons (MIT),
// the Inlet and Manifold-family marks, the app and tray icons.

#include <JuceHeader.h>

namespace inlet::app::assets
{

/** Inter at a CSS-like pixel size: Regular, or Medium (headings, buttons;
    never heavier). `tracking` is in em, as the design gives it. */
juce::Font font(float size, bool medium = false, float tracking = 0.0f);
/** A monospace face for SDP and clock identities. */
juce::Font mono(float size);

juce::Typeface::Ptr regularTypeface();
juce::Typeface::Ptr mediumTypeface();

/** A Phosphor icon by name ("power", "play" ...) in a colour; null if missing. */
std::unique_ptr<juce::Drawable> icon(const juce::String& name, juce::Colour colour);

/** The Inlet mark, stroked on its 24-unit grid. */
std::unique_ptr<juce::Drawable> inletMark(juce::Colour colour, float strokeWidth = 1.75f);
/** The family marks for About: "manifold", "eddy", "basin", "sluice", "inlet". */
std::unique_ptr<juce::Drawable> familyMark(const juce::String& name, juce::Colour colour);

/** The app icon at about `size` px (from the PNGs). */
juce::Image appIcon(int size);
/** The tray icon: state "locked" | "free" | "away", on a light or dark taskbar. */
juce::Image trayIcon(const juce::String& state, bool lightTaskbar);

/** Bytes of a bundled file by its original name ("ASIO-...png"). */
bool data(const juce::String& originalName, const void*& bytes, int& size);

} // namespace inlet::app::assets
