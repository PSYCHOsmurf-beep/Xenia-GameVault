// RPCS3 Spartan Test trackers, ported to Xenia (PSYCHOsmurf / RPCS3 Spartan Test).
// Released under the BSD license, like Xenia - see LICENSE in the root.
//
// The app-side trackers (the stall and freeze trackers are in kernel/guest_scheduler_spartan.cc):
// - Memory: the app's memory against the limit the Xbox kills it at (5 GB as a Game, 1 GB as an App), and the Xbox
//   GPU memory against its budget. A line every 60 s while a game runs; a warning the first time 80, 90 and 95% of
//   the limit (or the GPU budget) is passed, so a game closed by the Xbox for memory leaves a trace.
// - Audio: every 10 s with gaps, how often the sound ran dry (XAudio2 found nothing queued) or was one frame from it.
// - Crashes: an exception nothing handled is written with where it happened (xeniacanary.exe+offset, for the /MAP
//   file) and the guest thread that ran, to LocalState\spartan_crash.txt and the log (flushed), before Windows ends
//   the app. The filter that was there before is still called after.

#include "spartan_tracker.h"

// First, so windows.h comes in with Xenia's settings (NOMINMAX, lean) before dxgi brings it in
#include "xenia/base/platform_win.h"

#include <algorithm>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <utility>
#include <string>
#include <thread>

#include <dxgi1_4.h>
#include <fileapifromapp.h>
#include <winrt/Windows.System.h>

#include "third_party/fmt/include/fmt/format.h"
#include "UWPUtil.h"
#include "version.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/spartan_trace.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

DECLARE_bool(log_safepoint_pc);

namespace spartan {
namespace {

// The crash path runs on a thread that has just faulted, maybe inside the heap or a lock: it formats into stack
// buffers, writes LocalState\spartan_crash.txt with plain file calls first, and only then tries Xenia's log.
wchar_t g_crash_path[1024] = {};
// Declared here: the app SDK's headers leave the top-level filter type out with the call.
using TopLevelFilter = LONG(WINAPI*)(EXCEPTION_POINTERS*);
TopLevelFilter g_previous_filter = nullptr;

template <typename... Args>
size_t Append(char* buffer, size_t size, size_t used, fmt::format_string<Args...> format, Args&&... args) {
  if (used >= size) return used;
  const auto result = fmt::format_to_n(buffer + used, size - used, format, std::forward<Args>(args)...);
  return std::min(size, used + size_t(result.size));
}

size_t WhereInApp(char* buffer, size_t size, size_t used, uintptr_t address) {
  const auto base = reinterpret_cast<uintptr_t>(&__ImageBase);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
  if (address >= base && address < base + nt->OptionalHeader.SizeOfImage) {
    return Append(buffer, size, used, "xeniacanary.exe+{:X}", address - base);
  }
  return Append(buffer, size, used, "{:X} (outside xeniacanary.exe: JIT code or a system library)", address);
}

size_t GuestThreadText(char* buffer, size_t size, size_t used) {
  // IsInThread(nullptr) is true when this host thread has no guest thread (GetCurrentThread would assert there)
  auto* thread = xe::kernel::XThread::IsInThread(nullptr) ? nullptr : xe::kernel::XThread::GetCurrentThread();
  if (!thread) {
    return Append(buffer, size, used, "no guest thread");
  }
  used = Append(buffer, size, used, "guest thread tid {:08X}{}", thread->thread_id(),
                thread->main_thread() ? " (main)" : "");
  if (auto* state = thread->thread_state()) {
    if (auto* context = state->context()) {
      used = Append(buffer, size, used, " pc {:08X} lr {:08X}", context->last_safepoint_pc, uint32_t(context->lr));
      if (context->gv_extern_pc) used = Append(buffer, size, used, " in import {:08X}", context->gv_extern_pc);
    }
  }
  return used;
}

void Report(const char* text, size_t length) {
  if (g_crash_path[0]) {
    HANDLE file = CreateFileFromAppW(g_crash_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(file, text, DWORD(length), &written, nullptr);
      FlushFileBuffers(file);
      CloseHandle(file);
    }
  }
  // The log writer thread would wait on itself.
  if (!xe::IsLogWriterThread()) {
    XELOGE("{}", std::string_view(text, length));
    xe::FlushLog();
  }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
  static std::atomic<bool> once{false};
  if (info && info->ExceptionRecord && !once.exchange(true)) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    char text[768];
    size_t used = Append(text, sizeof(text), 0, "Spartan crash: exception {:08X} at ", uint32_t(record.ExceptionCode));
    used = WhereInApp(text, sizeof(text), used, reinterpret_cast<uintptr_t>(record.ExceptionAddress));
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
      const ULONG_PTR kind = record.ExceptionInformation[0];
      used = Append(text, sizeof(text), used, ", {} {:X}",
                    kind == 0 ? "reading" : kind == 1 ? "writing" : "executing", record.ExceptionInformation[1]);
    }
    used = Append(text, sizeof(text), used, "; host thread {}; ", GetCurrentThreadId());
    used = GuestThreadText(text, sizeof(text), used);
    Report(text, used);
  }
  return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

std::terminate_handler g_previous_terminate = nullptr;

[[noreturn]] void OnTerminate() {
  static std::atomic<bool> once{false};
  if (!once.exchange(true)) {
    char text[512];
    size_t used = Append(text, sizeof(text), 0,
                         "Spartan crash: an error nothing handled ended the app (std::terminate); host thread {}; ",
                         GetCurrentThreadId());
    used = GuestThreadText(text, sizeof(text), used);
    Report(text, used);
  }
  if (g_previous_terminate) g_previous_terminate();
  std::abort();
}

void InstallCrashHandlers() {
  const std::string local = UWP::GetLocalState();
  if (!local.empty()) {
    const std::wstring path = xe::to_path(local).wstring() + L"\\spartan_crash.txt";
    if (path.size() < std::size(g_crash_path)) wcscpy_s(g_crash_path, path.c_str());
  }
  // The app SDK leaves SetUnhandledExceptionFilter out (as it does the vectored handler calls Xenia binds from
  // ntdll); kernelbase exports it.
  using SetFilterFn = TopLevelFilter(WINAPI*)(TopLevelFilter);
  SetFilterFn set_filter = nullptr;
  for (const wchar_t* module : {L"kernelbase.dll", L"kernel32.dll"}) {
    if (HMODULE handle = GetModuleHandleW(module)) {
      set_filter = reinterpret_cast<SetFilterFn>(GetProcAddress(handle, "SetUnhandledExceptionFilter"));
      if (set_filter) break;
    }
  }
  if (set_filter) {
    g_previous_filter = set_filter(CrashFilter);
  } else {
    XELOGW("Spartan trackers: no crash handler on this system (SetUnhandledExceptionFilter not found)");
  }
  g_previous_terminate = std::set_terminate(OnTerminate);
}

// Warns once per level passed; a level is armed again after use falls 10 points below it.
struct Levels {
  bool passed[3] = {};
  template <typename F>
  void Check(double percent, F&& warn) {
    static constexpr int kLevels[3] = {80, 90, 95};
    for (int i = 0; i < 3; ++i) {
      if (!passed[i] && percent >= kLevels[i]) {
        passed[i] = true;
        warn(kLevels[i]);
      } else if (passed[i] && percent < kLevels[i] - 10) {
        passed[i] = false;
      }
    }
  }
};

// The Xbox GPU's adapter, for its memory budget. Taken once on the thread that set the emulator up (a relaunch
// replaces the graphics system, so the watch thread never reads it); the adapter outlives a new device.
IDXGIAdapter3* FindAdapter(xe::Emulator* emulator) {
  IDXGIAdapter3* adapter = nullptr;
  auto* graphics = emulator->graphics_system();
  auto* provider = graphics ? static_cast<xe::ui::d3d12::D3D12Provider*>(graphics->provider()) : nullptr;
  if (provider && provider->GetDevice() && provider->GetDXGIFactory()) {
    IDXGIFactory4* factory = nullptr;
    if (SUCCEEDED(provider->GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&factory)))) {
      if (FAILED(factory->EnumAdapterByLuid(provider->GetDevice()->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
        adapter = nullptr;
      }
      factory->Release();
    }
  }
  return adapter;
}

void Watch(xe::Emulator* emulator, IDXGIAdapter3* adapter) {
  using namespace std::chrono;
  namespace sp = xe::spartan;
  Levels app_levels, gpu_levels;
  uint64_t app_peak = 0, gpu_peak = 0;
  auto last_audio = steady_clock::now(), last_memory = last_audio;
  for (;;) {
    std::this_thread::sleep_for(seconds(1));
    const auto now = steady_clock::now();
    if (sp::flush_log_requested.exchange(false)) {
      xe::FlushLog();
    }

    // Audio: every 10 s, only when the sound ran dry or nearly.
    if (now - last_audio >= seconds(10)) {
      last_audio = now;
      const uint64_t frames = sp::audio_frames.exchange(0), dry = sp::audio_dry.exchange(0),
                     one_left = sp::audio_one_left.exchange(0);
      const uint32_t fewest = sp::audio_min_queued.exchange(UINT32_MAX);
      if (dry || one_left) {
        XELOGW(
            "Spartan audio: in 10 s {} frames to XAudio2; the sound ran dry {} times (a gap), down to one frame {} "
            "times; fewest queued {} of {}",
            frames, dry, one_left, fewest == UINT32_MAX ? 0 : fewest, sp::audio_queue_size.load());
      }
    }

    // Memory: the app against its limit, the Xbox GPU memory against its budget.
    uint64_t app = 0, limit = 0;
    try {
      app = winrt::Windows::System::MemoryManager::AppMemoryUsage();
      limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
    } catch (...) {
    }
    uint64_t gpu = 0, budget = 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO video = {};
    if (adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &video))) {
      gpu = video.CurrentUsage;
      budget = video.Budget;
    }
    app_peak = std::max(app_peak, app);
    gpu_peak = std::max(gpu_peak, gpu);
    if (limit) {
      app_levels.Check(100.0 * double(app) / double(limit), [&](int level) {
        XELOGW("Spartan memory: the app passed {}% of its memory limit: {} MB of {} MB (the Xbox closes it at the "
               "limit)",
               level, app >> 20, limit >> 20);
        if (level >= 95) xe::FlushLog();
      });
    }
    if (budget) {
      gpu_levels.Check(100.0 * double(gpu) / double(budget), [&](int level) {
        XELOGW("Spartan memory: Xbox GPU memory passed {}% of its budget: {} MB of {} MB", level, gpu >> 20,
               budget >> 20);
      });
    }
    if (now - last_memory >= seconds(60) && emulator->is_title_open()) {
      last_memory = now;
      XELOGI("Spartan memory: app {} MB of {} MB (peak {} MB), Xbox GPU memory {} MB of a {} MB budget (peak {} MB)",
             app >> 20, limit >> 20, app_peak >> 20, gpu >> 20, budget >> 20, gpu_peak >> 20);
    }
  }
}

}  // namespace

void StartTrackers(xe::Emulator* emulator) {
  static std::atomic<bool> started{false};
  if (!emulator || started.exchange(true)) {
    return;
  }
  XELOGI(
      "Spartan trackers: on (short stalls from 80 ms, freezes from 1 s, audio gaps, memory levels, crashes) - the "
      "RPCS3 Spartan Test trackers by PSYCHOsmurf; source {} {}; thread pc = {}",
      XE_BUILD_BRANCH, XE_BUILD_COMMIT_SHORT,
      cvars::log_safepoint_pc ? "the last loop it passed (log_safepoint_pc on)"
                              : "where it was last preempted (log_safepoint_pc off)");
  InstallCrashHandlers();
  IDXGIAdapter3* adapter = FindAdapter(emulator);
  if (!adapter) {
    XELOGW("Spartan trackers: the GPU memory budget can't be read (no DXGI adapter), app memory only");
  }
  std::thread(Watch, emulator, adapter).detach();
}

}  // namespace spartan
