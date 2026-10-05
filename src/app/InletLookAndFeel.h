// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

// The Nocturne look for every JUCE control the app uses: Inter, quiet dark
// grounds, outlined buttons (primary never filled), 8 px radii, themed menus,
// dialogs, tooltips and scrollbars. A TextButton picks its variant from its
// "variant" property ("primary" | "secondary" | "ghost") and an icon from its
// "icon" property (a Phosphor name).

#include <JuceHeader.h>

namespace inlet::app
{

class InletLookAndFeel : public juce::LookAndFeel_V4
{
public:
    InletLookAndFeel();

    static void setVariant(juce::Button& b, const juce::String& variant, const juce::String& iconName = {});

    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& f) override;

    // Buttons
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    // Text and choices
    void fillTextEditorBackground(juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawComboBox(juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getLabelFont(juce::Label&) override;

    // Tables and scrolling
    void drawTableHeaderBackground(juce::Graphics&, juce::TableHeaderComponent&) override;
    void drawTableHeaderColumn(juce::Graphics&, juce::TableHeaderComponent&, const juce::String& name, int id, int w, int h,
                               bool over, bool down, int flags) override;
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart,
                       int thumbSize, bool over, bool down) override;
    int getDefaultScrollbarWidth() override { return 8; }

    // Menus, dialogs, tooltips
    void drawPopupMenuBackground(juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area, bool separator, bool active, bool highlighted,
                           bool ticked, bool hasSubMenu, const juce::String& text, const juce::String& shortcut,
                           const juce::Drawable* icon, const juce::Colour* textColour) override;
    void getIdealPopupMenuItemSize(const juce::String& text, bool separator, int standardHeight, int& w, int& h) override;
    juce::Font getPopupMenuFont() override;
    int getPopupMenuBorderSize() override { return 5; }
    void drawAlertBox(juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>& textArea, juce::TextLayout&) override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;
    void drawTooltip(juce::Graphics&, const juce::String& text, int w, int h) override;
    juce::Rectangle<int> getTooltipBounds(const juce::String& text, juce::Point<int> pos, juce::Rectangle<int> parent) override;

    /** The keyboard-focus ring: 2 px accent, 2 px outside `r`. */
    static void drawFocusRing(juce::Graphics& g, juce::Rectangle<float> r, float radius);
};

} // namespace inlet::app
