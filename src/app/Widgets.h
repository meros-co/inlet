// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

// The small parts every page is made of: a state as a dot and a word, tags,
// cards, the sample-rate segmented control and the sidebar.

#include "InletdClient.h"

#include <JuceHeader.h>

namespace inlet::app
{

/** A literal with non-ASCII characters (" · ", "µs"), as UTF-8. */
inline juce::String u8(const char* s) { return juce::String(juce::CharPointer_UTF8(s)); }
/** "48 kHz", "44.1 kHz". */
juce::String kHz(double rate);

/** The programs using Inlet, from a status's `clients`: an ASIO host once it
    has loaded the driver; the Core Audio plug-in, which is attached all the
    time, only while something plays through it. */
juce::Array<juce::var> programsIn(const juce::var& status);

enum class State { Ok, Warn, Bad, Idle };

juce::Colour stateColour(State s);
/** A 7 px dot (a hollow ring when idle) and a word, in the state's colour. */
void drawStateLabel(juce::Graphics& g, juce::Rectangle<int> r, State s, const juce::String& word, float fontSize = 13.0f,
                    juce::Justification just = juce::Justification::centredLeft);
float stateLabelWidth(const juce::String& word, float fontSize = 13.0f);

enum class TagStyle { Neutral, Accent, Outline };
void drawTag(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, TagStyle style);
int tagWidth(const juce::String& text);

/** A surface card: fill, 1 px ring, 8 px radius. */
void drawCard(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, juce::Colour ring);
/** UPPERCASE small text with tracking: card kickers, section legends. */
void drawKicker(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, juce::Colour c, float size = 10.0f,
                float tracking = 0.12f);
/** An icon and a message (success in ok, error in bad, warning in warn). */
void drawMessage(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, State s);
/** A strip: tinted fill, ring, icon and text (unreachable, SAP warnings). */
void drawStrip(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, State s);

/** The dashed upper-case form of a clock identity, as SDP and devices show it. */
juce::String dashedIdentity(const juce::String& id);

/** Four (or more) radio-grouped options in one rounded box. */
class SegmentedControl : public juce::Component
{
public:
    SegmentedControl(juce::StringArray labels, juce::Array<int> values);
    void setValue(int v, juce::NotificationType n = juce::dontSendNotification);
    int getValue() const { return value; }
    std::function<void()> onChange;
    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress& k) override;
    int idealWidth() const;

private:
    int segmentAt(int x) const;
    juce::Array<juce::Rectangle<int>> segments() const;
    juce::StringArray labels;
    juce::Array<int> values;
    int value = 0, hover = -1;
};

/** The left rail: the lockup, the five pages, and the clock card. */
class Sidebar : public juce::Component
{
public:
    Sidebar();
    ~Sidebar() override;
    std::function<void(int page)> onSelect;
    void setSelected(int page);
    void update(const InletdClient::Snapshot& s);
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class NavItem;
    juce::OwnedArray<NavItem> items;
    int selected = 0;
    State clockState = State::Idle;
    juce::String clockLabel { "Stopped" }, clockDetail { u8("The sound card is off") };
    juce::Rectangle<int> clockCard;
};

/** inletd as the system runs it: the Windows service as the Service Control
    Manager sees it, the macOS launchd daemon co.meros.inletd. */
namespace service
{
enum class Status { Running, Stopped, Starting, Stopping, NotInstalled, Unknown };
Status query();
/** Starts it. False with the reason (Windows: no permission, an Inlet
    installed before the app could start it). */
bool start(juce::String& error);
/** Where to look when it will not start: services.msc, or its log on macOS. */
void openServices();
/** That button's name. */
juce::String openServicesLabel();
} // namespace service

} // namespace inlet::app
