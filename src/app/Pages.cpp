// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "Pages.h"

#include "BinaryData.h"

namespace inlet::app
{

namespace
{

const juce::Colour kOk { 0xff4caf50 }, kWarn { 0xffffb74d }, kBad { 0xffe57373 };

juce::String str(const juce::var& v, const char* key, const juce::String& fallback = {})
{
    const auto p = v.getProperty(key, {});
    return p.isVoid() ? fallback : p.toString();
}

juce::String count(const juce::var& v)
{
    return v.isVoid() ? juce::String("0") : juce::String((juce::int64) v);
}

void drawCell(juce::Graphics& g, const juce::String& text, int w, int h, juce::Colour colour = juce::Colours::white)
{
    g.setColour(colour);
    g.setFont(14.0f);
    g.drawText(text, 6, 0, w - 8, h, juce::Justification::centredLeft, true);
}

} // namespace

/* ---- status ------------------------------------------------------------------------------ */

void StatusPage::update(const InletdClient::Snapshot& s)
{
    rows.clear();
    if (!s.reachable)
    {
        rows.add({ "Service", s.error, false, true });
        rows.add({ "", "Start it from Services (\"Inlet\"), or run install.ps1 again.", false, false });
        repaint();
        return;
    }
    const auto& st = s.status;
    const auto iface = st.getProperty("interface", {});
    rows.add({ "Inlet", "", true });
    rows.add({ "Computer", str(st, "host") + "  (" + str(st, "os") + ")" });
    rows.add({ "Network", str(iface, "name") + "  " + str(iface, "ip") });
    rows.add({ "Device", str(st, "inputs") + " inputs, " + str(st, "outputs") + " outputs at " + str(st, "sampleRate") + " Hz" });
    const double linkMs = (double) st.getProperty("linkOffsetFrames", 0) * 1000.0 / juce::jmax(1.0, (double) st.getProperty("sampleRate", 48000));
    rows.add({ "Link offset", juce::String(linkMs, 2) + " ms (how far behind the network clock inputs play)" });

    const auto ptp = st.getProperty("ptp", {});
    rows.add({ "Clock", "", true });
    const auto state = str(ptp, "state");
    const bool locked = (bool) ptp.getProperty("locked", false);
    rows.add({ "PTP", state == "locked" ? "Locked to the grandmaster"
                       : state == "holdover" ? "Holdover: the grandmaster went quiet; keeping time"
                                             : "Free-running: no grandmaster (Inlet runs on this computer's clock)",
               false, !locked });
    if (!(bool) ptp.getProperty("available", false))
        rows.add({ "", "PTP is unavailable: " + str(ptp, "reason"), false, true });
    if (str(ptp, "gmIdentity").isNotEmpty())
        rows.add({ "Grandmaster", str(ptp, "gmIdentity") + "  (domain " + str(ptp, "domain") + ", " + str(ptp, "portState") + ")" });
    if (locked)
        rows.add({ "Path delay", juce::String((double) ptp.getProperty("meanPathDelayNs", 0) / 1000.0, 1) + " us" });

    rows.add({ "Programs using Inlet", "", true });
    const auto clients = st.getProperty("clients", {});
    if (!clients.isArray() || clients.size() == 0)
        rows.add({ "", "None. Choose \"Inlet\" as the ASIO device in your audio software." });
    else
        for (int i = 0; i < clients.size(); ++i)
        {
            const auto c = clients[i];
            juce::String v = str(c, "kind").toUpperCase() + ", ";
            v += (bool) c.getProperty("running", false) ? "running at " + str(c, "bufferFrames") + " frames" : "stopped";
            if ((bool) c.getProperty("ownsOutputs", false))
                v += ", sends its outputs";
            const auto late = (juce::int64) c.getProperty("lateWakeups", 0);
            if (late > 0)
                v += ", " + juce::String(late) + (late == 1 ? " late buffer" : " late buffers");
            rows.add({ str(c, "app"), v, false, late > 0 });
        }

    rows.add({ "Network streams", "", true });
    int active = 0, receiving = 0, sending = 0;
    const auto rx = st.getProperty("receivers", {});
    for (int i = 0; rx.isArray() && i < rx.size(); ++i)
    {
        active += (bool) rx[i].getProperty("active", false) ? 1 : 0;
        receiving += (bool) rx[i].getProperty("health", {}).getProperty("receiving", false) ? 1 : 0;
    }
    const auto tx = st.getProperty("senders", {});
    for (int i = 0; tx.isArray() && i < tx.size(); ++i)
        sending += (bool) tx[i].getProperty("health", {}).getProperty("sending", false) ? 1 : 0;
    rows.add({ "Receivers", juce::String(active) + " of " + juce::String(rx.size()) + " playing a stream, " + juce::String(receiving) + " receiving",
               false, receiving < active });
    rows.add({ "Senders", juce::String(sending) + " of " + juce::String(tx.size()) + " on the network" });
    const auto sap = st.getProperty("sap", {});
    if (str(sap, "warning").isNotEmpty())
        rows.add({ "SAP", str(sap, "warning"), false, true });
    repaint();
}

void StatusPage::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
    int y = 12;
    for (auto& r : rows)
    {
        if (r.heading)
        {
            y += 8;
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(17.0f, juce::Font::bold));
            g.drawText(r.key, 16, y, getWidth() - 32, 24, juce::Justification::centredLeft);
            y += 28;
            continue;
        }
        g.setFont(15.0f);
        g.setColour(juce::Colours::lightgrey);
        g.drawText(r.key, 24, y, 190, 22, juce::Justification::centredLeft, true);
        g.setColour(r.warn ? kWarn : juce::Colours::white);
        g.drawText(r.value, 220, y, getWidth() - 236, 22, juce::Justification::centredLeft, true);
        y += 24;
    }
}

/* ---- receivers --------------------------------------------------------------------------- */

ReceiversPage::ReceiversPage(InletdClient& c) : Page(c)
{
    auto& h = table.getHeader();
    h.addColumn("Receiver", 1, 110);
    h.addColumn("Stream", 2, 260);
    h.addColumn("Channels", 3, 80);
    h.addColumn("State", 4, 110);
    h.addColumn("Packets", 5, 90);
    h.addColumn("Lost", 6, 70);
    h.addColumn("Clock", 7, 160);
    table.setModel(this);
    addAndMakeVisible(table);
    streamChoice.setTextWhenNothingSelected("Choose a stream on the network...");
    addAndMakeVisible(streamChoice);
    for (auto* b : { &play, &stop, &paste })
        addAndMakeVisible(b);

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
        auto* w = new juce::AlertWindow("Add a stream", "Paste the stream's SDP (from its device or controller):",
                                        juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("sdp", {}, {});
        if (auto* e = w->getTextEditor("sdp"))
        {
            e->setMultiLine(true, false);
            e->setReturnKeyStartsNewLine(true);
            e->setSize(520, 220);
        }
        w->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey, juce::ModifierKeys::ctrlModifier, 0));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int result) {
                               if (result == 1)
                               {
                                   auto* body = new juce::DynamicObject();
                                   body->setProperty("sdp", w->getTextEditorContents("sdp").replace("\n", "\r\n").replace("\r\r\n", "\r\n"));
                                   client.request("POST", "/streams", juce::var(body));
                               }
                           }),
                           true);
    };
    updateButtons();
}

int ReceiversPage::selectedReceiver() const
{
    const int row = table.getSelectedRow();
    return row >= 0 && row < receivers.size() ? (int) receivers[row].getProperty("index", 0) : 0;
}

void ReceiversPage::updateButtons()
{
    const int row = table.getSelectedRow();
    play.setEnabled(row >= 0 && streams.size() > 0);
    stop.setEnabled(row >= 0 && row < receivers.size() && (bool) receivers[row].getProperty("active", false));
}

void ReceiversPage::update(const InletdClient::Snapshot& s)
{
    receivers.clear();
    const auto rx = s.status.getProperty("receivers", {});
    for (int i = 0; rx.isArray() && i < rx.size(); ++i)
        receivers.add(rx[i]);

    // Keep the chosen stream chosen across polls.
    const auto chosen = streamChoice.getSelectedItemIndex() >= 0 && streamChoice.getSelectedItemIndex() < streams.size()
                            ? streams[streamChoice.getSelectedItemIndex()].getProperty("id", "").toString()
                            : juce::String();
    streams.clear();
    for (int i = 0; s.streams.isArray() && i < s.streams.size(); ++i)
        streams.add(s.streams[i]);
    streamChoice.clear(juce::dontSendNotification);
    for (int i = 0; i < streams.size(); ++i)
    {
        const auto& st = streams.getReference(i);
        streamChoice.addItem(str(st, "name", "(unnamed)") + "  -  " + str(st, "description") + "  [" + str(st, "via") + "]", i + 1);
        if (str(st, "id") == chosen)
            streamChoice.setSelectedItemIndex(i, juce::dontSendNotification);
    }
    table.updateContent();
    table.repaint();
    updateButtons();
}

void ReceiversPage::resized()
{
    auto r = getLocalBounds().reduced(12);
    auto bottom = r.removeFromBottom(32);
    r.removeFromBottom(8);
    table.setBounds(r);
    paste.setBounds(bottom.removeFromRight(140));
    bottom.removeFromRight(8);
    stop.setBounds(bottom.removeFromRight(80));
    bottom.removeFromRight(8);
    play.setBounds(bottom.removeFromRight(170));
    bottom.removeFromRight(8);
    streamChoice.setBounds(bottom);
}

void ReceiversPage::paintRowBackground(juce::Graphics& g, int, int, int, bool selected)
{
    g.fillAll(selected ? juce::Colour(0xff3a5a80) : juce::Colour(0xff2a2a2a));
}

void ReceiversPage::paintCell(juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (row >= receivers.size())
        return;
    const auto& r = receivers.getReference(row);
    const auto stream = r.getProperty("stream", {});
    const auto health = r.getProperty("health", {});
    const bool active = (bool) r.getProperty("active", false);
    switch (column)
    {
    case 1: drawCell(g, str(r, "label"), w, h); break;
    case 2: drawCell(g, active ? str(stream, "name") : (str(r, "error").isNotEmpty() ? str(r, "error") : "-"), w, h,
                     str(r, "error").isNotEmpty() && !active ? kBad : juce::Colours::white);
        break;
    case 3: drawCell(g, active ? str(stream, "channels") : "", w, h); break;
    case 4:
        if (!active)
            drawCell(g, "idle", w, h, juce::Colours::grey);
        else if ((bool) health.getProperty("receiving", false))
            drawCell(g, "receiving", w, h, kOk);
        else
            drawCell(g, "no packets", w, h, kBad);
        break;
    case 5: drawCell(g, active ? count(health.getProperty("packets", 0)) : "", w, h); break;
    case 6: drawCell(g, active ? count(health.getProperty("lost", 0)) : "", w, h,
                     (juce::int64) health.getProperty("lost", 0) > 0 ? kWarn : juce::Colours::white);
        break;
    case 7:
        if (active)
            drawCell(g, (bool) health.getProperty("bridged", false) ? "other clock: resampled" : "on Inlet's clock", w, h,
                     (bool) health.getProperty("bridged", false) ? kWarn : kOk);
        break;
    default: break;
    }
}

/* ---- senders ------------------------------------------------------------------------------ */

SendersPage::SendersPage(InletdClient& c) : Page(c)
{
    auto& h = table.getHeader();
    h.addColumn("Sender", 1, 110);
    h.addColumn("Outputs", 2, 110);
    h.addColumn("Address", 3, 170);
    h.addColumn("Packet time", 4, 100);
    h.addColumn("State", 5, 110);
    h.addColumn("Packets", 6, 90);
    h.addColumn("Silent", 7, 90);
    table.setModel(this);
    addAndMakeVisible(table);
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

void SendersPage::selectedRowsChanged(int row)
{
    toggle.setEnabled(row >= 0 && row < senders.size());
    if (row >= 0 && row < senders.size())
        toggle.setButtonText((bool) senders[row].getProperty("enabled", true) ? "Turn off" : "Turn on");
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
}

void SendersPage::resized()
{
    auto r = getLocalBounds().reduced(12);
    auto bottom = r.removeFromBottom(32);
    r.removeFromBottom(8);
    table.setBounds(r);
    toggle.setBounds(bottom.removeFromRight(120));
}

void SendersPage::paintRowBackground(juce::Graphics& g, int, int, int, bool selected)
{
    g.fillAll(selected ? juce::Colour(0xff3a5a80) : juce::Colour(0xff2a2a2a));
}

void SendersPage::paintCell(juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (row >= senders.size())
        return;
    const auto& s = senders.getReference(row);
    const auto health = s.getProperty("health", {});
    const int first = (int) s.getProperty("firstOutput", 0) + 1, n = (int) s.getProperty("channels", 0);
    const bool sending = (bool) health.getProperty("sending", false);
    switch (column)
    {
    case 1: drawCell(g, str(s, "label"), w, h); break;
    case 2: drawCell(g, juce::String(first) + "-" + juce::String(first + n - 1), w, h); break;
    case 3: drawCell(g, str(s, "group") + ":" + str(s, "port"), w, h); break;
    case 4: drawCell(g, juce::String((int) s.getProperty("ptimeUs", 0)) + " us", w, h); break;
    case 5: drawCell(g, sending ? "on the network" : "off", w, h, sending ? kOk : juce::Colours::grey); break;
    case 6: drawCell(g, count(health.getProperty("packets", 0)), w, h); break;
    case 7: drawCell(g, count(health.getProperty("silentPackets", 0)), w, h, juce::Colours::grey); break;
    default: break;
    }
}

/* ---- settings ------------------------------------------------------------------------------ */

SettingsPage::SettingsPage(InletdClient& c) : Page(c)
{
    interfaceLabel.setText("Network interface", juce::dontSendNotification);
    rateLabel.setText("Sample rate", juce::dontSendNotification);
    addAndMakeVisible(interfaceLabel);
    addAndMakeVisible(interfaceChoice);
    addAndMakeVisible(rateLabel);
    addAndMakeVisible(rateChoice);
    for (int r : { 44100, 48000, 88200, 96000 })
        rateChoice.addItem(juce::String(r) + " Hz", r);
    interfaceChoice.onChange = [this] { editing = true; };
    rateChoice.onChange = [this] { editing = true; };

    addField("ptpDomain", "PTP domain", true);
    addField("inputs", "Inputs (to the computer)", true);
    addField("outputs", "Outputs (from the computer)", true);
    addField("receivers", "Receivers", true);
    addField("receiverChannels", "Channels per receiver", true);
    addField("senders", "Senders", true);
    addField("senderChannels", "Channels per sender", true);
    addField("linkOffsetUs", "Link offset (us, 0 = default)", true);
    addField("label", "Name on the network", false);
    addField("registryUrl", "NMOS registry (blank: find one)", false);

    note.setText("Applying restarts Inlet's service; audio software using Inlet is asked to reset.", juce::dontSendNotification);
    note.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(note);
    addAndMakeVisible(applyButton);
    addAndMakeVisible(revertButton);
    applyButton.onClick = [this] { apply(); };
    revertButton.onClick = [this] {
        editing = false;
        shown = juce::var();
    };
}

void SettingsPage::addField(const juce::String& key, const juce::String& text, bool numeric)
{
    auto* f = fields.add(new Field());
    f->key = key;
    f->label.setText(text, juce::dontSendNotification);
    if (numeric)
        f->editor.setInputRestrictions(7, "0123456789");
    f->editor.onTextChange = [this] { editing = true; };
    addAndMakeVisible(f->label);
    addAndMakeVisible(f->editor);
}

void SettingsPage::update(const InletdClient::Snapshot& s)
{
    if (!s.reachable)
    {
        setEnabled(false);
        return;
    }
    setEnabled(true);
    if (editing || juce::JSON::toString(s.settings) == juce::JSON::toString(shown))
        return;
    shown = s.settings;
    for (auto* f : fields)
        f->editor.setText(s.settings.getProperty(f->key, "").toString(), false);
    rateChoice.setSelectedId((int) s.settings.getProperty("sampleRate", 48000), juce::dontSendNotification);

    // The interfaces, with the one in use (or "automatic") selected.
    interfaceChoice.clear(juce::dontSendNotification);
    interfaceChoice.addItem("Automatic (a wired interface with a gateway)", 1);
    const auto configured = s.settings.getProperty("interfaceIp", "").toString();
    int id = 2;
    for (int i = 0; s.interfaces.isArray() && i < s.interfaces.size(); ++i)
    {
        const auto n = s.interfaces[i];
        if ((bool) n.getProperty("loopback", false) || !(bool) n.getProperty("up", false))
            continue;
        juce::String text = str(n, "name") + "  " + str(n, "ip");
        if ((bool) n.getProperty("wireless", false))
            text += "  (wireless: not for AES67)";
        interfaceChoice.addItem(text, id);
        if (str(n, "ip") == configured)
            interfaceChoice.setSelectedId(id, juce::dontSendNotification);
        ++id;
    }
    if (configured.isEmpty())
        interfaceChoice.setSelectedId(1, juce::dontSendNotification);
}

void SettingsPage::apply()
{
    auto* body = new juce::DynamicObject();
    for (auto* f : fields)
    {
        const auto text = f->editor.getText().trim();
        if (f->editor.getText().containsOnly("0123456789") && f->key != "label" && f->key != "registryUrl")
            body->setProperty(f->key, text.getIntValue());
        else
            body->setProperty(f->key, text);
    }
    body->setProperty("sampleRate", rateChoice.getSelectedId());
    // "Name  1.2.3.4  ..." -> the address; "Automatic" -> empty.
    const auto choice = interfaceChoice.getText();
    juce::String ip;
    if (interfaceChoice.getSelectedId() > 1)
    {
        juce::StringArray parts;
        parts.addTokens(choice, " ", "");
        for (auto& p : parts)
            if (p.containsOnly("0123456789.") && p.containsChar('.'))
                ip = p;
    }
    body->setProperty("interfaceIp", ip);
    client.request("PUT", "/settings", juce::var(body));
    editing = false;
    shown = juce::var();
}

void SettingsPage::resized()
{
    auto r = getLocalBounds().reduced(16);
    auto row = [&r] {
        auto line = r.removeFromTop(28);
        r.removeFromTop(6);
        return line;
    };
    {
        auto line = row();
        interfaceLabel.setBounds(line.removeFromLeft(240));
        interfaceChoice.setBounds(line.removeFromLeft(460));
    }
    {
        auto line = row();
        rateLabel.setBounds(line.removeFromLeft(240));
        rateChoice.setBounds(line.removeFromLeft(160));
    }
    for (auto* f : fields)
    {
        auto line = row();
        f->label.setBounds(line.removeFromLeft(240));
        f->editor.setBounds(line.removeFromLeft(f->key == "label" || f->key == "registryUrl" ? 460 : 120));
    }
    r.removeFromTop(8);
    auto buttons = r.removeFromTop(30);
    applyButton.setBounds(buttons.removeFromLeft(100));
    buttons.removeFromLeft(8);
    revertButton.setBounds(buttons.removeFromLeft(100));
    r.removeFromTop(8);
    note.setBounds(r.removeFromTop(24));
}

/* ---- about ------------------------------------------------------------------------------------- */

AboutPage::AboutPage()
{
    asioLogo = juce::ImageCache::getFromMemory(BinaryData::ASIOcompatiblelogoSteinbergTMBW_png,
                                               BinaryData::ASIOcompatiblelogoSteinbergTMBW_pngSize);
}

void AboutPage::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
    auto r = getLocalBounds().reduced(20);
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    g.drawText("Inlet " + juce::String(JUCE_APPLICATION_VERSION_STRING), r.removeFromTop(32), juce::Justification::centredLeft);
    g.setFont(15.0f);
    const juce::String text = juce::CharPointer_UTF8(
        "An AES67 virtual sound card for Windows and macOS.\n"
        "Copyright (c) 2026 Meros Inc. Licensed under the GNU Affero General Public License, version 3 only.\n\n"
        "Inlet's ASIO\xc2\xae driver is built with the Steinberg ASIO SDK, used under the GNU General Public License, version 3.\n"
        "ASIO is a registered trademark of Steinberg Media Technologies GmbH.\n\n"
        "PTP is GStreamer's (LGPL). Resampling is libsamplerate (BSD). Built with JUCE (AGPLv3).");
    g.drawFittedText(text, r.removeFromTop(150), juce::Justification::topLeft, 10);
    if (asioLogo.isValid())
    {
        const float scale = 140.0f / (float) asioLogo.getWidth();
        g.drawImage(asioLogo, r.removeFromTop((int) (asioLogo.getHeight() * scale)).removeFromLeft(140).toFloat(),
                    juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yTop);
    }
}

} // namespace inlet::app
