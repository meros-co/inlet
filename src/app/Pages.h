// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

#include "InletdClient.h"
#include "Widgets.h"

#include <JuceHeader.h>

namespace inlet::app
{

/** What every page shares: a header (title, subtitle), the latest snapshot,
    and the message of the last action (shown in the page's action bar). */
class Page : public juce::Component
{
public:
    Page(InletdClient& c, juce::String title) : client(c), title(std::move(title)) {}
    virtual void update(const InletdClient::Snapshot& s) = 0;
    void setMessage(const juce::String& text, State s)
    {
        message = text;
        messageState = s;
        repaint();
    }
    void paint(juce::Graphics& g) override;

protected:
    /** The page minus its padding and header; `headerRight` is for an action. */
    juce::Rectangle<int> body() const;
    juce::Rectangle<int> headerRight(int width) const;
    virtual juce::String subtitle() const = 0;

    InletdClient& client;
    juce::String title;
    juce::String message;
    State messageState = State::Ok;
};

/** Status: the clock, the device, the programs using it; Start and Stop. */
class StatusPage : public Page
{
public:
    explicit StatusPage(InletdClient& c);
    ~StatusPage() override;
    void update(const InletdClient::Snapshot& s) override;
    void paint(juce::Graphics& g) override;
    void resized() override;

    std::function<void(int page)> onOpenPage;
    std::function<void()> onStart, onStop;

private:
    juce::String subtitle() const override;
    void paintOnline(juce::Graphics& g);
    void paintOffline(juce::Graphics& g);

    class SummaryCard;
    InletdClient::Snapshot snap;
    service::Status serviceStatus = service::Status::Unknown;
    juce::TextButton stopButton { "Stop Inlet" }, startButton { "Start Inlet" }, servicesButton { "Open Services" };
    juce::OwnedArray<SummaryCard> cards;
    juce::Rectangle<int> clockRect, programsRect, stripRect, emptyRect;
};

/** Receivers ("Net In k"): what each plays and how well; play a stream on one. */
class ReceiversPage : public Page, private juce::TableListBoxModel
{
public:
    explicit ReceiversPage(InletdClient& c);
    void update(const InletdClient::Snapshot& s) override;
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::String subtitle() const override;
    int getNumRows() override { return receivers.size(); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void selectedRowsChanged(int) override;
    void updateButtons();
    int selectedReceiver() const;

    juce::TableListBox table;
    juce::ComboBox streamChoice;
    juce::TextButton play { "Play on this receiver" }, stop { "Stop" }, paste { "Add an SDP..." };
    juce::Array<juce::var> receivers, streams;
    juce::Rectangle<int> bar;
};

/** Senders ("Net Out k"): which outputs each carries, where, and whether it is on. */
class SendersPage : public Page, private juce::TableListBoxModel
{
public:
    explicit SendersPage(InletdClient& c);
    void update(const InletdClient::Snapshot& s) override;
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::String subtitle() const override;
    int getNumRows() override { return senders.size(); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void selectedRowsChanged(int) override;

    juce::TableListBox table;
    juce::TextButton toggle { "Turn off" };
    juce::Array<juce::var> senders;
    juce::Rectangle<int> bar;
};

/** The network, the clock and the shape of the sound card. Applying restarts
    inletd's engine (a DAW using Inlet is asked to reset). */
class SettingsPage : public Page
{
public:
    explicit SettingsPage(InletdClient& c);
    void update(const InletdClient::Snapshot& s) override;
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::String subtitle() const override;
    struct Field
    {
        juce::String key, label, hint;
        juce::TextEditor editor;
        juce::Rectangle<int> labelArea;
    };
    Field* addField(const juce::String& key, const juce::String& label, const juce::String& hint, bool numeric);
    Field* field(const juce::String& key);
    void markChanged(juce::Component& c);
    void setEditing(bool e);
    void apply();
    juce::String deviceNote() const;

    juce::ComboBox interfaceChoice;
    juce::Rectangle<int> interfaceLabel, rateLabel, networkLegend, clockLegend, deviceLegend, noteRect, footer;
    SegmentedControl rate;
    juce::OwnedArray<Field> fields;
    juce::TextButton applyButton { "Apply" }, revertButton { "Revert" };
    juce::var shown;
    bool editing = false;
};

/** The app, its licences, the ASIO trademark. */
class AboutPage : public Page
{
public:
    explicit AboutPage(InletdClient& c);
    void update(const InletdClient::Snapshot&) override {}
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::String subtitle() const override { return {}; }
    juce::Image asioLogo;
    juce::HyperlinkButton source, licences;
    juce::Rectangle<int> hero, textRect, logoTile;
};

} // namespace inlet::app
