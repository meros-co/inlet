// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "Widgets.h"

#include "Assets.h"
#include "InletLookAndFeel.h"
#include "InletTheme.h"

#if JUCE_WINDOWS
#  include <windows.h>
#  include <shellapi.h>
#elif JUCE_MAC
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

namespace inlet::app
{

using namespace theme;

juce::Array<juce::var> programsIn(const juce::var& status)
{
    juce::Array<juce::var> out;
    const auto clients = status.getProperty("clients", {});
    for (int i = 0; clients.isArray() && i < clients.size(); ++i)
        if (clients[i].getProperty("kind", "").toString() != "coreaudio" || (bool) clients[i].getProperty("running", false))
            out.add(clients[i]);
    return out;
}

juce::String kHz(double rate)
{
    const double k = rate / 1000.0;
    return (std::fmod(rate, 1000.0) == 0.0 ? juce::String((int) k) : juce::String(k, 1)) + " kHz";
}

juce::Colour stateColour(State s)
{
    switch (s)
    {
    case State::Ok: return colour::ok;
    case State::Warn: return colour::warn;
    case State::Bad: return colour::bad;
    default: return colour::idle;
    }
}

void drawStateLabel(juce::Graphics& g, juce::Rectangle<int> r, State s, const juce::String& word, float fontSize,
                    juce::Justification just)
{
    const auto c = stateColour(s);
    const float w = stateLabelWidth(word, fontSize);
    float x = (float) r.getX();
    if (just.testFlags(juce::Justification::right))
        x = (float) r.getRight() - w;
    const float cy = (float) r.getCentreY();
    const juce::Rectangle<float> dot(x, cy - 3.5f, 7.0f, 7.0f);
    g.setColour(c);
    if (s == State::Idle)
        g.drawEllipse(dot.reduced(0.75f), 1.5f);
    else
        g.fillEllipse(dot);
    g.setFont(assets::font(fontSize));
    g.drawText(word, juce::Rectangle<float>(x + 14.0f, (float) r.getY(), w - 14.0f + 2.0f, (float) r.getHeight()),
               juce::Justification::centredLeft, true);
}

float stateLabelWidth(const juce::String& word, float fontSize)
{
    return 14.0f + juce::GlyphArrangement::getStringWidth(assets::font(fontSize), word);
}

int tagWidth(const juce::String& text)
{
    return (int) juce::GlyphArrangement::getStringWidth(assets::font(11.0f), text) + 20;
}

void drawTag(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, TagStyle style)
{
    auto rf = r.toFloat().reduced(0.5f);
    juce::Colour fill = colour::n800, ink = colour::n100;
    if (style == TagStyle::Accent)
    {
        fill = colour::a800;
        ink = colour::a100;
    }
    if (style == TagStyle::Outline)
    {
        g.setColour(colour::accent);
        g.drawRoundedRectangle(rf, metric::radiusTag, metric::hairline);
        ink = colour::accent;
    }
    else
    {
        g.setColour(fill);
        g.fillRoundedRectangle(rf, metric::radiusTag);
    }
    g.setColour(ink);
    g.setFont(assets::font(11.0f));
    g.drawText(text, r, juce::Justification::centred, false);
}

void drawCard(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, juce::Colour ring)
{
    r = r.reduced(0.5f);
    g.setColour(fill);
    g.fillRoundedRectangle(r, metric::radiusMd);
    g.setColour(ring);
    g.drawRoundedRectangle(r, metric::radiusMd, metric::hairline);
}

void drawKicker(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, juce::Colour c, float size, float tracking)
{
    g.setColour(c);
    g.setFont(assets::font(size, size >= 11.0f, tracking));
    g.drawText(text.toUpperCase(), r, juce::Justification::centredLeft, true);
}

void drawMessage(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, State s)
{
    if (text.isEmpty())
        return;
    const auto c = stateColour(s);
    if (auto icon = assets::icon(s == State::Ok ? "check-circle" : "warning", c))
        icon->drawWithin(g, r.removeFromLeft(16).withSizeKeepingCentre(16, 16).toFloat(), juce::RectanglePlacement::centred, 1.0f);
    r.removeFromLeft(8);
    g.setColour(c);
    g.setFont(assets::font(13.0f));
    g.drawFittedText(text, r, juce::Justification::centredLeft, 2);
}

void drawStrip(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, State s)
{
    const auto c = stateColour(s);
    auto rf = r.toFloat().reduced(0.5f);
    g.setColour(c.withAlpha(0.10f));
    g.fillRoundedRectangle(rf, metric::radiusMd);
    g.setColour(c.withAlpha(0.30f));
    g.drawRoundedRectangle(rf, metric::radiusMd, metric::hairline);
    drawMessage(g, r.reduced(14, 0), text, s);
}

juce::String dashedIdentity(const juce::String& id)
{
    // "02005e.fffe.123456" (linuxptp's form, which inletd reports) -> "02-00-5E-FF-FE-12-34-56".
    const auto hex = id.retainCharacters("0123456789abcdefABCDEF").toUpperCase();
    if (hex.length() != 16)
        return id;
    juce::StringArray pairs;
    for (int i = 0; i < 16; i += 2)
        pairs.add(hex.substring(i, i + 2));
    return pairs.joinIntoString("-");
}

/* ---- the segmented control ----------------------------------------------------------------- */

SegmentedControl::SegmentedControl(juce::StringArray l, juce::Array<int> v) : labels(std::move(l)), values(std::move(v))
{
    setWantsKeyboardFocus(true);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

int SegmentedControl::idealWidth() const
{
    int w = 0;
    for (auto& l : labels)
        w += (int) juce::GlyphArrangement::getStringWidth(assets::font(13.0f), l) + 26;
    return w;
}

juce::Array<juce::Rectangle<int>> SegmentedControl::segments() const
{
    juce::Array<juce::Rectangle<int>> out;
    auto r = getLocalBounds();
    const int total = idealWidth();
    int x = 0;
    for (int i = 0; i < labels.size(); ++i)
    {
        const int w = i == labels.size() - 1 ? r.getWidth() - x
                                             : (int) std::round(((float) juce::GlyphArrangement::getStringWidth(assets::font(13.0f), labels[i]) + 26.0f)
                                                                * (float) r.getWidth() / (float) total);
        out.add({ x, 0, w, r.getHeight() });
        x += w;
    }
    return out;
}

int SegmentedControl::segmentAt(int x) const
{
    const auto s = segments();
    for (int i = 0; i < s.size(); ++i)
        if (x >= s[i].getX() && x < s[i].getRight())
            return i;
    return -1;
}

void SegmentedControl::setValue(int v, juce::NotificationType n)
{
    if (v == value)
        return;
    value = v;
    repaint();
    if (n != juce::dontSendNotification && onChange)
        onChange();
}

void SegmentedControl::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    const bool changed = (bool) getProperties().getWithDefault("changed", false);
    g.setColour(changed ? colour::a600 : colour::divider);
    g.drawRoundedRectangle(r, metric::radiusMd, metric::hairline);
    const auto s = segments();
    for (int i = 0; i < s.size(); ++i)
    {
        auto seg = s[i].toFloat();
        if (i > 0)
        {
            g.setColour(colour::divider);
            g.fillRect(seg.getX(), 1.0f, 1.0f, seg.getHeight() - 2.0f);
        }
        const bool on = values[i] == value;
        if (!on && i == hover)
        {
            g.setColour(colour::hoverNeutral);
            g.fillRect(seg.reduced(1.0f));
        }
        if (on)
        {
            g.setColour(colour::accent);
            g.drawRect(seg.reduced(1.0f), 1.0f);
        }
        g.setColour(on ? colour::accent : colour::text);
        g.setFont(assets::font(13.0f));
        g.drawText(labels[i], s[i], juce::Justification::centred, false);
    }
    if (hasKeyboardFocus(false))
        InletLookAndFeel::drawFocusRing(g, r, metric::radiusMd);
}

void SegmentedControl::mouseUp(const juce::MouseEvent& e)
{
    const int i = segmentAt(e.x);
    if (i >= 0 && isEnabled())
        setValue(values[i], juce::sendNotification);
}

void SegmentedControl::mouseMove(const juce::MouseEvent& e)
{
    const int h = segmentAt(e.x);
    if (h != hover)
    {
        hover = h;
        repaint();
    }
}

void SegmentedControl::mouseExit(const juce::MouseEvent&)
{
    hover = -1;
    repaint();
}

bool SegmentedControl::keyPressed(const juce::KeyPress& k)
{
    const int i = values.indexOf(value);
    if (k == juce::KeyPress::leftKey && i > 0)
        setValue(values[i - 1], juce::sendNotification);
    else if (k == juce::KeyPress::rightKey && i < values.size() - 1)
        setValue(values[i + 1], juce::sendNotification);
    else
        return false;
    return true;
}

/* ---- the sidebar ------------------------------------------------------------------------------ */

class Sidebar::NavItem : public juce::Button
{
public:
    NavItem(const juce::String& name, const juce::String& iconName) : juce::Button(name), iconName(iconName)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        setWantsKeyboardFocus(true);
        setMouseClickGrabsKeyboardFocus(false);
    }

    void paintButton(juce::Graphics& g, bool over, bool) override
    {
        auto r = getLocalBounds().toFloat();
        const bool on = getToggleState();
        if (on || over)
        {
            g.setColour(on ? colour::a900 : colour::hoverNeutral);
            g.fillRoundedRectangle(r, metric::radiusMd);
        }
        const auto ink = on ? colour::a300 : (over ? colour::text : colour::n400);
        auto area = getLocalBounds().reduced(10, 0);
        if (auto icon = assets::icon(iconName, ink))
            icon->drawWithin(g, area.removeFromLeft(16).withSizeKeepingCentre(16, 16).toFloat(), juce::RectanglePlacement::centred, 1.0f);
        area.removeFromLeft(10);
        if (count.isNotEmpty())
        {
            g.setColour(colour::n500);
            g.setFont(assets::font(11.0f));
            g.drawText(count, area.removeFromRight(48), juce::Justification::centredRight, false);
        }
        g.setColour(ink);
        g.setFont(assets::font(type::body));
        g.drawText(getName(), area, juce::Justification::centredLeft, true);
        if (hasKeyboardFocus(false))
            InletLookAndFeel::drawFocusRing(g, r, metric::radiusMd);
    }

    bool keyPressed(const juce::KeyPress& k) override
    {
        auto* parent = dynamic_cast<Sidebar*>(getParentComponent());
        if (parent == nullptr)
            return false;
        const int i = parent->items.indexOf(this);
        if (k == juce::KeyPress::upKey && i > 0)
            parent->items[i - 1]->grabKeyboardFocus();
        else if (k == juce::KeyPress::downKey && i < parent->items.size() - 1)
            parent->items[i + 1]->grabKeyboardFocus();
        else if (k == juce::KeyPress::returnKey || k == juce::KeyPress::spaceKey)
            triggerClick();
        else
            return false;
        return true;
    }

    juce::String iconName, count;
};

Sidebar::Sidebar()
{
    const char* names[] = { "Status", "Receivers", "Senders", "Settings", "About" };
    const char* icons[] = { "pulse", "arrow-square-in", "arrow-square-out", "sliders-horizontal", "info" };
    for (int i = 0; i < 5; ++i)
    {
        auto* item = items.add(new NavItem(names[i], icons[i]));
        item->onClick = [this, i] {
            setSelected(i);
            if (onSelect)
                onSelect(i);
        };
        addAndMakeVisible(item);
    }
    setSelected(0);
}

Sidebar::~Sidebar() = default;

void Sidebar::setSelected(int page)
{
    selected = page;
    for (int i = 0; i < items.size(); ++i)
        items[i]->setToggleState(i == page, juce::dontSendNotification);
}

void Sidebar::update(const InletdClient::Snapshot& s)
{
    const auto st = s.status;
    if (!s.reachable)
    {
        items[1]->count = items[2]->count = {};
        clockState = State::Idle;
        clockLabel = "Stopped";
        clockDetail = u8("The sound card is off");
    }
    else
    {
        int active = 0, sending = 0;
        const auto rx = st.getProperty("receivers", {}), tx = st.getProperty("senders", {});
        for (int i = 0; rx.isArray() && i < rx.size(); ++i)
            active += (bool) rx[i].getProperty("active", false) ? 1 : 0;
        for (int i = 0; tx.isArray() && i < tx.size(); ++i)
            sending += (bool) tx[i].getProperty("health", {}).getProperty("sending", false) ? 1 : 0;
        items[1]->count = juce::String(active) + " / " + juce::String(rx.size());
        items[2]->count = juce::String(sending) + " / " + juce::String(tx.size());

        const auto ptp = st.getProperty("ptp", {});
        const auto state = ptp.getProperty("state", "").toString();
        if (state == "locked")
        {
            clockState = State::Ok;
            clockLabel = "Locked";
            clockDetail = "Domain " + ptp.getProperty("domain", 0).toString() + u8(" · ")
                        + kHz((double) st.getProperty("sampleRate", 48000)) + u8(" · path ")
                        + juce::String((double) ptp.getProperty("meanPathDelayNs", 0) / 1000.0, 1) + u8(" µs");
        }
        else if (state == "holdover")
        {
            clockState = State::Warn;
            clockLabel = "Holdover";
            clockDetail = "Grandmaster went quiet; keeping time";
        }
        else
        {
            clockState = State::Warn;
            clockLabel = "Free-running";
            clockDetail = u8("No grandmaster. Running on this computer’s clock");
        }
    }
    for (auto* i : items)
        i->repaint();
    repaint();
}

void Sidebar::resized()
{
    auto r = getLocalBounds().withTrimmedRight(1).reduced(12, 0).withTrimmedTop(22).withTrimmedBottom(14);
    r.removeFromTop(28 + 22);   // the lockup, and the gap after it
    for (auto* i : items)
    {
        i->setBounds(r.removeFromTop(metric::navItemH));
        r.removeFromTop(2);
    }
    clockCard = getLocalBounds().withTrimmedRight(1).reduced(12, 0).withTrimmedBottom(14);
    clockCard = clockCard.removeFromBottom(98);
}

void Sidebar::paint(juce::Graphics& g)
{
    g.fillAll(colour::rail);
    g.setColour(colour::railEdge);
    g.fillRect(getWidth() - 1, 0, 1, getHeight());

    // The lockup: the mark, a rule, the word.
    auto lockup = juce::Rectangle<int>(22, 22, getWidth() - 44, 28);
    if (auto mark = assets::inletMark(colour::a300))
        mark->drawWithin(g, lockup.removeFromLeft(24).withSizeKeepingCentre(24, 24).toFloat(), juce::RectanglePlacement::centred, 1.0f);
    lockup.removeFromLeft(12);
    g.setColour(colour::n800);
    g.fillRect(lockup.removeFromLeft(1).withSizeKeepingCentre(1, 18));
    lockup.removeFromLeft(12);
    g.setColour(colour::text);
    g.setFont(assets::font(type::wordmark, true, 0.22f));
    g.drawText("INLET", lockup, juce::Justification::centredLeft, false);

    // The clock card.
    drawCard(g, clockCard.toFloat(), colour::surface.withAlpha(0.7f), colour::edgeSm);
    auto c = clockCard.reduced(12);
    drawKicker(g, c.removeFromTop(14), "Clock", colour::n500, 10.0f, 0.12f);
    c.removeFromTop(8);
    drawStateLabel(g, c.removeFromTop(18), clockState, clockLabel, 13.0f);
    // The label in Medium, over the regular one drawn above.
    c.removeFromTop(6);
    g.setColour(colour::n500);
    g.setFont(assets::font(12.0f));
    g.drawFittedText(clockDetail, c, juce::Justification::topLeft, 2);
}

/* ---- the service ----------------------------------------------------------------------------------- */

namespace service
{

#if JUCE_WINDOWS
Status query()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr)
        return Status::Unknown;
    SC_HANDLE svc = OpenServiceW(scm, L"inletd", SERVICE_QUERY_STATUS);
    Status out = Status::NotInstalled;
    if (svc != nullptr)
    {
        SERVICE_STATUS st {};
        if (QueryServiceStatus(svc, &st))
            switch (st.dwCurrentState)
            {
            case SERVICE_RUNNING: out = Status::Running; break;
            case SERVICE_START_PENDING: out = Status::Starting; break;
            case SERVICE_STOP_PENDING: out = Status::Stopping; break;
            case SERVICE_STOPPED: out = Status::Stopped; break;
            default: out = Status::Unknown; break;
            }
        else
            out = Status::Unknown;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return out;
}

bool start(juce::String& error)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr)
    {
        error = "Windows' service manager did not answer.";
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, L"inletd", SERVICE_START);
    bool ok = false;
    if (svc == nullptr)
        error = GetLastError() == ERROR_ACCESS_DENIED
                    ? "Windows did not let the Inlet app start the service. Run install.cmd from this version once; "
                      "until then, start \"Inlet\" in Services."
                    : "Inlet's service is not installed. Run install.cmd.";
    else
    {
        ok = StartServiceW(svc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        if (!ok)
            error = "The service did not start (Windows error " + juce::String((int) GetLastError()) + "). See C:\\ProgramData\\Inlet\\inletd.log.";
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

void openServices()
{
    ShellExecuteW(nullptr, L"open", L"services.msc", nullptr, nullptr, SW_SHOWNORMAL);
}

juce::String openServicesLabel() { return "Open Services"; }
#elif JUCE_MAC
// What the installer puts in place (packaging/macos).
const char* const kDaemonPlist = "/Library/LaunchDaemons/co.meros.inletd.plist";
const char* const kStartSocket = "/var/run/co.meros.inletd.sock";
const char* const kLog = "/Library/Logs/Inlet/inletd.log";
// The lock a running inletd holds (inlet::shm::Directory).
const char* const kRunningLock = "/tmp/Inlet.Directory.lock";

Status query()
{
    if (!juce::File(kDaemonPlist).existsAsFile())
        return Status::NotInstalled;
    const int fd = ::open(kRunningLock, O_RDONLY);
    if (fd < 0)
        return Status::Stopped;
    const bool held = flock(fd, LOCK_SH | LOCK_NB) != 0;
    close(fd);
    return held ? Status::Running : Status::Stopped;
}

bool start(juce::String& error)
{
    if (!juce::File(kDaemonPlist).existsAsFile())
    {
        error = u8("Inlet\xe2\x80\x99s service is not installed. Install Inlet again from its installer package.");
        return false;
    }
    // launchd listens on this socket for inletd: a connection starts it.
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a {};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, kStartSocket, sizeof a.sun_path - 1);
    const bool ok = fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    const juce::String why(std::strerror(errno));
    if (fd >= 0)
        close(fd);
    if (!ok)
        error = "The service did not start (" + why + "). Its log is " + juce::String(kLog) + ".";
    return ok;
}

void openServices()
{
    juce::ChildProcess open;
    open.start(juce::StringArray { "/usr/bin/open", "-a", "Console", kLog }, 0);
    open.waitForProcessToFinish(5000);
}

juce::String openServicesLabel() { return "Open Log"; }
#else
Status query() { return Status::Unknown; }
bool start(juce::String& error)
{
    error = "Start inletd yourself.";
    return false;
}
void openServices() {}
juce::String openServicesLabel() { return "Open Services"; }
#endif

} // namespace service

} // namespace inlet::app
