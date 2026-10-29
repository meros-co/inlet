// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
//
// inletd: Inlet's per-machine process. It owns the network so the drivers
// need not: PTP, the AES67 streams, SAP and NMOS, and the shared region the
// ASIO driver and the Core Audio plug-in attach to. Windows runs it as a
// service, macOS as a launchd daemon; `--console` runs it in a terminal.
//
//   inletd [--console] [--config PATH]
//   inletd --install-service | --uninstall-service      (Windows, as administrator)
//
// On macOS launchd also listens on a socket for it (co.meros.inletd.plist):
// a connection starts inletd when it is stopped, which is how the Inlet app's
// Start Inlet works without administrator rights.

#include "Config.h"
#include "Engine.h"

#include "inlet/SharedRegion.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <sddl.h>
#elif defined(__APPLE__)
#  include <cstdlib>
#  include <launch.h>
#  include <sys/socket.h>
#  include <thread>
#  include <unistd.h>
#endif

namespace
{

std::mutex gLock;
std::condition_variable gWake;
bool gQuit = false;
bool gRestart = false;
inlet::Config gPending;
/** Set in service mode: tells the SCM a stop the app asked for is under way. */
void (*gStopPending)() = nullptr;

void requestQuit()
{
    {
        std::lock_guard<std::mutex> g(gLock);
        gQuit = true;
    }
    gWake.notify_all();
}

/** Runs engines until asked to quit: a new one whenever the settings change,
    and again after a failure (the network may not be up yet at boot). */
int runDaemon(const std::string& configPath)
{
    std::string error;
    // One inletd per machine: the directory drivers find the region by is
    // also the guard.
    auto directory = inlet::shm::Directory::create(error);
    if (!directory)
    {
        std::fprintf(stderr, "inletd: %s\n", error.c_str());
        return 1;
    }
    inlet::Config config;
    if (!config.load(configPath, error))
    {
        std::fprintf(stderr, "inletd: %s (starting from the defaults)\n", error.c_str());
        config = inlet::Config();
    }
    for (;;)
    {
        {
            std::lock_guard<std::mutex> g(gLock);
            if (gQuit)
                break;
            gRestart = false;
        }
        inlet::Engine engine(config, configPath, *directory);
        engine.onRestart = [](const inlet::Config& next) {
            {
                std::lock_guard<std::mutex> g(gLock);
                gPending = next;
                gRestart = true;
            }
            gWake.notify_all();
        };
        engine.onStop = [] {
            std::fprintf(stderr, "inletd: stopping, as the Inlet app asked\n");
            if (gStopPending)
                gStopPending();
            requestQuit();
        };
        if (!engine.start(error))
        {
            std::fprintf(stderr, "inletd: cannot start: %s; trying again in 5 s\n", error.c_str());
            engine.stop();
            std::unique_lock<std::mutex> g(gLock);
            gWake.wait_for(g, std::chrono::seconds(5), [] { return gQuit; });
            continue;
        }
        {
            std::unique_lock<std::mutex> g(gLock);
            gWake.wait(g, [] { return gQuit || gRestart; });
        }
        engine.stop();
        std::lock_guard<std::mutex> g(gLock);
        if (gRestart)
        {
            config = gPending;
            std::fprintf(stderr, "inletd: settings changed; restarting\n");
        }
    }
    return 0;
}

#if defined(_WIN32)

const wchar_t* const kServiceName = L"inletd";
SERVICE_STATUS_HANDLE gStatusHandle = nullptr;
SERVICE_STATUS gStatus {};
std::string gConfigPath;

void setStatus(DWORD state)
{
    gStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gStatus.dwCurrentState = state;
    gStatus.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    gStatus.dwWin32ExitCode = NO_ERROR;
    SetServiceStatus(gStatusHandle, &gStatus);
}

DWORD WINAPI serviceControl(DWORD control, DWORD, LPVOID, LPVOID)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
    {
        setStatus(SERVICE_STOP_PENDING);
        requestQuit();
    }
    return NO_ERROR;
}

void WINAPI serviceMain(DWORD, LPWSTR*)
{
    gStatusHandle = RegisterServiceCtrlHandlerExW(kServiceName, serviceControl, nullptr);
    if (gStatusHandle == nullptr)
        return;
    // A service has no console: what it says goes to a log beside the settings.
    const std::string log = gConfigPath.substr(0, gConfigPath.find_last_of("\\/") + 1) + "inletd.log";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(gConfigPath).parent_path(), ec);
    std::freopen(log.c_str(), "a", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    setStatus(SERVICE_RUNNING);
    gStopPending = [] { setStatus(SERVICE_STOP_PENDING); };
    runDaemon(gConfigPath);
    setStatus(SERVICE_STOPPED);
}

int installService(bool install)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (scm == nullptr)
    {
        std::fprintf(stderr, "inletd: the service manager refused (run as administrator)\n");
        return 1;
    }
    int rc = 0;
    if (install)
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        // Quoted: "C:\Program Files\..." would otherwise be read as C:\Program.
        const std::wstring command = L"\"" + std::wstring(path) + L"\"";
        SC_HANDLE svc = CreateServiceW(scm, kServiceName, L"Inlet", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                       SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
                                       nullptr, nullptr);
        if (svc == nullptr)
        {
            std::fprintf(stderr, "inletd: could not install the service (error %lu)\n", GetLastError());
            rc = 1;
        }
        else
        {
            SERVICE_DESCRIPTIONW d { const_cast<wchar_t*>(L"Inlet AES67 virtual sound card: PTP, streams and NMOS for the Inlet driver") };
            ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d);
            // Windows' default rights, plus start and stop (RP, WP) for the
            // signed-in user (IU): the app's Start Inlet, and starting it again
            // at sign-in, without administrator rights.
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPWPLOCRRC;;;IU)"
                    L"(A;;CCLCSWLOCRRC;;;SU)",
                    SDDL_REVISION_1, &sd, nullptr))
            {
                if (!SetServiceObjectSecurity(svc, DACL_SECURITY_INFORMATION, sd))
                    std::fprintf(stderr, "inletd: could not let users start the service (error %lu)\n", GetLastError());
                LocalFree(sd);
            }
            StartServiceW(svc, 0, nullptr);
            CloseServiceHandle(svc);
        }
    }
    else
    {
        SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (svc != nullptr)
        {
            SERVICE_STATUS st;
            ControlService(svc, SERVICE_CONTROL_STOP, &st);
            DeleteService(svc);
            CloseServiceHandle(svc);
        }
    }
    CloseServiceHandle(scm);
    return rc;
}

#endif

#if defined(__APPLE__)
/** Started by launchd: takes the "Start" socket it listens on for inletd and
    answers its connections (by closing them), so launchd sees them handled.
    Nothing when inletd was started some other way. */
void serveStartSocket()
{
    int* fds = nullptr;
    size_t count = 0;
    if (launch_activate_socket("Start", &fds, &count) != 0 || count == 0)
        return;
    for (size_t i = 0; i < count; ++i)
        std::thread([fd = fds[i]] {
            for (;;)
            {
                const int c = accept(fd, nullptr, nullptr);
                if (c >= 0)
                    close(c);
            }
        }).detach();
    free(fds);
}
#endif

} // namespace

int main(int argc, char** argv)
{
    bool console = false;
    std::string configPath = inlet::Config::defaultPath();
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--console"))
            console = true;
        else if (!std::strcmp(argv[i], "--config") && i + 1 < argc)
            configPath = argv[++i];
#if defined(_WIN32)
        else if (!std::strcmp(argv[i], "--install-service"))
            return installService(true);
        else if (!std::strcmp(argv[i], "--uninstall-service"))
            return installService(false);
#endif
        else
        {
            std::fprintf(stderr, "usage: %s [--console] [--config PATH]\n", argv[0]);
            return 2;
        }
    }

#if defined(_WIN32)
    if (!console)
    {
        gConfigPath = configPath;
        SERVICE_TABLE_ENTRYW table[] = { { const_cast<wchar_t*>(kServiceName), serviceMain }, { nullptr, nullptr } };
        if (StartServiceCtrlDispatcherW(table))
            return 0;
        if (GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            return 1;
        // Not started by the service manager: run here.
    }
#endif
#if defined(__APPLE__)
    if (!console)
        serveStartSocket();
#endif
    std::signal(SIGINT, [](int) { requestQuit(); });
    std::signal(SIGTERM, [](int) { requestQuit(); });
    return runDaemon(configPath);
}
