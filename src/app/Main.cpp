// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// Inlet: the settings app for inletd, the process that runs the AES67 virtual
// sound card. A window (status, receivers, senders, settings) and a tray icon
// that stays when the window closes. One instance: starting it again (as the
// ASIO driver's control panel does) brings the window up.
//
//   Inlet.exe [--tray]      --tray: start with the window hidden (at sign-in)

#include "InletdClient.h"
#include "Pages.h"

#include <JuceHeader.h>

namespace inlet::app
{

namespace
{

/** The tray and window icon: a ring with an inward arrow, green when
    receiving on a locked clock, amber otherwise, grey when inletd is away. */
juce::Image makeIcon(int size, juce::Colour colour)
{
    juce::Image img(juce::Image::ARGB, size, size, true);
    juce::Graphics g(img);
    const float s = (float) size;
    g.setColour(colour);
    g.drawEllipse(s * 0.08f, s * 0.08f, s * 0.84f, s * 0.84f, s * 0.1f);
    juce::Path arrow;
    arrow.startNewSubPath(s * 0.22f, s * 0.5f);
    arrow.lineTo(s * 0.62f, s * 0.5f);
    g.strokePath(arrow, juce::PathStrokeType(s * 0.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    juce::Path head;
    head.addTriangle(s * 0.52f, s * 0.32f, s * 0.78f, s * 0.5f, s * 0.52f, s * 0.68f);
    g.fillPath(head);
    return img;
}

} // namespace

class MainComponent : public juce::Component
{
public:
    explicit MainComponent(InletdClient& c)
        : client(c), status(c), receivers(c), senders(c), settings(c), tabs(juce::TabbedButtonBar::TabsAtTop)
    {
        const auto bg = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
        tabs.addTab("Status", bg, &status, false);
        tabs.addTab("Receivers", bg, &receivers, false);
        tabs.addTab("Senders", bg, &senders, false);
        tabs.addTab("Settings", bg, &settings, false);
        tabs.addTab("About", bg, &about, false);
        addAndMakeVisible(tabs);
        banner.setJustificationType(juce::Justification::centredLeft);
        banner.setFont(juce::FontOptions(15.0f));
        addAndMakeVisible(banner);
        setSize(900, 600);
    }

    void update(const InletdClient::Snapshot& s)
    {
        for (Page* p : { (Page*) &status, (Page*) &receivers, (Page*) &senders, (Page*) &settings })
            p->update(s);
        if (!s.reachable)
            setBanner(s.error, juce::Colour(0xffe57373));
        else if (!message.isEmpty() && juce::Time::getMillisecondCounter() - messageAt < 8000)
            setBanner(message, messageOk ? juce::Colour(0xff4caf50) : juce::Colour(0xffe57373));
        else
            setBanner({}, {});
    }

    void actionDone(bool ok, const juce::String& text)
    {
        message = ok ? juce::String("Done.") : text;
        messageOk = ok;
        messageAt = juce::Time::getMillisecondCounter();
        setBanner(message, ok ? juce::Colour(0xff4caf50) : juce::Colour(0xffe57373));
        client.pollNow();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (banner.getText().isNotEmpty())
            banner.setBounds(r.removeFromBottom(28).reduced(10, 2));
        else
            banner.setBounds({});
        tabs.setBounds(r);
    }

private:
    void setBanner(const juce::String& text, juce::Colour colour)
    {
        if (banner.getText() == text)
            return;
        banner.setText(text, juce::dontSendNotification);
        banner.setColour(juce::Label::textColourId, colour);
        resized();
    }

    InletdClient& client;
    StatusPage status;
    ReceiversPage receivers;
    SendersPage senders;
    SettingsPage settings;
    AboutPage about;
    juce::TabbedComponent tabs;
    juce::Label banner;
    juce::String message;
    bool messageOk = true;
    juce::uint32 messageAt = 0;
};

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow(InletdClient& client)
        : juce::DocumentWindow("Inlet", juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                                            juce::ResizableWindow::backgroundColourId),
                               juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        content = new MainComponent(client);
        setContentOwned(content, true);
        setResizable(true, true);
        setResizeLimits(720, 460, 4000, 3000);
        centreWithSize(getWidth(), getHeight());
        setIcon(makeIcon(64, juce::Colour(0xff4caf50)));
    }

    /** Closing hides it: the tray icon keeps Inlet's status in reach. */
    void closeButtonPressed() override { setVisible(false); }

    MainComponent* content = nullptr;
};

class TrayIcon : public juce::SystemTrayIconComponent
{
public:
    std::function<void()> onOpen, onQuit;

    void setState(juce::Colour colour, const juce::String& tip)
    {
        if (colour != shown)
        {
            shown = colour;
            setIconImage(makeIcon(32, colour), makeIcon(32, colour));
        }
        setIconTooltip(tip);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem("Open Inlet", [this] { if (onOpen) onOpen(); });
            m.addSeparator();
            m.addItem("Quit the Inlet app (the sound card keeps running)", [this] { if (onQuit) onQuit(); });
            m.showMenuAsync({});
        }
        else if (onOpen)
            onOpen();
    }

private:
    juce::Colour shown;
};

class InletApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Inlet"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& commandLine) override
    {
        client = std::make_unique<InletdClient>();
        window = std::make_unique<MainWindow>(*client);
        tray = std::make_unique<TrayIcon>();
        tray->setState(juce::Colours::grey, "Inlet");
        tray->onOpen = [this] { show(); };
        tray->onQuit = [this] { systemRequestedQuit(); };
        client->onSnapshot = [this](const InletdClient::Snapshot& s) {
            window->content->update(s);
            const auto ptp = s.status.getProperty("ptp", {});
            if (!s.reachable)
                tray->setState(juce::Colours::grey, "Inlet: the service is not running");
            else if ((bool) ptp.getProperty("locked", false))
                tray->setState(juce::Colour(0xff4caf50), "Inlet: locked to " + ptp.getProperty("gmIdentity", "").toString());
            else
                tray->setState(juce::Colour(0xffffb74d), "Inlet: no PTP grandmaster (free-running)");
        };
        client->onActionDone = [this](bool ok, const juce::String& text) { window->content->actionDone(ok, text); };
        if (!commandLine.contains("--tray"))
            show();
    }

    void shutdown() override
    {
        tray.reset();
        window.reset();
        client.reset();
    }

    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        if (!commandLine.contains("--tray"))
            show();
    }

    void systemRequestedQuit() override { quit(); }

private:
    void show()
    {
        window->setVisible(true);
        window->toFront(true);
        client->pollNow();
    }

    std::unique_ptr<InletdClient> client;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<TrayIcon> tray;
};

} // namespace inlet::app

START_JUCE_APPLICATION(inlet::app::InletApplication)
