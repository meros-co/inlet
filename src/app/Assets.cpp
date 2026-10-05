// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "Assets.h"

#include "BinaryData.h"

namespace inlet::app::assets
{

namespace
{

// The family marks, on the 24-unit grid with a round 1.75 stroke (the Manifold
// ecosystem's identity, Meros Inc.'s own).
const char* markSvg(const juce::String& name)
{
    if (name == "inlet")
        return R"(<circle cx="14.5" cy="12" r="6.5"/><path d="M3 9.75 H14.5"/><path d="M3 14.25 H14.5"/>)";
    if (name == "manifold")
        return R"(<path d="M3 12 H9"/><path d="M9 12 C13.5 12 13 5 17.5 5 H21"/><path d="M9 12 H21"/><path d="M9 12 C13.5 12 13 19 17.5 19 H21"/><circle cx="9" cy="12" r="1.7" fill="#000000" stroke="none"/>)";
    if (name == "eddy")
        return R"(<path d="M12 3 A9 9 0 1 1 3 12"/><path d="M12 16.5 A4.5 4.5 0 1 1 16.5 12"/>)";
    if (name == "basin")
        return R"(<path d="M4 11 V17.5 A2.5 2.5 0 0 0 6.5 20 H17.5 A2.5 2.5 0 0 0 20 17.5 V11"/><path d="M8 3 V14"/><path d="M12 3 V17"/><path d="M16 3 V11.5"/>)";
    if (name == "sluice")
        return R"(<path d="M10 3.5 H17.5"/><path d="M13.75 3.5 V10"/><path d="M3 7 H11.5"/><path d="M3 13 H21"/><path d="M3 18 H21"/>)";
    return nullptr;
}

std::unique_ptr<juce::Drawable> strokedMark(const char* body, juce::Colour colour, float stroke)
{
    if (body == nullptr)
        return nullptr;
    const juce::String svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#000000\" "
                             "stroke-width=\"" + juce::String(stroke) + "\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
                           + juce::String(body) + "</svg>";
    auto xml = juce::parseXML(svg);
    if (xml == nullptr)
        return nullptr;
    auto d = juce::Drawable::createFromSVG(*xml);
    if (d != nullptr)
        d->replaceColour(juce::Colours::black, colour);
    return d;
}

juce::Image png(const juce::String& name)
{
    const void* bytes = nullptr;
    int size = 0;
    if (!data(name, bytes, size))
        return {};
    return juce::ImageCache::getFromMemory(bytes, size);
}

} // namespace

bool data(const juce::String& originalName, const void*& bytes, int& size)
{
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        if (originalName == BinaryData::originalFilenames[i])
        {
            bytes = BinaryData::getNamedResource(BinaryData::namedResourceList[i], size);
            return bytes != nullptr;
        }
    return false;
}

juce::Typeface::Ptr regularTypeface()
{
    static juce::Typeface::Ptr t = juce::Typeface::createSystemTypefaceFor(BinaryData::InterRegular_ttf,
                                                                           (size_t) BinaryData::InterRegular_ttfSize);
    return t;
}

juce::Typeface::Ptr mediumTypeface()
{
    static juce::Typeface::Ptr t = juce::Typeface::createSystemTypefaceFor(BinaryData::InterMedium_ttf,
                                                                           (size_t) BinaryData::InterMedium_ttfSize);
    return t;
}

juce::Font font(float size, bool medium, float tracking)
{
    auto options = juce::FontOptions(medium ? mediumTypeface() : regularTypeface()).withPointHeight(size);
    if (tracking != 0.0f)
        options = options.withKerningFactor(tracking);
    return juce::Font(options);
}

juce::Font mono(float size)
{
#if JUCE_WINDOWS
    return juce::Font(juce::FontOptions("Cascadia Mono", size * 1.2f, juce::Font::plain).withFallbacks({ "Consolas" }));
#else
    return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), size * 1.2f, juce::Font::plain));
#endif
}

std::unique_ptr<juce::Drawable> icon(const juce::String& name, juce::Colour colour)
{
    const void* bytes = nullptr;
    int size = 0;
    if (!data(name + ".svg", bytes, size))
        return nullptr;
    // Phosphor fills with currentColor; recoloured after parsing.
    const auto svg = juce::String::fromUTF8(static_cast<const char*>(bytes), size).replace("currentColor", "#000000");
    auto xml = juce::parseXML(svg);
    if (xml == nullptr)
        return nullptr;
    auto d = juce::Drawable::createFromSVG(*xml);
    if (d != nullptr)
        d->replaceColour(juce::Colours::black, colour);
    return d;
}

std::unique_ptr<juce::Drawable> inletMark(juce::Colour colour, float strokeWidth)
{
    return strokedMark(markSvg("inlet"), colour, strokeWidth);
}

std::unique_ptr<juce::Drawable> familyMark(const juce::String& name, juce::Colour colour)
{
    return strokedMark(markSvg(name), colour, 1.75f);
}

juce::Image appIcon(int size)
{
    for (int s : { 32, 64, 128, 256 })
        if (s >= size)
            return png("app-icon-" + juce::String(s) + ".png");
    return png("app-icon-256.png");
}

juce::Image trayIcon(const juce::String& state, bool lightTaskbar)
{
    return png("tray-" + state + "-on-" + (lightTaskbar ? "light" : "dark") + "-32.png");
}

} // namespace inlet::app::assets
