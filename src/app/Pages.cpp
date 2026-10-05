// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "Pages.h"

#include "Assets.h"
#include "InletLookAndFeel.h"
#include "InletTheme.h"

namespace inlet::app
{

using namespace theme;

namespace
{

juce::String str(const juce::var& v, const char* key, const juce::String& fallback = {})
{
    const auto p = v.getProperty(key, {});
    return p.isVoid() ? fallback : p.toString();
}

/** 1 204 332: thin-space thousands, as the design writes counts. */
juce::String count(const juce::var& v)
{
    juce::String digits((juce::int64) v);
    juce::String out;
    for (int i = 0; i < digits.length(); ++i)
    {
        if (i > 0 && (digits.length() - i) % 3 == 0)
            out << u8("\xe2\x80\x89");
        out << digits.substring(i, i + 1);
    }
    return out;
}

const juce::String dash = u8("\xe2\x80\x94");
const juce::String dot = u8(" \xc2\xb7 ");
const juce::String enDash = u8("\xe2\x80\x93");

void cellText(juce::Graphics& g, const juce::String& text, int w, int h, juce::Colour c, bool right = false,
              const juce::Font* font = nullptr)
{
    g.setColour(c);
    g.setFont(font != nullptr ? *font : assets::font(type::table));
    g.drawText(text, 6, 0, w - 12, h, right ? juce::Justification::centredRight : juce::Justification::centredLeft, true);
}

void tableLook(juce::TableListBox& t)
{
    t.setRowHeight(metric::tableRowH);
    t.setHeaderHeight(metric::tableHeaderH);
    t.setColour(juce::ListBox::backgroundColourId, colour::bg);
    t.setOutlineThickness(0);
}

void rowBackground(juce::Graphics& g, int w, int h, bool selected)
{
    if (selected)
        g.fillAll(colour::rowSelected);
    g.setColour(colour::rowRule);
    g.fillRect(0, h - 1, w, 1);
}

} // namespace

/* ---- the page frame ---------------------------------------------------------------------- */

void Page::paint(juce::Graphics& g)
{
    g.fillAll(colour::bg);
    auto r = getLocalBounds().reduced(metric::pagePadX, 0).withTrimmedTop(metric::pagePadTop);
    g.setColour(colour::text);
    g.setFont(assets::font(type::pageTitle, true, -0.01f));
    g.drawText(title, r.removeFromTop(26), juce::Justification::centredLeft, true);
    r.removeFromTop(4);
    g.setColour(colour::n500);
    g.setFont(assets::font(type::secondary));
    g.drawText(subtitle(), r.removeFromTop(18).withTrimmedRight(180), juce::Justification::centredLeft, true);
}

juce::Rectangle<int> Page::body() const
{
    return getLocalBounds()
        .reduced(metric::pagePadX, 0)
        .withTrimmedTop(metric::pagePadTop + 26 + 4 + 18 + metric::sectionGap)
        .withTrimmedBottom(metric::pagePadBottom);
}

juce::Rectangle<int> Page::headerRight(int width) const
{
    return { getWidth() - metric::pagePadX - width, metric::pagePadTop + 4, width, metric::controlH };
}

/* ---- status ---------------------------------------------------------------------------------- */

class StatusPage::SummaryCard : public juce::Component
{
public:
    juce::String kicker, value, suffix, detail;
    State detailState = State::Idle;
    std::function<void()> onClick;

    SummaryCard() { setMouseCursor(juce::MouseCursor::PointingHandCursor); }

    void paint(juce::Graphics& g) override
    {
        const bool over = isMouseOver();
        drawCard(g, getLocalBounds().toFloat(), over ? colour::surface.brighter(0.05f) : colour::surface,
                 over ? colour::edgeMd : colour::edgeSm);
        auto r = getLocalBounds().reduced(16, 14);
        drawKicker(g, r.removeFromTop(14), kicker, colour::n500, type::micro, 0.12f);
        r.removeFromTop(10);
        auto line = r.removeFromTop(28);
        const auto vf = assets::font(type::statValue, true);
        g.setColour(colour::text);
        g.setFont(vf);
        const int vw = (int) juce::GlyphArrangement::getStringWidth(vf, value);
        g.drawText(value, line, juce::Justification::centredLeft, false);
        if (suffix.isNotEmpty())
        {
            g.setColour(colour::n500);
            g.setFont(assets::font(type::secondary));
            g.drawText(suffix, line.withTrimmedLeft(vw + 7).translated(0, 2), juce::Justification::centredLeft, true);
        }
        r.removeFromTop(8);
        drawStateLabel(g, r.removeFromTop(18), detailState, detail);
    }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseUp(const juce::MouseEvent& e) override
    {
        if (onClick && getLocalBounds().contains(e.getPosition()))
            onClick();
    }
};

StatusPage::StatusPage(InletdClient& c) : Page(c, "Status")
{
    InletLookAndFeel::setVariant(stopButton, "secondary", "stop");
    InletLookAndFeel::setVariant(startButton, "primary", "play");
    InletLookAndFeel::setVariant(servicesButton, "ghost", "arrow-square-out");
    stopButton.onClick = [this] { if (onStop) onStop(); };
    startButton.onClick = [this] { if (onStart) onStart(); };
    servicesButton.onClick = [] { service::openServices(); };
    stopButton.setTooltip(u8("Stops Inlet\xe2\x80\x99s service: the sound card, its streams and its clock."));
    for (auto* b : { &stopButton, &startButton, &servicesButton })
        addChildComponent(b);
    const char* kickers[] = { "Device", "Receivers", "Senders" };
    const int targets[] = { 3, 1, 2 };   // Settings, Receivers, Senders
    for (int i = 0; i < 3; ++i)
    {
        auto* card = cards.add(new SummaryCard());
        card->kicker = kickers[i];
        card->onClick = [this, page = targets[i]] { if (onOpenPage) onOpenPage(page); };
        addChildComponent(card);
    }
}

StatusPage::~StatusPage() = default;

juce::String StatusPage::subtitle() const
{
    if (!snap.reachable)
        return juce::SystemStats::getComputerName();
    const auto& st = snap.status;
    const auto iface = st.getProperty("interface", {});
    return str(st, "host") + dot + str(iface, "name") + dot + str(iface, "ip");
}

void StatusPage::update(const InletdClient::Snapshot& s)
{
    snap = s;
    serviceStatus = s.reachable ? service::Status::Running : service::query();
    const auto& st = s.status;
    if (s.reachable)
    {
        cards[0]->value = str(st, "inputs") + " in" + dot + str(st, "outputs") + " out";
        cards[0]->detail = kHz((double) st.getProperty("sampleRate", 48000)) + dot + u8("ASIO\xc2\xae driver");
        cards[0]->detailState = State::Ok;
        const auto rx = st.getProperty("receivers", {}), tx = st.getProperty("senders", {});
        int active = 0, receiving = 0, enabled = 0, sending = 0;
        for (int i = 0; rx.isArray() && i < rx.size(); ++i)
            if ((bool) rx[i].getProperty("active", false))
            {
                ++active;
                receiving += (bool) rx[i].getProperty("health", {}).getProperty("receiving", false) ? 1 : 0;
            }
        for (int i = 0; tx.isArray() && i < tx.size(); ++i)
        {
            enabled += (bool) tx[i].getProperty("enabled", true) ? 1 : 0;
            sending += (bool) tx[i].getProperty("health", {}).getProperty("sending", false) ? 1 : 0;
        }
        cards[1]->value = juce::String(active);
        cards[1]->suffix = "of " + juce::String(rx.size()) + " playing";
        cards[1]->detail = active == 0 ? "nothing playing" : juce::String(receiving) + " receiving";
        cards[1]->detailState = active == 0 ? State::Idle : (receiving == active ? State::Ok : State::Bad);
        cards[2]->value = juce::String(sending);
        cards[2]->suffix = "of " + juce::String(tx.size()) + " on the network";
        cards[2]->detail = enabled == 0 ? "all turned off" : (sending == enabled ? "all sending" : juce::String(enabled - sending) + " not sending");
        cards[2]->detailState = enabled == 0 ? State::Idle : (sending == enabled ? State::Ok : State::Warn);
    }
    stopButton.setVisible(s.reachable);
    for (auto* card : cards)
    {
        card->setVisible(s.reachable);
        card->repaint();
    }
    const bool installed = serviceStatus != service::Status::NotInstalled;
    const bool busy = serviceStatus == service::Status::Starting || serviceStatus == service::Status::Stopping;
    startButton.setVisible(!s.reachable && installed);
    startButton.setEnabled(!busy && serviceStatus != service::Status::Running);
    startButton.setButtonText(serviceStatus == service::Status::Starting ? u8("Starting\xe2\x80\xa6")
                              : serviceStatus == service::Status::Stopping ? u8("Stopping\xe2\x80\xa6")
                                                                            : juce::String("Start Inlet"));
    servicesButton.setVisible(!s.reachable && installed);
    resized();
    repaint();
}

void StatusPage::resized()
{
    stopButton.setBounds(headerRight(138));

    // Offline: the empty state, and the strip at the bottom.
    auto r = body();
    stripRect = r.removeFromBottom(40);
    emptyRect = r.withSizeKeepingCentre(juce::jmin(520, r.getWidth()), juce::jmin(300, r.getHeight()));
    {
        auto e = emptyRect;
        e.removeFromTop(56 + 22 + 30 + 12 + 66 + 18);
        auto row = e.removeFromTop(44);
        startButton.setBounds(row.removeFromLeft(176));
        row.removeFromLeft(12);
        servicesButton.setBounds(row.removeFromLeft(156).withSizeKeepingCentre(156, metric::controlH));
    }

    // Online: the clock, the cards, the programs.
    r = body();
    clockRect = r.removeFromTop(176);
    r.removeFromTop(12);
    auto cardsRow = r.removeFromTop(104);
    const int cw = (cardsRow.getWidth() - 24) / 3;
    for (int i = 0; i < 3; ++i)
    {
        cards[i]->setBounds(cardsRow.removeFromLeft(cw));
        cardsRow.removeFromLeft(12);
    }
    r.removeFromTop(metric::sectionGap + 4);
    programsRect = r;
}

void StatusPage::paint(juce::Graphics& g)
{
    Page::paint(g);
    if (snap.reachable)
        paintOnline(g);
    else
        paintOffline(g);
}

void StatusPage::paintOnline(juce::Graphics& g)
{
    const auto& st = snap.status;
    const auto ptp = st.getProperty("ptp", {});
    const auto state = str(ptp, "state");
    const bool locked = state == "locked";
    const bool holdover = state == "holdover";

    // The clock card: a soft diagonal ground, a top hairline in the state's colour.
    {
        auto r = clockRect.toFloat().reduced(0.5f);
        g.setGradientFill(juce::ColourGradient(colour::surface.interpolatedWith(colour::section, 0.35f), r.getX(), r.getY(),
                                               colour::surface, r.getRight(), r.getBottom(), false));
        g.fillRoundedRectangle(r, metric::radiusMd);
        g.setColour(colour::edgeSm);
        g.drawRoundedRectangle(r, metric::radiusMd, metric::hairline);
        g.setColour(locked ? colour::ok : colour::warn);
        g.fillRect(r.getX() + metric::radiusMd, r.getY(), r.getWidth() - 2 * metric::radiusMd, 1.0f);

        auto inner = clockRect.reduced(22, 20);
        auto left = inner.removeFromLeft((int) (inner.getWidth() * 0.54f));
        inner.removeFromLeft(24);
        drawKicker(g, left.removeFromTop(14), "PTP clock", colour::a300, type::micro, 0.12f);
        left.removeFromTop(10);
        auto titleRow = left.removeFromTop(30);
        const auto c = locked ? colour::ok : colour::warn;
        auto dotArea = titleRow.removeFromLeft(24).toFloat().withSizeKeepingCentre(16, 16);
        g.setColour(c.withAlpha(0.2f));
        g.fillEllipse(dotArea);
        g.setColour(c);
        g.fillEllipse(dotArea.reduced(4));
        juce::String heading, text;
        if (locked)
        {
            heading = "Locked to the grandmaster";
            text = u8("Inlet\xe2\x80\x99s clock is the network\xe2\x80\x99s media clock: streams from this grandmaster play without resampling.");
        }
        else if (holdover)
        {
            heading = "Holdover";
            text = "The grandmaster went quiet. Inlet keeps its last rate until it returns.";
        }
        else
        {
            heading = "Free-running";
            text = "No grandmaster on domain " + str(ptp, "domain", "0")
                 + u8(". Inlet runs on this computer\xe2\x80\x99s clock; streams are resampled.");
        }
        g.setColour(colour::text);
        g.setFont(assets::font(type::heroTitle, true));
        g.drawText(heading, titleRow, juce::Justification::centredLeft, true);
        left.removeFromTop(8);
        g.setColour(colour::n400);
        g.setFont(assets::font(type::secondary));
        if (!(bool) ptp.getProperty("available", true))
        {
            g.setColour(colour::warn);
            text = "PTP is unavailable: " + str(ptp, "reason");
        }
        g.drawFittedText(text, left, juce::Justification::topLeft, 4, 1.0f);

        // The definition grid.
        const int colW = inner.getWidth() / 2;
        auto cell = [&](int col, int row, const juce::String& label, const juce::String& value, bool mono) {
            juce::Rectangle<int> r2(inner.getX() + col * colW, inner.getY() + row * 68, colW - 12, 62);
            drawKicker(g, r2.removeFromTop(14), label, colour::n500, type::micro, 0.12f);
            r2.removeFromTop(6);
            g.setColour(colour::text);
            g.setFont(mono ? assets::mono(12.5f) : assets::font(type::body));
            g.drawFittedText(value, r2, juce::Justification::topLeft, 2, 1.0f);
        };
        const auto gm = str(ptp, "gmIdentity");
        const auto port = str(ptp, "portState");
        const double rate = juce::jmax(1.0, (double) st.getProperty("sampleRate", 48000));
        cell(0, 0, "Grandmaster", gm.isEmpty() ? dash : dashedIdentity(gm), gm.isNotEmpty());
        cell(1, 0, "Domain", str(ptp, "domain", "0") + (port.isEmpty() ? juce::String() : dot + port.toLowerCase()), false);
        cell(0, 1, "Path delay",
             locked || holdover ? juce::String((double) ptp.getProperty("meanPathDelayNs", 0) / 1000.0, 1) + u8(" \xc2\xb5s") : dash, false);
        cell(1, 1, "Link offset", juce::String((double) st.getProperty("linkOffsetFrames", 0) * 1000.0 / rate, 2) + " ms", false);
    }

    // Programs using Inlet.
    auto r = programsRect;
    drawKicker(g, r.removeFromTop(16), "Programs using Inlet", colour::n500, type::kicker, 0.1f);
    r.removeFromTop(6);
    const auto clients = st.getProperty("clients", {});
    if (!clients.isArray() || clients.size() == 0)
    {
        g.setColour(colour::n500);
        g.setFont(assets::font(type::table));
        g.drawText(u8("None. Choose \xe2\x80\x9cInlet\xe2\x80\x9d as the ASIO device in your audio software."),
                   r.removeFromTop(34), juce::Justification::centredLeft, true);
    }
    else
        for (int i = 0; i < clients.size() && r.getHeight() >= 34; ++i)
        {
            const auto c = clients[i];
            auto row = r.removeFromTop(34);
            g.setColour(colour::rowRule);
            g.fillRect(row.getX(), row.getBottom() - 1, row.getWidth(), 1);
            auto x = row.reduced(4, 0);
            g.setColour(colour::text);
            g.setFont(assets::font(type::table));
            g.drawText(str(c, "app"), x.removeFromLeft(juce::jmax(160, x.getWidth() / 3)), juce::Justification::centredLeft, true);
            const bool running = (bool) c.getProperty("running", false);
            const auto late = (juce::int64) c.getProperty("lateWakeups", 0);
            drawStateLabel(g, x.removeFromLeft(170), !running ? State::Idle : (late > 0 ? State::Warn : State::Ok),
                           !running ? "open, not running"
                                    : (late > 0 ? juce::String(late) + (late == 1 ? " late buffer" : " late buffers") : "running"));
            g.setColour(colour::n400);
            g.drawText(str(c, "bufferFrames") + " frames" + dot
                           + ((bool) c.getProperty("ownsOutputs", false) ? "sends its outputs" : "inputs only"),
                       x, juce::Justification::centredLeft, true);
        }

    const auto sap = st.getProperty("sap", {});
    if (message.isNotEmpty())
        drawStrip(g, r.withTrimmedTop(juce::jmax(0, r.getHeight() - 40)), message, messageState);
    else if (str(sap, "warning").isNotEmpty() && r.getHeight() >= 48)
        drawStrip(g, r.withTrimmedTop(r.getHeight() - 40), str(sap, "warning"), State::Warn);
}

void StatusPage::paintOffline(juce::Graphics& g)
{
    auto e = emptyRect;
    if (auto mark = assets::inletMark(colour::n600, metric::markStroke))
        mark->drawWithin(g, e.removeFromTop(56).removeFromLeft(56).toFloat(), juce::RectanglePlacement::xLeft, 1.0f);
    else
        e.removeFromTop(56);
    e.removeFromTop(22);
    const bool installed = serviceStatus != service::Status::NotInstalled;
    g.setColour(colour::text);
    g.setFont(assets::font(type::heroTitle, true));
    g.drawText(installed ? u8("Inlet isn\xe2\x80\x99t running") : u8("Inlet isn\xe2\x80\x99t installed"), e.removeFromTop(30),
               juce::Justification::centredLeft, true);
    e.removeFromTop(12);
    juce::String text;
    switch (serviceStatus)
    {
    case service::Status::NotInstalled:
        text = u8("The sound card runs as a background service, and it isn\xe2\x80\x99t installed. Run install.cmd from the Inlet package.");
        break;
    case service::Status::Starting:
        text = "The sound card is starting. This window connects on its own.";
        break;
    case service::Status::Stopping:
        text = "The sound card is stopping.";
        break;
    case service::Status::Running:
        text = u8("The sound card\xe2\x80\x99s service is running but not answering yet. This window reconnects on its own.");
        break;
    default:
        text = u8("The sound card is stopped, so audio software can\xe2\x80\x99t use Inlet and nothing is on the network. "
                  "Start it here; programs that had Inlet open need to reopen it.");
        break;
    }
    g.setColour(colour::n400);
    g.setFont(assets::font(type::body));
    g.drawFittedText(text, e.removeFromTop(66), juce::Justification::topLeft, 3, 1.0f);

    if (message.isNotEmpty())
        drawStrip(g, stripRect, message, messageState);
    else if (serviceStatus == service::Status::Unknown && snap.error.isNotEmpty())
        drawStrip(g, stripRect, snap.error, State::Bad);
}

/* ---- receivers --------------------------------------------------------------------------------- */

ReceiversPage::ReceiversPage(InletdClient& c) : Page(c, "Receivers")
{
    auto& h = table.getHeader();
    const int columnFlags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable;
    h.addColumn("Receiver", 1, 96, 60, -1, columnFlags);
    h.addColumn("Stream", 2, 220, 80, -1, columnFlags);
    h.addColumn("Ch", 3, 44, 36, -1, columnFlags);
    h.addColumn("State", 4, 126, 80, -1, columnFlags);
    h.addColumn("Packets", 5, 100, 60, -1, columnFlags);
    h.addColumn("Lost", 6, 64, 40, -1, columnFlags);
    h.addColumn("Clock", 7, 170, 80, -1, columnFlags);
    for (int id : { 3, 5, 6 })
        h.getProperties().set("right" + juce::String(id), true);
    h.setStretchToFitActive(true);
    tableLook(table);
    table.setModel(this);
    addAndMakeVisible(table);

    streamChoice.setTextWhenNothingSelected(u8("Choose a stream on the network\xe2\x80\xa6"));
    streamChoice.setTextWhenNoChoicesAvailable("No streams found yet");
    addAndMakeVisible(streamChoice);
    InletLookAndFeel::setVariant(play, "primary", "play");
    InletLookAndFeel::setVariant(stop, "secondary", "stop");
    InletLookAndFeel::setVariant(paste, "secondary", "plus");
    paste.setButtonText(u8("Add an SDP\xe2\x80\xa6"));
    for (auto* b : { &play, &stop, &paste })
        addAndMakeVisible(b);
    streamChoice.onChange = [this] { updateButtons(); };

    play.onClick = [this] {
        const int k = selectedReceiver();
        const int s = streamChoice.getSelectedItemIndex();
        if (k > 0 && s >= 0 && s < streams.size())
        {
            auto* body = new juce::DynamicObject();
            body->setProperty("streamId", streams[s].getProperty("id", ""));
            client.request("PUT", "/receivers/" + juce::String(k), juce::var(body));
        }
    };
    stop.onClick = [this] {
        if (const int k = selectedReceiver(); k > 0)
            client.request("DELETE", "/receivers/" + juce::String(k));
    };
    paste.onClick = [this] {
        auto* w = new juce::AlertWindow("Add a stream", u8("Paste the stream\xe2\x80\x99s SDP, from its device or controller."),
                                        juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("sdp", {}, {});
        if (auto* e = w->getTextEditor("sdp"))
        {
            e->setMultiLine(true, false);
            e->setReturnKeyStartsNewLine(true);
            e->setFont(assets::mono(12.5f));
            e->setSize(540, 220);
        }
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->addButton("Add", 1);
        for (int i = 0; i < w->getNumButtons(); ++i)
            if (auto* b = w->getButton(i))
                InletLookAndFeel::setVariant(*b, i == 1 ? "primary" : "secondary");
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int result) {
                               if (result != 1)
                                   return;
                               auto* body = new juce::DynamicObject();
                               body->setProperty("sdp", w->getTextEditorContents("sdp").replace("\r\n", "\n").replace("\n", "\r\n"));
                               client.request("POST", "/streams", juce::var(body));
                           }),
                           true);
    };
    updateButtons();
}

juce::String ReceiversPage::subtitle() const
{
    const int n = receivers.size();
    return "Streams from the network, as Net In 1" + (n > 1 ? enDash + juce::String(n) : juce::String())
         + " in your audio software.";
}

int ReceiversPage::selectedReceiver() const
{
    const int row = table.getSelectedRow();
    return row >= 0 && row < receivers.size() ? (int) receivers[row].getProperty("index", 0) : 0;
}

void ReceiversPage::selectedRowsChanged(int)
{
    updateButtons();
    repaint(bar);
}

void ReceiversPage::updateButtons()
{
    const int row = table.getSelectedRow();
    play.setEnabled(row >= 0 && streamChoice.getSelectedItemIndex() >= 0);
    stop.setEnabled(row >= 0 && row < receivers.size() && (bool) receivers[row].getProperty("active", false));
}

void ReceiversPage::update(const InletdClient::Snapshot& s)
{
    receivers.clear();
    const auto rx = s.status.getProperty("receivers", {});
    for (int i = 0; rx.isArray() && i < rx.size(); ++i)
        receivers.add(rx[i]);

    // Keep the chosen stream chosen across polls.
    const int chosenIndex = streamChoice.getSelectedItemIndex();
    const auto chosen = chosenIndex >= 0 && chosenIndex < streams.size() ? str(streams[chosenIndex], "id") : juce::String();
    streams.clear();
    for (int i = 0; s.streams.isArray() && i < s.streams.size(); ++i)
        streams.add(s.streams[i]);
    streamChoice.clear(juce::dontSendNotification);
    for (int i = 0; i < streams.size(); ++i)
    {
        const auto& st = streams.getReference(i);
        auto text = str(st, "name", "(unnamed)");
        if (str(st, "description").isNotEmpty())
            text << dot << str(st, "description");
        text << "  [" << str(st, "via").toUpperCase() << "]";
        streamChoice.addItem(text, i + 1);
        if (str(st, "id") == chosen)
            streamChoice.setSelectedItemIndex(i, juce::dontSendNotification);
    }
    table.updateContent();
    table.repaint();
    updateButtons();
    repaint();
}

void ReceiversPage::resized()
{
    paste.setBounds(headerRight(150));
    auto r = body();
    bar = r.removeFromBottom(96);
    r.removeFromBottom(12);
    table.setBounds(r);
    auto b = bar.reduced(16, 12);
    b.removeFromTop(18 + 8);
    auto row = b.removeFromTop(metric::controlH);
    stop.setBounds(row.removeFromRight(92));
    row.removeFromRight(10);
    play.setBounds(row.removeFromRight(196));
    row.removeFromRight(12);
    streamChoice.setBounds(row);
}

void ReceiversPage::paint(juce::Graphics& g)
{
    Page::paint(g);
    drawCard(g, bar.toFloat(), colour::surface, colour::edgeSm);
    auto label = bar.reduced(16, 12).removeFromTop(18);
    const int row = table.getSelectedRow();
    const auto f = assets::font(type::label);
    g.setFont(f);
    g.setColour(colour::text70);
    if (row >= 0 && row < receivers.size())
    {
        g.drawText("Play on ", label, juce::Justification::centredLeft, false);
        g.setColour(colour::a300);
        g.drawText(str(receivers[row], "label"), label.withTrimmedLeft((int) juce::GlyphArrangement::getStringWidth(f, "Play on ")),
                   juce::Justification::centredLeft, false);
    }
    else
        g.drawText("Select a receiver above", label, juce::Justification::centredLeft, false);
    drawMessage(g, label.withTrimmedLeft(label.getWidth() / 3), message, messageState);
}

void ReceiversPage::paintRowBackground(juce::Graphics& g, int, int w, int h, bool selected)
{
    rowBackground(g, w, h, selected);
}

void ReceiversPage::paintCell(juce::Graphics& g, int row, int column, int w, int h, bool selected)
{
    if (row >= receivers.size())
        return;
    const auto& r = receivers.getReference(row);
    const auto stream = r.getProperty("stream", {});
    const auto health = r.getProperty("health", {});
    const bool active = (bool) r.getProperty("active", false);
    const auto err = str(r, "error");
    const juce::Rectangle<int> area { 6, 0, w - 6, h };
    switch (column)
    {
    case 1: cellText(g, str(r, "label"), w, h, selected ? colour::a300 : colour::text); break;
    case 2:
        if (active)
            cellText(g, str(stream, "name"), w, h, colour::text);
        else if (err.isNotEmpty())
            cellText(g, err, w, h, colour::bad);
        else
            cellText(g, dash, w, h, colour::n600);
        break;
    case 3: if (active) cellText(g, str(stream, "channels"), w, h, colour::text, true); break;
    case 4:
        if (!active)
            drawStateLabel(g, area, State::Idle, "idle");
        else if ((bool) health.getProperty("receiving", false))
            drawStateLabel(g, area, State::Ok, "receiving");
        else
            drawStateLabel(g, area, State::Bad, "no packets");
        break;
    case 5: if (active) cellText(g, count(health.getProperty("packets", 0)), w, h, colour::n300, true); break;
    case 6:
        if (active)
            cellText(g, count(health.getProperty("lost", 0)), w, h,
                     (juce::int64) health.getProperty("lost", 0) > 0 ? colour::warn : colour::n500, true);
        break;
    case 7:
        if (active)
        {
            const bool bridged = (bool) health.getProperty("bridged", false);
            drawStateLabel(g, area, bridged ? State::Warn : State::Ok, bridged ? "resampled" : u8("Inlet\xe2\x80\x99s clock"));
        }
        break;
    default: break;
    }
}

/* ---- senders -------------------------------------------------------------------------------------- */

SendersPage::SendersPage(InletdClient& c) : Page(c, "Senders")
{
    auto& h = table.getHeader();
    const int columnFlags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable;
    h.addColumn("Sender", 1, 96, 60, -1, columnFlags);
    h.addColumn("Outputs", 2, 80, 60, -1, columnFlags);
    h.addColumn("Address", 3, 170, 80, -1, columnFlags);
    h.addColumn("Packet", 4, 80, 60, -1, columnFlags);
    h.addColumn("State", 5, 150, 80, -1, columnFlags);
    h.addColumn("Packets", 6, 104, 60, -1, columnFlags);
    h.addColumn("Silent", 7, 96, 50, -1, columnFlags);
    for (int id : { 6, 7 })
        h.getProperties().set("right" + juce::String(id), true);
    h.setStretchToFitActive(true);
    tableLook(table);
    table.setModel(this);
    addAndMakeVisible(table);
    InletLookAndFeel::setVariant(toggle, "secondary", "power");
    addAndMakeVisible(toggle);
    toggle.setEnabled(false);
    toggle.onClick = [this] {
        const int row = table.getSelectedRow();
        if (row < 0 || row >= senders.size())
            return;
        auto* body = new juce::DynamicObject();
        body->setProperty("enabled", !(bool) senders[row].getProperty("enabled", true));
        client.request("PUT", "/senders/" + str(senders[row], "index"), juce::var(body));
    };
}

juce::String SendersPage::subtitle() const
{
    const int n = senders.size();
    return u8("The computer\xe2\x80\x99s outputs, on the network as Net Out 1") + (n > 1 ? enDash + juce::String(n) : juce::String()) + ".";
}

void SendersPage::selectedRowsChanged(int row)
{
    const bool valid = row >= 0 && row < senders.size();
    toggle.setEnabled(valid);
    toggle.setButtonText(valid && !(bool) senders[row].getProperty("enabled", true) ? "Turn on" : "Turn off");
    repaint(bar);
}

void SendersPage::update(const InletdClient::Snapshot& s)
{
    senders.clear();
    const auto tx = s.status.getProperty("senders", {});
    for (int i = 0; tx.isArray() && i < tx.size(); ++i)
        senders.add(tx[i]);
    table.updateContent();
    table.repaint();
    selectedRowsChanged(table.getSelectedRow());
    repaint();
}

void SendersPage::resized()
{
    auto r = body();
    bar = r.removeFromBottom(60);
    r.removeFromBottom(12);
    table.setBounds(r);
    toggle.setBounds(bar.reduced(16, 12).removeFromRight(120));
}

void SendersPage::paint(juce::Graphics& g)
{
    Page::paint(g);
    drawCard(g, bar.toFloat(), colour::surface, colour::edgeSm);
    auto b = bar.reduced(16, 12);
    b.removeFromRight(120 + 16);
    const int row = table.getSelectedRow();
    const auto f = assets::font(type::secondary);
    g.setFont(f);
    if (row >= 0 && row < senders.size())
    {
        const auto& s = senders.getReference(row);
        const int first = (int) s.getProperty("firstOutput", 0) + 1, n = (int) s.getProperty("channels", 0);
        const auto label = str(s, "label");
        const int lw = (int) juce::GlyphArrangement::getStringWidth(f, label) + 2;
        auto left = b.removeFromLeft(b.getWidth() / 2);
        g.setColour(colour::a300);
        g.drawText(label, left.removeFromLeft(lw), juce::Justification::centredLeft, false);
        g.setColour(colour::n400);
        g.drawText(dot + "outputs " + juce::String(first) + enDash + juce::String(first + n - 1), left, juce::Justification::centredLeft, true);
    }
    else
    {
        g.setColour(colour::text70);
        g.drawText("Select a sender above", b.removeFromLeft(b.getWidth() / 2), juce::Justification::centredLeft, false);
    }
    drawMessage(g, b, message, messageState);
}

void SendersPage::paintRowBackground(juce::Graphics& g, int, int w, int h, bool selected)
{
    rowBackground(g, w, h, selected);
}

void SendersPage::paintCell(juce::Graphics& g, int row, int column, int w, int h, bool selected)
{
    if (row >= senders.size())
        return;
    const auto& s = senders.getReference(row);
    const auto health = s.getProperty("health", {});
    const int first = (int) s.getProperty("firstOutput", 0) + 1, n = (int) s.getProperty("channels", 0);
    const bool enabled = (bool) s.getProperty("enabled", true);
    const bool sending = (bool) health.getProperty("sending", false);
    const auto mono = assets::mono(12.5f);
    const juce::Rectangle<int> area { 6, 0, w - 6, h };
    switch (column)
    {
    case 1: cellText(g, str(s, "label"), w, h, selected ? colour::a300 : colour::text); break;
    case 2: cellText(g, juce::String(first) + enDash + juce::String(first + n - 1), w, h, colour::text); break;
    case 3: cellText(g, str(s, "group") + ":" + str(s, "port"), w, h, colour::n300, false, &mono); break;
    case 4: cellText(g, juce::String((double) s.getProperty("ptimeUs", 0) / 1000.0, 3).trimCharactersAtEnd("0").trimCharactersAtEnd(".") + " ms", w, h, colour::text); break;
    case 5:
        drawStateLabel(g, area, !enabled ? State::Idle : (sending ? State::Ok : State::Bad),
                       !enabled ? "off" : (sending ? "on the network" : "not sending"));
        break;
    case 6: cellText(g, count(health.getProperty("packets", 0)), w, h, colour::n300, true); break;
    case 7: cellText(g, count(health.getProperty("silentPackets", 0)), w, h, colour::n500, true); break;
    default: break;
    }
}

/* ---- settings -------------------------------------------------------------------------------------- */

SettingsPage::SettingsPage(InletdClient& c)
    : Page(c, "Settings"),
      rate(juce::StringArray { "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz" }, juce::Array<int> { 44100, 48000, 88200, 96000 })
{
    addAndMakeVisible(interfaceChoice);
    addAndMakeVisible(rate);
    interfaceChoice.onChange = [this] { markChanged(interfaceChoice); };
    rate.onChange = [this] { markChanged(rate); };

    addField("label", "Name on the network", {}, false)
        ->editor.setTextToShowWhenEmpty("Inlet (" + juce::SystemStats::getComputerName() + ")", colour::n600);
    addField("registryUrl", "NMOS registry", "blank finds one", false)
        ->editor.setTextToShowWhenEmpty(u8("Found on the network (mDNS)"), colour::n600);
    addField("ptpDomain", "PTP domain", {}, true);
    addField("linkOffsetUs", u8("Link offset, \xc2\xb5s"), "0 = 3 ms", true);
    addField("inputs", "Inputs", {}, true);
    addField("outputs", "Outputs", {}, true);
    addField("receivers", "Receivers", {}, true);
    addField("receiverChannels", "Channels each", {}, true);
    addField("senders", "Senders", {}, true);
    addField("senderChannels", "Channels each", {}, true);

    InletLookAndFeel::setVariant(applyButton, "primary", "check-circle");
    InletLookAndFeel::setVariant(revertButton, "ghost", "arrow-counter-clockwise");
    addAndMakeVisible(applyButton);
    addAndMakeVisible(revertButton);
    applyButton.onClick = [this] { apply(); };
    revertButton.onClick = [this] {
        setEditing(false);
        shown = juce::var();
        client.pollNow();
    };
    setEditing(false);
}

juce::String SettingsPage::subtitle() const
{
    return "The network, the clock and the shape of the sound card.";
}

SettingsPage::Field* SettingsPage::addField(const juce::String& key, const juce::String& label, const juce::String& hint, bool numeric)
{
    auto* f = fields.add(new Field());
    f->key = key;
    f->label = label;
    f->hint = hint;
    if (numeric)
        f->editor.setInputRestrictions(7, "0123456789");
    f->editor.setFont(assets::font(type::body));
    f->editor.setIndents(12, 10);
    f->editor.onTextChange = [this, f] { markChanged(f->editor); };
    addAndMakeVisible(f->editor);
    return f;
}

SettingsPage::Field* SettingsPage::field(const juce::String& key)
{
    for (auto* f : fields)
        if (f->key == key)
            return f;
    return nullptr;
}

void SettingsPage::markChanged(juce::Component& c)
{
    c.getProperties().set("changed", true);
    c.repaint();
    setEditing(true);
}

void SettingsPage::setEditing(bool e)
{
    editing = e;
    applyButton.setEnabled(e);
    revertButton.setEnabled(e);
    if (!e)
    {
        for (auto* f : fields)
        {
            f->editor.getProperties().set("changed", false);
            f->editor.repaint();
        }
        for (juce::Component* c : { (juce::Component*) &interfaceChoice, (juce::Component*) &rate })
        {
            c->getProperties().set("changed", false);
            c->repaint();
        }
    }
    repaint();
}

void SettingsPage::update(const InletdClient::Snapshot& s)
{
    setEnabled(s.reachable);
    setAlpha(s.reachable ? 1.0f : metric::disabledAlpha);
    if (!s.reachable || editing || juce::JSON::toString(s.settings) == juce::JSON::toString(shown))
        return;
    shown = s.settings;
    for (auto* f : fields)
        f->editor.setText(s.settings.getProperty(f->key, "").toString(), false);
    rate.setValue((int) s.settings.getProperty("sampleRate", 48000));

    // The interfaces, with the one configured (or "automatic") selected.
    interfaceChoice.clear(juce::dontSendNotification);
    interfaceChoice.addItem("Automatic", 1);
    const auto configured = s.settings.getProperty("interfaceIp", "").toString();
    int id = 2;
    for (int i = 0; s.interfaces.isArray() && i < s.interfaces.size(); ++i)
    {
        const auto n = s.interfaces[i];
        if ((bool) n.getProperty("loopback", false) || !(bool) n.getProperty("up", false))
            continue;
        juce::String text = str(n, "name") + dot + str(n, "ip");
        if ((bool) n.getProperty("wireless", false))
            text += u8(" \xe2\x80\x94 wireless, not for AES67");
        interfaceChoice.addItem(text, id);
        if (str(n, "ip") == configured)
            interfaceChoice.setSelectedId(id, juce::dontSendNotification);
        ++id;
    }
    if (configured.isEmpty())
        interfaceChoice.setSelectedId(1, juce::dontSendNotification);
    setEditing(false);
    repaint();
}

juce::String SettingsPage::deviceNote() const
{
    auto num = [this](const char* k) {
        for (auto* f : fields)
            if (f->key == k)
                return f->editor.getText().getIntValue();
        return 0;
    };
    const int r = num("receivers"), rc = num("receiverChannels"), in = num("inputs");
    const int s = num("senders"), sc = num("senderChannels"), out = num("outputs");
    const juce::String times = u8(" \xc3\x97 ");
    juce::String note = juce::String(r) + " receivers" + times + juce::String(rc) + " channels ";
    note += r * rc == in ? "fill the " + juce::String(in) + " inputs. "
                         : "use " + juce::String(juce::jmin(r * rc, in)) + " of the " + juce::String(in) + " inputs. ";
    note += juce::String(s) + " senders" + times + juce::String(sc) + " channels ";
    note += s * sc == out ? "carry the " + juce::String(out) + " outputs."
                          : "carry " + juce::String(juce::jmin(s * sc, out)) + " of the " + juce::String(out) + " outputs.";
    return note;
}

void SettingsPage::apply()
{
    auto* body = new juce::DynamicObject();
    for (auto* f : fields)
    {
        const auto text = f->editor.getText().trim();
        if (f->key == "label" || f->key == "registryUrl")
            body->setProperty(f->key, text);
        else
            body->setProperty(f->key, text.getIntValue());
    }
    body->setProperty("sampleRate", rate.getValue());
    // "Name · 1.2.3.4 ..." -> the address; "Automatic" -> empty.
    juce::String ip;
    if (interfaceChoice.getSelectedId() > 1)
        for (auto& p : juce::StringArray::fromTokens(interfaceChoice.getText(), " ", ""))
            if (p.containsOnly("0123456789.") && p.containsChar('.'))
                ip = p;
    body->setProperty("interfaceIp", ip);
    client.request("PUT", "/settings", juce::var(body));
    setEditing(false);
    shown = juce::var();
}

void SettingsPage::resized()
{
    auto r = body();
    footer = r.removeFromBottom(60);
    r.removeFromBottom(12);
    {
        auto f = footer.reduced(16, 12);
        applyButton.setBounds(f.removeFromRight(100));
        f.removeFromRight(8);
        revertButton.setBounds(f.removeFromRight(104));
    }

    const bool twoColumns = r.getWidth() >= 600;
    const int gap = 32;
    auto left = twoColumns ? r.removeFromLeft((r.getWidth() - gap) / 2) : r;
    auto right = twoColumns ? r.withTrimmedLeft(gap) : juce::Rectangle<int>();

    auto place = [](juce::Rectangle<int>& col, juce::Rectangle<int>& labelArea, juce::Component& c, int width) {
        labelArea = col.removeFromTop(17);
        col.removeFromTop(5);
        c.setBounds(col.removeFromTop(metric::controlH).withWidth(width > 0 ? juce::jmin(width, col.getWidth()) : col.getWidth()));
        col.removeFromTop(12);
    };
    auto placeField = [&](juce::Rectangle<int>& col, const char* key) {
        if (auto* f = field(key))
            place(col, f->labelArea, f->editor, 0);
    };
    auto pair = [&](juce::Rectangle<int>& col, const char* a, const char* b) {
        auto row = col.removeFromTop(17 + 5 + metric::controlH + 12);
        const int half = (row.getWidth() - 16) / 2;
        auto ra = row.removeFromLeft(half), rb = row.withTrimmedLeft(16);
        placeField(ra, a);
        placeField(rb, b);
    };

    networkLegend = left.removeFromTop(16);
    left.removeFromTop(10);
    place(left, interfaceLabel, interfaceChoice, 0);
    placeField(left, "label");
    placeField(left, "registryUrl");

    auto& col = twoColumns ? right : left;
    clockLegend = col.removeFromTop(16);
    col.removeFromTop(10);
    place(col, rateLabel, rate, rate.idealWidth());
    pair(col, "ptpDomain", "linkOffsetUs");
    col.removeFromTop(4);
    deviceLegend = col.removeFromTop(16);
    col.removeFromTop(10);
    pair(col, "inputs", "outputs");
    pair(col, "receivers", "receiverChannels");
    pair(col, "senders", "senderChannels");
    noteRect = twoColumns ? left.withTrimmedTop(12).removeFromTop(juce::jmin(68, juce::jmax(0, left.getHeight() - 12)))
                          : col.removeFromTop(juce::jmin(68, col.getHeight()));
}

void SettingsPage::paint(juce::Graphics& g)
{
    Page::paint(g);
    drawKicker(g, networkLegend, "Network", colour::a300, type::kicker, 0.1f);
    drawKicker(g, clockLegend, "Clock", colour::a300, type::kicker, 0.1f);
    drawKicker(g, deviceLegend, "Device", colour::a300, type::kicker, 0.1f);
    const auto lf = assets::font(type::label);
    auto label = [&](juce::Rectangle<int> r, const juce::String& text, const juce::String& hint) {
        g.setFont(lf);
        g.setColour(colour::text70);
        g.drawText(text, r, juce::Justification::centredLeft, false);
        if (hint.isNotEmpty())
        {
            g.setColour(colour::n600);
            g.drawText(dot + hint, r.withTrimmedLeft((int) juce::GlyphArrangement::getStringWidth(lf, text)),
                       juce::Justification::centredLeft, true);
        }
    };
    label(interfaceLabel, "Network interface", {});
    label(rateLabel, "Sample rate", {});
    for (auto* f : fields)
        label(f->labelArea, f->label, f->hint);

    // The note that reads the device numbers back.
    if (noteRect.getHeight() >= 40)
    {
        g.setColour(colour::surface.withAlpha(0.6f));
        g.fillRoundedRectangle(noteRect.toFloat(), metric::radiusMd);
        auto inner = noteRect.reduced(14, 12);
        if (auto icon = assets::icon("info", colour::a300))
            icon->drawWithin(g, inner.removeFromLeft(16).removeFromTop(18).toFloat(), juce::RectanglePlacement::centred, 1.0f);
        inner.removeFromLeft(10);
        g.setColour(colour::n300);
        g.setFont(assets::font(type::secondary));
        g.drawFittedText(deviceNote(), inner, juce::Justification::topLeft, 3, 1.0f);
    }

    // The footer bar.
    drawCard(g, footer.toFloat(), colour::surface, colour::edgeSm);
    auto f = footer.reduced(16, 12);
    f.removeFromRight(100 + 8 + 104 + 16);
    if (editing)
    {
        const juce::String tag = "Unsaved changes";
        drawTag(g, f.removeFromLeft(tagWidth(tag)).withSizeKeepingCentre(tagWidth(tag), 24), tag, TagStyle::Outline);
        f.removeFromLeft(14);
    }
    if (message.isNotEmpty())
        drawMessage(g, f, message, messageState);
    else
    {
        g.setColour(colour::n500);
        g.setFont(assets::font(12.5f));
        g.drawFittedText(u8("Applying restarts the sound card; programs using Inlet are asked to reset."), f,
                         juce::Justification::centredLeft, 2, 1.0f);
    }
}

/* ---- about --------------------------------------------------------------------------------------- */

AboutPage::AboutPage(InletdClient& c) : Page(c, "About")
{
    const void* bytes = nullptr;
    int size = 0;
    if (assets::data("ASIO-compatible-logo-Steinberg-TM-BW.png", bytes, size))
        asioLogo = juce::ImageCache::getFromMemory(bytes, size);
    source.setButtonText("Source code");
    source.setURL(juce::URL("https://github.com/meros-co/inlet"));
    licences.setButtonText("Licences");
    licences.onClick = [] {
        const auto f = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("THIRD-PARTY.txt");
        if (f.existsAsFile())
            f.startAsProcess();
        else
            juce::URL("https://github.com/meros-co/inlet/blob/main/LICENSE").launchInDefaultBrowser();
    };
    for (auto* l : { &source, &licences })
    {
        l->setFont(assets::font(type::secondary, true), false, juce::Justification::centredLeft);
        l->setColour(juce::HyperlinkButton::textColourId, colour::a300);
        addAndMakeVisible(l);
    }
}

void AboutPage::resized()
{
    auto r = getLocalBounds().reduced(metric::pagePadX, 0).withTrimmedTop(metric::pagePadTop).withTrimmedBottom(metric::pagePadBottom);
    hero = r.removeFromTop(186);
    r.removeFromTop(24);
    logoTile = r.removeFromRight(120).removeFromTop(150);
    r.removeFromRight(28);
    textRect = r.removeFromTop(juce::jmin(r.getHeight() - 40, 290));
    r.removeFromTop(8);
    auto links = r.removeFromTop(24);
    source.setBounds(links.removeFromLeft(96));
    links.removeFromLeft(16);
    licences.setBounds(links.removeFromLeft(72));
}

void AboutPage::paint(juce::Graphics& g)
{
    g.fillAll(colour::bg);

    // The hero: the app, its version, its family.
    {
        auto r = hero.toFloat().reduced(0.5f);
        g.setGradientFill(juce::ColourGradient(colour::section, r.getX(), r.getY(), colour::bg, r.getRight(), r.getBottom(), false));
        g.fillRoundedRectangle(r, metric::radiusLg);
        g.setColour(colour::edgeSm);
        g.drawRoundedRectangle(r, metric::radiusLg, metric::hairline);

        auto inner = hero.reduced(28, 26);
        auto top = inner.removeFromTop(72);
        g.drawImage(assets::appIcon(128), top.removeFromLeft(72).toFloat(), juce::RectanglePlacement::centred);
        top.removeFromLeft(20);
        auto nameRow = top.removeFromTop(40);
        const auto nameFont = assets::font(26.0f, true, 0.22f);
        g.setFont(nameFont);
        g.setColour(colour::text);
        const int nameW = (int) juce::GlyphArrangement::getStringWidth(nameFont, "INLET");
        g.drawText("INLET", nameRow, juce::Justification::centredLeft, false);
        g.setColour(colour::n500);
        g.setFont(assets::font(type::secondary));
        g.drawText("version " JUCE_APPLICATION_VERSION_STRING, nameRow.withTrimmedLeft(nameW + 14).translated(0, 2),
                   juce::Justification::centredLeft, false);
        g.setColour(colour::n300);
        g.setFont(assets::font(type::body));
        g.drawText("An AES67 virtual sound card.", top, juce::Justification::centredLeft, true);

        inner.removeFromTop(18);
        auto rule = inner.removeFromTop(1).withWidth(juce::jmin(520, inner.getWidth()));
        g.setGradientFill(juce::ColourGradient(colour::n700, (float) rule.getX(), 0.0f, colour::n700.withAlpha(0.0f),
                                               (float) rule.getRight(), 0.0f, false));
        g.fillRect(rule);
        inner.removeFromTop(16);
        auto family = inner.removeFromTop(24);
        const auto kf = assets::font(type::kicker, true, 0.12f);
        drawKicker(g, family.removeFromLeft(70), "Part of", colour::n500, type::kicker, 0.12f);
        if (auto m = assets::familyMark("manifold", colour::text))
            m->drawWithin(g, family.removeFromLeft(20).withSizeKeepingCentre(20, 20).toFloat(), juce::RectanglePlacement::centred, 1.0f);
        family.removeFromLeft(10);
        const auto mf = assets::font(13.0f, true, 0.17f);
        g.setColour(colour::text);
        g.setFont(mf);
        g.drawText("MANIFOLD", family.removeFromLeft((int) juce::GlyphArrangement::getStringWidth(mf, "MANIFOLD") + 4),
                   juce::Justification::centredLeft, false);
        family.removeFromLeft(26);
        juce::ignoreUnused(kf);
        for (const char* name : { "eddy", "basin", "sluice", "inlet" })
        {
            const bool self = juce::String(name) == "inlet";
            if (auto m = assets::familyMark(name, self ? colour::a300 : colour::n500))
                m->drawWithin(g, family.removeFromLeft(16).withSizeKeepingCentre(16, 16).toFloat(), juce::RectanglePlacement::centred, 1.0f);
            family.removeFromLeft(6);
            const auto nf = assets::font(type::label);
            const auto word = juce::String(name).substring(0, 1).toUpperCase() + juce::String(name).substring(1);
            g.setColour(self ? colour::a300 : colour::n500);
            g.setFont(nf);
            g.drawText(word, family.removeFromLeft((int) juce::GlyphArrangement::getStringWidth(nf, word) + 2),
                       juce::Justification::centredLeft, false);
            family.removeFromLeft(18);
        }
    }

    // The licences, and the ASIO trademark.
    juce::AttributedString as;
    as.setLineSpacing(5.0f);
    as.append(u8("Copyright \xc2\xa9 2026 Meros Inc. Inlet is free software under the GNU Affero General Public License, version 3 only.\n\n"),
              assets::font(type::secondary), colour::n300);
    as.append(u8("ASIO is a trademark and software of Steinberg Media Technologies GmbH. Inlet\xe2\x80\x99s ASIO\xc2\xae driver "
                 "is built with the ASIO SDK, used under the GNU General Public License, version 3.\n\n"),
              assets::font(type::secondary), colour::n300);
    as.append(u8("Also inside: GStreamer (LGPL) for PTP, libsamplerate (BSD), JUCE (AGPLv3), "
                 "the Inter typeface (SIL Open Font License 1.1) and Phosphor icons (MIT)."),
              assets::font(type::secondary), colour::n500);
    as.setWordWrap(juce::AttributedString::byWord);
    as.draw(g, textRect.toFloat());

    // The logo is black: it sits on a light tile so it stays legible.
    if (asioLogo.isValid())
    {
        const float w = (float) logoTile.getWidth();
        const float h = juce::jmin((float) logoTile.getHeight(), 16.0f * 2 + (w - 32.0f) * (float) asioLogo.getHeight() / (float) asioLogo.getWidth());
        auto tile = logoTile.toFloat().withHeight(h);
        g.setColour(colour::n100);
        g.fillRoundedRectangle(tile, metric::radiusMd);
        g.drawImage(asioLogo, tile.reduced(16.0f), juce::RectanglePlacement::centred);
    }
}

} // namespace inlet::app
