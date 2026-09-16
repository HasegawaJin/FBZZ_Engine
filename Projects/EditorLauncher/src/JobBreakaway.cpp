/// @file    JobBreakaway.cpp
/// @brief   起動元の Job Object から抜けて、親の終了に巻き込まれないようにする。
/// @author  Hasegawa Jin
/// @date    2026-09-10
#include "JobBreakaway.hpp"

#include <Engine/Core/Logger.hpp>

#include <Windows.h>
#include <string>

namespace fbzz::editor_launcher {

namespace {

// 作り直した側に立てる印。これが立っていれば二度と作り直さない。
// WHY: 抜けたつもりで抜けられていない場合 (BREAKAWAY_OK が無い Job を読み違えた等) に、
//      自分を起動し続ける無限ループになる。1 回で打ち切る歯止めを必ず置く。
constexpr wchar_t kBreakawayMarker[] = L"FBZZ_JOB_BREAKAWAY_DONE";

struct JobStatus {
    bool inJob       = false;
    bool killOnClose = false;  // Job が閉じると配下ごと殺される
    bool breakawayOk = false;  // CREATE_BREAKAWAY_FROM_JOB が許されている
};

JobStatus QueryJobStatus()
{
    JobStatus status;

    BOOL inJob = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) || inJob == FALSE)
        return status;
    status.inJob = true;

    // 第 1 引数 nullptr で「自分が入っている Job」を問い合わせる。
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    DWORD returned = 0;
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
                                   &info, sizeof(info), &returned))
        return status;

    const DWORD flags = info.BasicLimitInformation.LimitFlags;
    status.killOnClose = (flags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
    status.breakawayOk = (flags & JOB_OBJECT_LIMIT_BREAKAWAY_OK) != 0;
    return status;
}

} // namespace

bool RelaunchOutsideKillOnCloseJob()
{
    if (GetEnvironmentVariableW(kBreakawayMarker, nullptr, 0) != 0)
        return false;  // 既に作り直した側

    const JobStatus status = QueryJobStatus();
    if (!status.inJob || !status.killOnClose)
        return false;  // 道連れにされない

    if (!status.breakawayOk) {
        // 抜ける手段が無い。黙って死ぬより «なぜ死ぬのか» を残す方がまだ良い。
        FBZZ_LOG_WARN("Editor is inside a kill-on-close job that forbids breakaway. "
                      "It will be terminated when its launcher exits "
                      "(this happens with GameHub started via 'npm start').");
        return false;
    }

    // 自分の完全なコマンドラインをそのまま渡す。引数の解析と再構築はしない
    // (パスに空白や日本語が入るので、分解して組み直すと壊す機会が増えるだけ)。
    std::wstring commandLine = GetCommandLineW();
    commandLine.push_back(L'\0');  // CreateProcessW は書き換え可能なバッファを要求する

    if (!SetEnvironmentVariableW(kBreakawayMarker, L"1")) {
        FBZZ_LOG_WARN("Job breakaway: cannot set the guard variable; staying in the job.");
        return false;
    }

    STARTUPINFOW        si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                                   CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS,
                                   nullptr, nullptr, &si, &pi);
    if (ok == FALSE) {
        // 起動できなかったのなら、この プロセスがそのまま続けるしかない。
        // 印を消しておかないと、次回の起動でも «作り直し済み» と誤認する。
        SetEnvironmentVariableW(kBreakawayMarker, nullptr);
        FBZZ_LOG_WARN("Job breakaway failed (error %lu); continuing inside the job.",
                      GetLastError());
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    FBZZ_LOG_INFO("Relaunched outside the launcher's job object (pid %lu).", pi.dwProcessId);
    return true;
}

} // namespace fbzz::editor_launcher
