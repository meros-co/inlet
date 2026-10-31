// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// Inlet: the app for inletd, the service that runs the AES67 virtual sound
// card. A window (status, receivers, senders, settings, about) and a tray icon
// that stays when the window closes (in the menu bar on macOS). One instance:
// starting it again (as the ASIO driver's control panel, or Audio MIDI Setup's
// Configure button, does) brings the window up.
//
//   Inlet.exe [--tray]      --tray: start hidden (at sign-in), and start the
//                           service if it is stopped

#include "Assets.h"
#include "InletLookAndFeel.h"
#include "InletTheme.h"
#include "InletdClient.h"
#include "Pages.h"
#include "Widgets.h"

#include <JuceHeader.h>

#if JUCE_WINDOWS
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace inlet::app
{

using namespace theme;

namespace
{

/** True when the taskbar is light (Windows' "system" theme), so the tray icon
    uses its dark-on-light drawing. (macOS draws the menu-bar icon from its
    shape alone, in the menu bar's colour.) */
bool lightTaskbar()
{
#if JUCE_MAC
    return !juce::Desktop::getInstance().isDarkModeActive();
#elif JUCE_WINDOWS
    return juce::WindowsRegistry::getValue(
               "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize\\SystemUsesLightTheme", "0")
               .getIntValue()
        != 0;
#else
    return false;
#endif
}

/** The programs using Inlet now ("Ableton Live, REAPER"), empty when none. */
juce::StringArray programsUsingInlet(const InletdClient::Snapshot& s)
{
    juce::StringArray names;
    for (const auto& c : programsIn(s.status))
        names.addIfNotAlreadyThere(c.getProperty("app", "").toString());
    names.removeEmptyStrings();
    return names;
}

/** An OK/Cancel question in the app's look; `then` runs on OK. */
void confirm(const juce::String& title, const juce::String& text, const juce::String& okText, std::function<void()> then)
{
    auto* w = new juce::AlertWindow(title, text, juce::MessageBoxIconType::NoIcon);
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->addButton(okText, 1);
    for (int i = 0; i < w->getNumButtons(); ++i)
        if (auto* b = w->getButton(i))
            InletLookAndFeel::setVariant(*b, i == 1 ? "primary" : "secondary");
    w->enterModalState(true, juce::ModalCallbackFunction::create([then = std::move(then)](int r) {
                           if (r == 1)
                               then();
                       }),
                       true);
}

} // namespace

class MainComponent : public juce::Component, private juce::Timer
{
public:
    explicit MainComponent(InletdClient& c) : client(c), status(c), receivers(c), senders(c), settings(c), about(c)
    {
        addAndMakeVisible(sidebar);
        pages = { &status, &receivers, &senders, &settings, &about };
        for (auto* p : pages)
            addChildComponent(p);
        sidebar.onSelect = [this](int i) { show(i); };
        status.onOpenPage = [this](int i) { show(i); };
        show(0);
        setSize(metric::windowW, metric::windowH);
    }

    std::function<void()> onStart, onStop;

    void update(const InletdClient::Snapshot& s)
    {
        sidebar.update(s);
        for (auto* p : pages)
            p->update(s);
    }

    /** A message on the page in view, for 8 s. */
    void showMessage(const juce::String& text, State s)
    {
        for (auto* p : pages)
            p->setMessage({}, State::Ok);
        pages[current]->setMessage(text, s);
        startTimer(8000);
    }

    void show(int page)
    {
        current = juce::jlimit(0, pages.size() - 1, page);
        sidebar.setSelected(current);
        for (int i = 0; i < pages.size(); ++i)
            pages[i]->setVisible(i == current);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        sidebar.setBounds(r.removeFromLeft(metric::sidebarW));
        for (auto* p : pages)
            p->setBounds(r);
    }

    StatusPage& statusPage() { return status; }

private:
    void timerCallback() override
    {
        stopTimer();
        for (auto* p : pages)
            p->setMessage({}, State::Ok);
    }

    InletdClient& client;
    Sidebar sidebar;
    StatusPage status;
    ReceiversPage receivers;
    SendersPage senders;
    SettingsPage settings;
    AboutPage about;
    juce::Array<Page*> pages;
    int current = 0;
};

class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow(InletdClient& client) : juce::DocumentWindow("Inlet", colour::bg, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        content = new MainComponent(client);
        setContentOwned(content, true);
        setResizable(true, false);
        setResizeLimits(720, 460, 4000, 3000);
        centreWithSize(getWidth(), getHeight());
        setIcon(assets::appIcon(256));
    }

    /** Closing hides it: the tray icon keeps Inlet in reach. */
    void closeButtonPressed() override { setVisible(false); }

    void visibilityChanged() override
    {
        juce::DocumentWindow::visibilityChanged();
#if JUCE_WINDOWS
        // A dark title bar, to match the window (Windows 10 2004 and later).
        if (auto* peer = getPeer())
        {
            BOOL dark = TRUE;
            DwmSetWindowAttribute((HWND) peer->getNativeHandle(), 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        }
#endif
    }

    MainComponent* content = nullptr;
};

class TrayIcon : public juce::SystemTrayIconComponent
{
public:
    std::function<void()> onOpen, onStart, onStop, onQuitApp, onQuitAll;
    bool running = false;

    void setState(const juce::String& state, const juce::String& tip)
    {
        const bool light = lightTaskbar();
        if (state != shownState || light != shownLight)
        {
            shownState = state;
            shownLight = light;
            const auto img = assets::trayIcon(state, light);
            setIconImage(img, img);
        }
        setIconTooltip(tip);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
#if JUCE_MAC
        // A menu-bar item: its menu on any click.
        juce::ignoreUnused(e);
        showDropdownMenu(menu());
#else
        if (!e.mods.isPopupMenu())
        {
            if (onOpen)
                onOpen();
            return;
        }
        menu().showMenuAsync(juce::PopupMenu::Options().withMinimumWidth(300));
#endif
    }

private:
    juce::PopupMenu menu()
    {
        juce::PopupMenu m;
        m.addItem("Open Inlet", [this] { if (onOpen) onOpen(); });
        if (running)
            m.addItem("Stop Inlet", [this] { if (onStop) onStop(); });
        else
            m.addItem("Start Inlet", [this] { if (onStart) onStart(); });
        m.addSeparator();
        m.addItem("Quit the Inlet app\nThe sound card keeps running", [this] { if (onQuitApp) onQuitApp(); });
        m.addItem("Quit Inlet completely\nStops the sound card until you start it again", [this] { if (onQuitAll) onQuitAll(); });
        return m;
    }

    juce::String shownState;
    bool shownLight = false;
};

class InletApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Inlet"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override
    {
#if JUCE_DEBUG
        // A debug build beside an installed Inlet: Inlet.exe --dev
        return getCommandLineParameters().contains("--dev");
#else
        return false;
#endif
    }

    void initialise(const juce::String& commandLine) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
#if JUCE_DEBUG
        // A test inletd on another port: Inlet.exe --dev --port=8092
        if (const auto p = commandLine.fromFirstOccurrenceOf("--port=", false, false); p.isNotEmpty())
            InletdClient::port = p.getIntValue();
#endif
        tooltips = std::make_unique<juce::TooltipWindow>(nullptr, 600);
        client = std::make_unique<InletdClient>();
        window = std::make_unique<MainWindow>(*client);
        tray = std::make_unique<TrayIcon>();
        tray->setState("away", "Inlet");
        tray->onOpen = [this] { show(); };
        tray->onStart = [this] { start(); };
        tray->onStop = [this] { stop(false); };
        tray->onQuitApp = [this] { quit(); };
        tray->onQuitAll = [this] { stop(true); };
        window->content->statusPage().onStart = [this] { start(); };
        window->content->statusPage().onStop = [this] { stop(false); };

        client->onSnapshot = [this](const InletdClient::Snapshot& s) {
            last = s;
            window->content->update(s);
            tray->running = s.reachable;
            const auto ptp = s.status.getProperty("ptp", {});
            if (!s.reachable)
                tray->setState("away", u8("Inlet: stopped"));
            else if (ptp.getProperty("state", "").toString() == "locked")
                tray->setState("locked", u8("Inlet: locked to the grandmaster"));
            else
                tray->setState("free", u8("Inlet: no grandmaster (free-running)"));
        };
        client->onActionDone = [this](bool ok, const juce::String& text) {
            window->content->showMessage(ok ? juce::String("Done.") : text, ok ? State::Ok : State::Bad);
            client->pollNow();
        };

        if (commandLine.contains("--tray"))
        {
            // At sign-in: bring the sound card back if it was stopped (the
            // service starts with the computer; this covers a stop since).
            if (service::query() == service::Status::Stopped)
            {
                juce::String ignored;
                service::start(ignored);
            }
        }
        else
            show();
#if JUCE_DEBUG
        // Screenshots of a page in a debug build: Inlet.exe --dev --page=3
        if (const auto page = commandLine.fromFirstOccurrenceOf("--page=", false, false); page.isNotEmpty())
            window->content->show(page.getIntValue());
#endif
    }

    void shutdown() override
    {
        tray.reset();
        window.reset();
        client.reset();
        tooltips.reset();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
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

    void start()
    {
        juce::String error;
        if (service::start(error))
            window->content->showMessage(u8("Starting Inlet\xe2\x80\xa6"), State::Ok);
        else
        {
            window->content->showMessage(error, State::Bad);
            show();
        }
        client->pollNow();
    }

    /** Stops the service (and, with `andQuit`, then the app), asking first when
        programs are using Inlet: they lose the sound card. */
    void stop(bool andQuit)
    {
        if (!last.reachable)
        {
            if (andQuit)
                quit();
            return;
        }
        const auto programs = programsUsingInlet(last);
        auto go = [this, andQuit] {
            client->request("POST", "/stop", {}, [this, andQuit](bool ok, const juce::String& text) {
                if (andQuit && ok)
                    return quit();
                window->content->showMessage(ok ? juce::String("Inlet stopped.") : text, ok ? State::Ok : State::Bad);
                if (!ok)
                    show();
                client->pollNow();
            });
        };
        if (programs.isEmpty())
            return go();
        const auto list = programs.joinIntoString(", ");
        if (!window->isVisible())
            show();
        confirm(andQuit ? "Quit Inlet completely?" : "Stop Inlet?",
                list + (programs.size() == 1 ? " is" : " are") + u8(" using Inlet. Stopping it silences the sound card: "
                                                                   "audio stops, and Inlet\xe2\x80\x99s streams leave the network until you start it again.")
                    + "\n\n" + (programs.size() == 1 ? "That program keeps" : "Those programs keep")
                    + " the device open and carry on when Inlet starts.",
                andQuit ? "Quit completely" : "Stop Inlet", go);
    }

    InletLookAndFeel lookAndFeel;
    std::unique_ptr<juce::TooltipWindow> tooltips;
    std::unique_ptr<InletdClient> client;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<TrayIcon> tray;
    InletdClient::Snapshot last;
};

} // namespace inlet::app

START_JUCE_APPLICATION(inlet::app::InletApplication)
