// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.

#include "inlet/PtpFollower.h"
#include "inlet/Clock.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include <gst/gst.h>
#include <gst/net/gstptpclock.h>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  define G_OS_HELPER_SUFFIX ".exe"
#elif defined(__APPLE__)
#  include <climits>
#  include <cstdlib>
#  include <mach-o/dyld.h>
#  define G_OS_HELPER_SUFFIX ""
#else
#  define G_OS_HELPER_SUFFIX ""
#endif

namespace inlet
{

std::string PtpStatus::identityText(const uint8_t id[8])
{
    char buf[24];
    std::snprintf(buf, sizeof buf, "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", id[0], id[1], id[2], id[3], id[4],
                  id[5], id[6], id[7]);
    return buf;
}

namespace
{

void identityBytes(guint64 id, uint8_t out[8])
{
    for (int i = 0; i < 8; ++i)
        out[i] = (uint8_t) (id >> (56 - 8 * i));
}

/** The follower's internal clock (GStreamer's monotonic system clock) less
    Inlet's: the tightest of a few bracketed readings. */
int64_t measureInternalMinusMono()
{
    GstClock* sys = gst_system_clock_obtain();
    int64_t best = 0, bestWidth = INT64_MAX;
    for (int i = 0; i < 7; ++i)
    {
        const int64_t a = monoNowNs();
        const int64_t g = (int64_t) gst_clock_get_internal_time(sys);
        const int64_t b = monoNowNs();
        if (b - a < bestWidth)
        {
            bestWidth = b - a;
            best = g - (a + (b - a) / 2);
        }
    }
    gst_object_unref(sys);
    return best;
}

} // namespace

struct PtpFollower::Impl
{
    int domain = 0;
    GstClock* clock = nullptr;
    gulong callbackId = 0;
    bool ptpStarted = false;

    mutable std::mutex lock;
    PtpStatus status;
    PtpCalibration cal;
    bool haveCal = false;
    int64_t internalMinusMono = 0;
    bool haveOffset = false;

    static gboolean onStats(guint8 domain, const GstStructure* s, gpointer user);
};

gboolean PtpFollower::Impl::onStats(guint8 domain, const GstStructure* s, gpointer user)
{
    auto* self = static_cast<Impl*>(user);
    if (domain != self->domain)
        return TRUE;
    std::lock_guard<std::mutex> g(self->lock);
    if (gst_structure_has_name(s, GST_PTP_STATISTICS_BEST_MASTER_CLOCK_SELECTED))
    {
        guint64 master = 0, gm = 0;
        guint port = 0;
        gst_structure_get_uint64(s, "master-clock-id", &master);
        gst_structure_get_uint64(s, "grandmaster-clock-id", &gm);
        gst_structure_get_uint(s, "master-clock-port", &port);
        identityBytes(master, self->status.masterIdentity);
        identityBytes(gm, self->status.gmIdentity);
        self->status.masterPort = port;
        self->status.gmPresent = true;
    }
    else if (gst_structure_has_name(s, GST_PTP_STATISTICS_PATH_DELAY_MEASURED))
    {
        guint64 d = 0;
        if (gst_structure_get_uint64(s, "mean-path-delay-avg", &d))
            self->status.meanPathDelayNs = (int64_t) d;
    }
    else if (gst_structure_has_name(s, GST_PTP_STATISTICS_TIME_UPDATED))
    {
        gboolean synced = FALSE;
        guint64 internal = 0, external = 0, num = 1, den = 1;
        gint64 disc = 0;
        gdouble r2 = 0;
        gst_structure_get_boolean(s, "synced", &synced);
        gst_structure_get_uint64(s, "internal-time", &internal);
        gst_structure_get_uint64(s, "external-time", &external);
        gst_structure_get_uint64(s, "rate-num", &num);
        gst_structure_get_uint64(s, "rate-den", &den);
        gst_structure_get_int64(s, "discontinuity", &disc);
        gst_structure_get_double(s, "r-squared", &r2);

        self->status.synced = synced != FALSE;
        self->status.lastDiscontinuityNs = disc;
        self->status.rSquared = r2;
        self->status.updates++;

        // GstClock calibration: external = (internal - cinternal) * num / den + cexternal.
        self->cal.synced = synced != FALSE && den != 0;
        self->cal.internalNs = (int64_t) internal;
        self->cal.externalNs = (int64_t) external;
        self->cal.rate = den != 0 ? (double) num / (double) den : 1.0;
        std::memcpy(self->cal.gmIdentity, self->status.gmIdentity, sizeof self->cal.gmIdentity);
        self->cal.domain = self->domain;
        self->haveCal = true;
    }
    return TRUE;
}

PtpFollower::PtpFollower(std::unique_ptr<Impl> i) : impl(std::move(i)) {}

PtpFollower::~PtpFollower()
{
    if (impl->callbackId != 0)
        gst_ptp_statistics_callback_remove(impl->callbackId);
    if (impl->clock != nullptr)
        gst_object_unref(impl->clock);
    if (impl->ptpStarted)
        gst_ptp_deinit();
}

std::unique_ptr<PtpFollower> PtpFollower::open(int domain, const std::vector<std::string>& interfaces,
                                               std::string& error)
{
    if (domain < 0 || domain > 255)
    {
        error = "PTP domain out of range";
        return nullptr;
    }
    if (!gst_is_initialized())
    {
        // Installed, Inlet carries GStreamer's PTP helper beside its own
        // executable rather than in a GStreamer installation.
        if (g_getenv("GST_PTP_HELPER_1_0") == nullptr)
        {
            gchar* self = g_file_read_link("/proc/self/exe", nullptr);   // Linux; null elsewhere, so Windows and macOS ask the OS
#if defined(_WIN32)
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            g_free(self);
            self = g_utf16_to_utf8(reinterpret_cast<const gunichar2*>(path), -1, nullptr, nullptr, nullptr);
#elif defined(__APPLE__)
            char path[PATH_MAX] = {}, real[PATH_MAX] = {};
            uint32_t size = sizeof path;
            g_free(self);
            self = nullptr;
            if (_NSGetExecutablePath(path, &size) == 0 && realpath(path, real) != nullptr)
                self = g_strdup(real);
#endif
            if (self != nullptr)
            {
                gchar* dir = g_path_get_dirname(self);
                gchar* helper = g_build_filename(dir, "gst-ptp-helper" G_OS_HELPER_SUFFIX, nullptr);
                if (g_file_test(helper, G_FILE_TEST_IS_EXECUTABLE))
                    g_setenv("GST_PTP_HELPER_1_0", helper, TRUE);
                g_free(helper);
                g_free(dir);
                g_free(self);
            }
        }
        gst_init(nullptr, nullptr);
    }

    auto impl = std::make_unique<Impl>();
    impl->domain = domain;
    impl->status.domain = domain;

    std::vector<gchar*> ifaces;
    for (const auto& s : interfaces)
        ifaces.push_back(const_cast<gchar*>(s.c_str()));
    ifaces.push_back(nullptr);
    if (!gst_ptp_init(GST_PTP_CLOCK_ID_NONE, interfaces.empty() ? nullptr : ifaces.data()))
    {
        error = "GStreamer's PTP helper did not start (is gst-ptp-helper installed, and are ports 319/320 free?)";
        return nullptr;
    }
    impl->ptpStarted = true;
    impl->callbackId = gst_ptp_statistics_callback_add(&Impl::onStats, impl.get(), nullptr);
    impl->clock = gst_ptp_clock_new("inlet-ptp", (guint) domain);
    if (impl->clock == nullptr)
    {
        error = "could not create the PTP clock";
        return nullptr;
    }
    impl->status.available = true;
    impl->internalMinusMono = measureInternalMinusMono();
    impl->haveOffset = true;
    return std::unique_ptr<PtpFollower>(new PtpFollower(std::move(impl)));
}

bool PtpFollower::latest(PtpCalibration& cal, int64_t& internalMinusMono)
{
    std::lock_guard<std::mutex> g(impl->lock);
    if (!impl->haveCal)
        return false;
    cal = impl->cal;
    internalMinusMono = impl->internalMinusMono;
    return true;
}

PtpStatus PtpFollower::status() const
{
    std::lock_guard<std::mutex> g(impl->lock);
    return impl->status;
}

int64_t PtpFollower::ptpNowNs() const
{
    if (impl->clock == nullptr || !gst_clock_is_synced(impl->clock))
        return -1;
    return (int64_t) gst_clock_get_time(impl->clock);
}

} // namespace inlet
