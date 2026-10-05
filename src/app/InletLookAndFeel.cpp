// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "InletLookAndFeel.h"

#include "Assets.h"
#include "InletTheme.h"

namespace inlet::app
{

using namespace theme;

namespace
{
juce::Colour dim(juce::Colour c, const juce::Component& comp)
{
    return comp.isEnabled() ? c : c.withMultipliedAlpha(metric::disabledAlpha);
}

// Buttons do not take focus on a click (setVariant), so a focused button got
// there by keyboard: that is when the ring shows.
bool focusVisible(const juce::Component& c)
{
    return c.hasKeyboardFocus(false);
}
} // namespace

InletLookAndFeel::InletLookAndFeel()
{
    auto scheme = juce::LookAndFeel_V4::ColourScheme(colour::bg, colour::bg, colour::surface, colour::divider, colour::text,
                                                     colour::surface, colour::text, colour::a900, colour::a300);
    setColourScheme(scheme);

    setColour(juce::ResizableWindow::backgroundColourId, colour::bg);
    setColour(juce::DocumentWindow::backgroundColourId, colour::bg);
    setColour(juce::Label::textColourId, colour::text);
    setColour(juce::TextEditor::backgroundColourId, colour::surface);
    setColour(juce::TextEditor::textColourId, colour::text);
    setColour(juce::TextEditor::outlineColourId, colour::divider);
    setColour(juce::TextEditor::focusedOutlineColourId, colour::accent);
    setColour(juce::TextEditor::highlightColourId, colour::selection);
    setColour(juce::TextEditor::highlightedTextColourId, colour::text);
    setColour(juce::CaretComponent::caretColourId, colour::accent);
    setColour(juce::ComboBox::backgroundColourId, colour::surface);
    setColour(juce::ComboBox::textColourId, colour::text);
    setColour(juce::ComboBox::outlineColourId, colour::divider);
    setColour(juce::ComboBox::arrowColourId, colour::n400);
    setColour(juce::ComboBox::focusedOutlineColourId, colour::accent);
    setColour(juce::PopupMenu::backgroundColourId, colour::surface);
    setColour(juce::PopupMenu::textColourId, colour::text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, colour::a900);
    setColour(juce::PopupMenu::highlightedTextColourId, colour::a200);
    setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    setColour(juce::TextButton::textColourOffId, colour::text);
    setColour(juce::TextButton::textColourOnId, colour::accent);
    setColour(juce::ListBox::backgroundColourId, colour::bg);
    setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TableHeaderComponent::backgroundColourId, colour::bg);
    setColour(juce::TableHeaderComponent::textColourId, colour::text60);
    setColour(juce::TableHeaderComponent::outlineColourId, colour::divider);
    setColour(juce::TableHeaderComponent::highlightColourId, colour::hoverNeutral);
    setColour(juce::ScrollBar::thumbColourId, colour::n700);
    setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    setColour(juce::AlertWindow::backgroundColourId, colour::surface);
    setColour(juce::AlertWindow::textColourId, colour::text.withAlpha(0.85f));
    setColour(juce::AlertWindow::outlineColourId, colour::edgeLg);
    setColour(juce::TooltipWindow::backgroundColourId, colour::surface);
    setColour(juce::TooltipWindow::textColourId, colour::text);
    setColour(juce::TooltipWindow::outlineColourId, colour::edgeSm);
    setColour(juce::ToggleButton::textColourId, colour::text);
    setColour(juce::ToggleButton::tickColourId, colour::accent);
}

void InletLookAndFeel::setVariant(juce::Button& b, const juce::String& variant, const juce::String& iconName)
{
    b.getProperties().set("variant", variant);
    if (iconName.isNotEmpty())
        b.getProperties().set("icon", iconName);
    b.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    b.setMouseClickGrabsKeyboardFocus(false);
    b.repaint();
}

juce::Typeface::Ptr InletLookAndFeel::getTypefaceForFont(const juce::Font& f)
{
    if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName() || f.getTypefaceName().isEmpty())
        return f.isBold() || f.getTypefaceStyle() == "Medium" ? assets::mediumTypeface() : assets::regularTypeface();
    return juce::LookAndFeel_V4::getTypefaceForFont(f);
}

void InletLookAndFeel::drawFocusRing(juce::Graphics& g, juce::Rectangle<float> r, float radius)
{
    g.setColour(colour::accent);
    g.drawRoundedRectangle(r.expanded(metric::focusOffset + metric::focusRing / 2), radius + metric::focusOffset,
                           metric::focusRing);
}

/* ---- buttons --------------------------------------------------------------------------------- */

void InletLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down)
{
    const auto variant = b.getProperties().getWithDefault("variant", "secondary").toString();
    auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    const float radius = metric::radiusMd;
    juce::Colour fill = juce::Colours::transparentBlack, outline = juce::Colours::transparentBlack;
    if (variant == "primary")
    {
        outline = colour::accent;
        if (down) fill = colour::pressAccent;
        else if (highlighted) fill = colour::hoverAccent;
    }
    else if (variant == "ghost")
    {
        if (down) fill = colour::accent.withAlpha(0.18f);
        else if (highlighted) fill = colour::accent.withAlpha(0.10f);
    }
    else if (variant == "nav")
        return;   // nav items paint themselves
    else
    {
        outline = colour::divider;
        if (down) fill = colour::pressNeutral;
        else if (highlighted) fill = colour::hoverNeutral;
    }
    if (b.getToggleState() && variant == "segment")
        outline = colour::accent;
    g.setColour(dim(fill, b));
    g.fillRoundedRectangle(r, radius);
    if (!outline.isTransparent())
    {
        g.setColour(dim(outline, b));
        g.drawRoundedRectangle(r, radius, metric::hairline);
    }
    if (focusVisible(b))
        drawFocusRing(g, r, radius);
}

juce::Font InletLookAndFeel::getTextButtonFont(juce::TextButton&, int)
{
    return assets::font(type::body, true);
}

void InletLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    const auto variant = b.getProperties().getWithDefault("variant", "secondary").toString();
    const juce::Colour textColour = (variant == "primary" || variant == "ghost") ? colour::accent : colour::text;
    const auto font = assets::font(type::body, true);
    const auto text = b.getButtonText();
    const auto iconName = b.getProperties().getWithDefault("icon", "").toString();
    std::unique_ptr<juce::Drawable> icon = iconName.isNotEmpty() ? assets::icon(iconName, dim(textColour, b)) : nullptr;
    const float iconSize = 13.0f, gap = icon != nullptr && text.isNotEmpty() ? 6.0f : 0.0f;
    const float textW = juce::GlyphArrangement::getStringWidth(font, text);
    const float total = (icon != nullptr ? iconSize : 0.0f) + gap + textW;
    auto r = b.getLocalBounds().toFloat();
    float x = r.getCentreX() - total / 2;
    if (icon != nullptr)
    {
        icon->drawWithin(g, { x, r.getCentreY() - iconSize / 2, iconSize, iconSize }, juce::RectanglePlacement::centred, 1.0f);
        x += iconSize + gap;
    }
    g.setFont(font);
    g.setColour(dim(textColour, b));
    g.drawText(text, juce::Rectangle<float>(x, r.getY(), textW + 2, r.getHeight()), juce::Justification::centredLeft, false);
}

void InletLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    juce::LookAndFeel_V4::drawToggleButton(g, b, highlighted, false);
}

/* ---- text and choices ------------------------------------------------------------------------ */

void InletLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour(dim(colour::surface, e));
    g.fillRoundedRectangle(juce::Rectangle<float>(0, 0, (float) w, (float) h).reduced(0.5f), metric::radiusMd);
}

void InletLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    auto r = juce::Rectangle<float>(0, 0, (float) w, (float) h).reduced(0.5f);
    juce::Colour c = colour::divider;
    if ((bool) e.getProperties().getWithDefault("changed", false))
        c = colour::a600;
    if (e.hasKeyboardFocus(true) && !e.isReadOnly())
        c = colour::accent;
    else if (e.isMouseOver(true))
        c = colour::text.withAlpha(0.45f);
    g.setColour(dim(c, e));
    g.drawRoundedRectangle(r, metric::radiusMd, metric::hairline);
}

void InletLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float>(0, 0, (float) w, (float) h).reduced(0.5f);
    g.setColour(dim(colour::surface, box));
    g.fillRoundedRectangle(r, metric::radiusMd);
    juce::Colour outline = colour::divider;
    if ((bool) box.getProperties().getWithDefault("changed", false))
        outline = colour::a600;
    if (box.hasKeyboardFocus(true))
        outline = colour::accent;
    else if (box.isMouseOver(true))
        outline = colour::text.withAlpha(0.45f);
    g.setColour(dim(outline, box));
    g.drawRoundedRectangle(r, metric::radiusMd, metric::hairline);
    // A small chevron.
    const float cx = (float) w - 18.0f, cy = (float) h / 2.0f;
    juce::Path p;
    p.startNewSubPath(cx - 4.5f, cy - 2.0f);
    p.lineTo(cx, cy + 2.5f);
    p.lineTo(cx + 4.5f, cy - 2.0f);
    g.setColour(dim(colour::n400, box));
    g.strokePath(p, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void InletLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(12, 1, box.getWidth() - 40, box.getHeight() - 2);
    label.setFont(getComboBoxFont(box));
}

juce::Font InletLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return assets::font(type::body);
}

juce::Font InletLookAndFeel::getLabelFont(juce::Label& l)
{
    return l.getProperties().contains("font") ? l.getFont() : assets::font(type::body);
}

/* ---- tables and scrolling ------------------------------------------------------------------------ */

void InletLookAndFeel::drawTableHeaderBackground(juce::Graphics& g, juce::TableHeaderComponent& h)
{
    g.fillAll(colour::bg);
    g.setColour(colour::divider);
    g.fillRect(0, h.getHeight() - 1, h.getWidth(), 1);
}

void InletLookAndFeel::drawTableHeaderColumn(juce::Graphics& g, juce::TableHeaderComponent& header, const juce::String& name,
                                             int id, int w, int h, bool over, bool, int)
{
    if (over)
    {
        g.setColour(colour::hoverNeutral);
        g.fillRect(0, 0, w, h - 1);
    }
    g.setColour(colour::text60);
    g.setFont(assets::font(type::kicker, true, 0.08f));
    // Number columns are right-aligned, as their cells are.
    const bool right = (bool) header.getProperties().getWithDefault("right" + juce::String(id), false);
    g.drawText(name.toUpperCase(), 6, 0, w - 12, h, right ? juce::Justification::centredRight : juce::Justification::centredLeft,
               true);
}

void InletLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                     int thumbStart, int thumbSize, bool over, bool)
{
    juce::Rectangle<float> thumb = vertical ? juce::Rectangle<float>((float) x + 2, (float) thumbStart, (float) w - 4, (float) thumbSize)
                                            : juce::Rectangle<float>((float) thumbStart, (float) y + 2, (float) thumbSize, (float) h - 4);
    g.setColour(over ? colour::n600 : colour::n700);
    g.fillRoundedRectangle(thumb, juce::jmin(thumb.getWidth(), thumb.getHeight()) / 2);
}

/* ---- menus, dialogs, tooltips -------------------------------------------------------------------- */

void InletLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int w, int h)
{
    g.fillAll(colour::bg);
    auto r = juce::Rectangle<float>(0, 0, (float) w, (float) h).reduced(0.5f);
    g.setColour(colour::surface);
    g.fillRoundedRectangle(r, metric::radiusMd);
    g.setColour(colour::edgeMd);
    g.drawRoundedRectangle(r, metric::radiusMd, metric::hairline);
}

juce::Font InletLookAndFeel::getPopupMenuFont()
{
    return assets::font(type::body);
}

void InletLookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool separator, int, int& w, int& h)
{
    if (separator)
    {
        w = 50;
        h = 11;
        return;
    }
    // "Title\nSecond line": a taller item with a muted second line.
    const auto lines = juce::StringArray::fromLines(text);
    float width = 0;
    for (int i = 0; i < lines.size(); ++i)
        width = juce::jmax(width, juce::GlyphArrangement::getStringWidth(assets::font(i == 0 ? type::body : type::label), lines[i]));
    w = (int) width + 40;
    h = lines.size() > 1 ? 54 : 34;
}

void InletLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool separator, bool active,
                                         bool highlighted, bool ticked, bool, const juce::String& text, const juce::String&,
                                         const juce::Drawable*, const juce::Colour*)
{
    if (separator)
    {
        g.setColour(colour::divider);
        g.fillRect(area.reduced(6, 0).withHeight(1).withCentre(area.getCentre()));
        return;
    }
    auto r = area.reduced(0, 1).toFloat();
    if (highlighted && active)
    {
        g.setColour(colour::a900);
        g.fillRoundedRectangle(r, 5.0f);
    }
    const auto lines = juce::StringArray::fromLines(text);
    auto textArea = area.reduced(14, 0);
    const juce::Colour main = !active ? colour::n600 : (highlighted ? colour::a200 : colour::text);
    if (lines.size() > 1)
    {
        g.setFont(assets::font(type::body));
        g.setColour(main);
        g.drawText(lines[0], textArea.removeFromTop(area.getHeight() / 2 + 2).withTrimmedTop(6), juce::Justification::bottomLeft, true);
        g.setFont(assets::font(type::label));
        g.setColour(colour::n500);
        g.drawText(lines[1], textArea.withTrimmedTop(2), juce::Justification::topLeft, true);
    }
    else
    {
        g.setFont(assets::font(type::body));
        g.setColour(main);
        g.drawText((ticked ? juce::String(juce::CharPointer_UTF8("\xe2\x9c\x93  ")) : juce::String()) + text, textArea,
                   juce::Justification::centredLeft, true);
    }
}

juce::Font InletLookAndFeel::getAlertWindowTitleFont()
{
    return assets::font(type::pageTitle, true);
}

juce::Font InletLookAndFeel::getAlertWindowMessageFont()
{
    return assets::font(type::body);
}

juce::Font InletLookAndFeel::getAlertWindowFont()
{
    return assets::font(type::body);
}

void InletLookAndFeel::drawAlertBox(juce::Graphics& g, juce::AlertWindow& w, const juce::Rectangle<int>& textArea, juce::TextLayout& layout)
{
    auto r = w.getLocalBounds().toFloat().reduced(0.5f);
    g.fillAll(colour::bg);
    g.setColour(colour::surface);
    g.fillRoundedRectangle(r, metric::radiusLg);
    g.setColour(colour::edgeLg);
    g.drawRoundedRectangle(r, metric::radiusLg, metric::hairline);
    layout.draw(g, textArea.toFloat().withX((float) textArea.getX()));
}

void InletLookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int w, int h)
{
    auto r = juce::Rectangle<float>(0, 0, (float) w, (float) h).reduced(0.5f);
    g.fillAll(colour::bg);
    g.setColour(colour::surface);
    g.fillRoundedRectangle(r, metric::radiusSm);
    g.setColour(colour::edgeSm);
    g.drawRoundedRectangle(r, metric::radiusSm, metric::hairline);
    g.setColour(colour::text);
    g.setFont(assets::font(12.5f));
    g.drawFittedText(text, juce::Rectangle<int>(0, 0, w, h).reduced(10, 4), juce::Justification::centredLeft, 4);
}

juce::Rectangle<int> InletLookAndFeel::getTooltipBounds(const juce::String& text, juce::Point<int> pos, juce::Rectangle<int> parent)
{
    const int w = (int) juce::GlyphArrangement::getStringWidth(assets::font(12.5f), text) + 24;
    const int h = 30;
    return juce::Rectangle<int>(pos.x > parent.getCentreX() ? pos.x - (w + 12) : pos.x + 12,
                                pos.y > parent.getCentreY() ? pos.y - (h + 6) : pos.y + 6, w, h)
        .constrainedWithin(parent);
}

} // namespace inlet::app
