// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "Assets.h"

#include "BinaryData.h"

namespace inlet::app::assets
{

namespace
{

// The Inlet mark, on a 24-unit grid with a round stroke.
const char* const kInletMark =
    R"(<circle cx="14.5" cy="12" r="6.5"/><path d="M3 9.75 H14.5"/><path d="M3 14.25 H14.5"/>)";

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
    return strokedMark(kInletMark, colour, strokeWidth);
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
