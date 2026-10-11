/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/


#include "Logger.h"
#include "Startup.h"
#include "Utils.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include "Options.h"

std::string getDate() {
    time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    tm local_tm = *localtime(&tt);
    std::stringstream date;
    int S = local_tm.tm_sec;
    int M = local_tm.tm_min;
    int H = local_tm.tm_hour;
    std::string Secs = (S > 9 ? std::to_string(S) : "0" + std::to_string(S));
    std::string Min = (M > 9 ? std::to_string(M) : "0" + std::to_string(M));
    std::string Hour = (H > 9 ? std::to_string(H) : "0" + std::to_string(H));
    date
        << "["
        << local_tm.tm_mday << "/"
        << local_tm.tm_mon + 1 << "/"
        << local_tm.tm_year + 1900 << " "
        << Hour << ":"
        << Min << ":"
        << Secs
        << "] ";
    return date.str();
}
// Multi-instance support (BeamMP Manager fork): when BEAMMP_INSTANCE_DIR is set,
// each launcher instance writes its own Launcher.log there instead of sharing
// one log file next to the executable.
static beammp_fs_string LogPath() {
    static const beammp_fs_string Path = []() -> beammp_fs_string {
#if defined(_WIN32)
        const wchar_t* Dir = _wgetenv(L"BEAMMP_INSTANCE_DIR");
#else
        const char* Dir = std::getenv("BEAMMP_INSTANCE_DIR");
#endif
        if (Dir != nullptr && *Dir != 0) {
            std::error_code ec;
            std::filesystem::path P(Dir);
            std::filesystem::create_directories(P, ec);
            return (P / beammp_wide("Launcher.log")).native();
        }
        return GetEP() + beammp_wide("Launcher.log");
    }();
    return Path;
}

void InitLog() {
    std::ofstream LFS;
    LFS.open(LogPath());
    if (!LFS.is_open()) {
        error("logger file init failed!");
    } else
        LFS.close();
}
void addToLog(const std::string& Line) {
    std::ofstream LFS;
    LFS.open(LogPath(), std::ios_base::app);
    LFS << Line.c_str();
    LFS.close();
}
void addToLog(const std::wstring& Line) {
    std::wofstream LFS;
    LFS.open(LogPath(), std::ios_base::app);
    LFS << Line.c_str();
    LFS.close();
}
void info(const std::string& toPrint) {
    std::string Print = getDate() + "[INFO] " + toPrint + "\n";
    beammp_stdout << Utils::ToWString(Print);
    addToLog(Print);
}
void debug(const std::string& toPrint) {
    std::string Print = getDate() + "[DEBUG] " + toPrint + "\n";
    if (options.verbose) {
        beammp_stdout << Utils::ToWString(Print);
    }
    addToLog(Print);
}
void warn(const std::string& toPrint) {
    std::string Print = getDate() + "[WARN] " + toPrint + "\n";
    beammp_stdout << Utils::ToWString(Print);
    addToLog(Print);
}
void error(const std::string& toPrint) {
    std::string Print = getDate() + "[ERROR] " + toPrint + "\n";
    beammp_stdout << Utils::ToWString(Print);
    addToLog(Print);
}
void fatal(const std::string& toPrint) {
    std::string Print = getDate() + "[FATAL] " + toPrint + "\n";
    beammp_stdout << Utils::ToWString(Print);
    addToLog(Print);
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::exit(1);
}
void except(const std::string& toPrint) {
    std::string Print = getDate() + "[EXCEP] " + toPrint + "\n";
    beammp_stdout << Utils::ToWString(Print);
    addToLog(Print);
}


#ifdef _WIN32
void info(const std::wstring& toPrint) {
    std::wstring Print = Utils::ToWString(getDate()) + L"[INFO] " + toPrint + L"\n";
    std::wcout << Print;
    addToLog(Print);
}
void debug(const std::wstring& toPrint) {
    std::wstring Print = Utils::ToWString(getDate()) + L"[DEBUG] " + toPrint + L"\n";
    if (options.verbose) {
        std::wcout << Print;
    }
    addToLog(Print);
}
void warn(const std::wstring& toPrint) {
    std::wstring Print = Utils::ToWString(getDate()) + L"[WARN] " + toPrint + L"\n";
    std::wcout << Print;
    addToLog(Print);
}
void error(const std::wstring& toPrint) {
    std::wstring Print = Utils::ToWString(getDate()) + L"[ERROR] " + toPrint + L"\n";
    std::wcout << Print;
    addToLog(Print);
}
void fatal(const std::wstring& toPrint) {
    std::wstring Print = Utils::ToWString(getDate()) + L"[FATAL] " + toPrint + L"\n";
    std::wcout << Print;
    addToLog(Print);
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::exit(1);
}
void except(const std::wstring& toPrint) {
    std::wstring Print = Utils::ToWString(getDate()) + L"[EXCEP] " + toPrint + L"\n";
    std::wcout << Print;
    addToLog(Print);
}
#endif