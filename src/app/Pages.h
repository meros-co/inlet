// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

#include "InletdClient.h"

#include <JuceHeader.h>

namespace inlet::app
{

/** Every page shows the latest snapshot; some also act through the client. */
class Page : public juce::Component
{
public:
    explicit Page(InletdClient& c) : client(c) {}
    virtual void update(const InletdClient::Snapshot& s) = 0;

protected:
    InletdClient& client;
};

/** PTP, the device, the programs using it, SAP: what David checks first. */
class StatusPage : public Page
{
public:
    using Page::Page;
    void update(const InletdClient::Snapshot& s) override;
    void paint(juce::Graphics& g) override;

private:
    struct Row { juce::String key, value; bool heading = false; bool warn = false; };
    juce::Array<Row> rows;
};

/** The receivers ("Net In k"): what each plays, and how well; play a stream on one. */
class ReceiversPage : public Page, private juce::TableListBoxModel
{
public:
    explicit ReceiversPage(InletdClient& c);
    void update(const InletdClient::Snapshot& s) override;
    void resized() override;

private:
    int getNumRows() override { return receivers.size(); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void selectedRowsChanged(int) override { updateButtons(); }
    void updateButtons();
    int selectedReceiver() const;

    juce::TableListBox table;
    juce::ComboBox streamChoice;
    juce::TextButton play { "Play on this receiver" }, stop { "Stop" }, paste { "Add an SDP..." };
    juce::Array<juce::var> receivers, streams;
};

/** The senders ("Net Out k"): which outputs each carries, where, and whether it is on. */
class SendersPage : public Page, private juce::TableListBoxModel
{
public:
    explicit SendersPage(InletdClient& c);
    void update(const InletdClient::Snapshot& s) override;
    void resized() override;

private:
    int getNumRows() override { return senders.size(); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void selectedRowsChanged(int) override;

    juce::TableListBox table;
    juce::TextButton toggle { "Turn off" };
    juce::Array<juce::var> senders;
};

/** The network interface, PTP domain, rate and the device's shape. Applying
    restarts inletd's engine (a DAW using Inlet is asked to reset). */
class SettingsPage : public Page
{
public:
    explicit SettingsPage(InletdClient& c);
    void update(const InletdClient::Snapshot& s) override;
    void resized() override;

private:
    struct Field
    {
        juce::Label label;
        juce::TextEditor editor;
        juce::String key;
    };
    void addField(const juce::String& key, const juce::String& text, bool numeric);
    void apply();

    juce::Label interfaceLabel, rateLabel, note;
    juce::ComboBox interfaceChoice, rateChoice;
    juce::OwnedArray<Field> fields;
    juce::TextButton applyButton { "Apply" }, revertButton { "Revert" };
    juce::var shown;       // the settings the editors were last filled from
    bool editing = false;  // the user has changed something: polls do not overwrite it
};

/** Version, licences, the ASIO trademark (Steinberg's usage guidelines). */
class AboutPage : public juce::Component
{
public:
    AboutPage();
    void paint(juce::Graphics& g) override;

private:
    juce::Image asioLogo;
};

} // namespace inlet::app
