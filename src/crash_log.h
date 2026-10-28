// crash_log.h - crash report for the Juiced 2 recompilation.
//
// When the game dies from abort(), an uncaught C++ exception, a bad C runtime
// call or an unhandled Windows exception, this writes the reason and a stack
// trace to juiced2_crash.txt next to juiced2.exe. Symbol names come from the
// build's .pdb file when it can be found. For an unhandled Windows exception it
// also records what every OTHER thread of the game was doing at that moment, so
// timing problems between the game's threads can be understood.
// Nothing here changes how the game runs; it only records what happened.
#pragma once

#ifdef _WIN32

#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

namespace juiced2_crash {

inline HANDLE& LogHandle() {
  static HANDLE h = INVALID_HANDLE_VALUE;
  return h;
}

// One report at a time: if two threads crash together their reports must not mix.
struct ReportLockHolder {
  CRITICAL_SECTION cs;
  ReportLockHolder() { InitializeCriticalSection(&cs); }
};
inline CRITICAL_SECTION& ReportLock() {
  static ReportLockHolder holder;
  return holder.cs;
}

// Plain unbuffered write so nothing is lost if the process dies right after.
inline void Write(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) {
    return;
  }
  if (n >= static_cast<int>(sizeof(buf))) {
    n = static_cast<int>(sizeof(buf)) - 1;
  }
  HANDLE h = LogHandle();
  if (h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(h, buf, static_cast<DWORD>(n), &written, nullptr);
    FlushFileBuffers(h);
  }
  OutputDebugStringA(buf);
}

// dbghelp.dll is loaded only when a crash is being reported.
struct DbgHelp {
  bool tried = false;
  bool ok = false;
  BOOL(WINAPI* SymInit)(HANDLE, PCSTR, BOOL) = nullptr;
  DWORD(WINAPI* SymOpts)(DWORD) = nullptr;
  BOOL(WINAPI* SymFromAddrFn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO) = nullptr;
  BOOL(WINAPI* SymLineFn)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64) = nullptr;
};

inline DbgHelp& Dbg() {
  static DbgHelp d;
  return d;
}

inline void InitSymbols() {
  DbgHelp& d = Dbg();
  if (d.tried) {
    return;
  }
  d.tried = true;
  HMODULE m = LoadLibraryA("dbghelp.dll");
  if (!m) {
    Write("(dbghelp.dll could not be loaded; showing addresses only)\n");
    return;
  }
  d.SymInit = reinterpret_cast<BOOL(WINAPI*)(HANDLE, PCSTR, BOOL)>(GetProcAddress(m, "SymInitialize"));
  d.SymOpts = reinterpret_cast<DWORD(WINAPI*)(DWORD)>(GetProcAddress(m, "SymSetOptions"));
  d.SymFromAddrFn =
      reinterpret_cast<BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO)>(GetProcAddress(m, "SymFromAddr"));
  d.SymLineFn = reinterpret_cast<BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64)>(
      GetProcAddress(m, "SymGetLineFromAddr64"));
  if (!d.SymInit || !d.SymOpts || !d.SymFromAddrFn) {
    Write("(dbghelp.dll is missing functions; showing addresses only)\n");
    return;
  }
  d.SymOpts(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  Write("(loading symbols; this can take a little while for a big game...)\n");
  d.ok = d.SymInit(GetCurrentProcess(), nullptr, TRUE) != FALSE;
  if (!d.ok) {
    Write("(SymInitialize failed; showing addresses only)\n");
  }
}

inline void WriteFrame(int index, void* addr) {
  const DWORD64 a = static_cast<DWORD64>(reinterpret_cast<uintptr_t>(addr));
  char modname[MAX_PATH] = "?";
  DWORD64 modbase = 0;
  HMODULE hm = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(addr), &hm) &&
      hm) {
    GetModuleFileNameA(hm, modname, MAX_PATH);
    modbase = static_cast<DWORD64>(reinterpret_cast<uintptr_t>(hm));
  }
  const char* base = std::strrchr(modname, '\\');
  base = base ? base + 1 : modname;

  char symbuf[sizeof(SYMBOL_INFO) + 512];
  std::memset(symbuf, 0, sizeof(symbuf));
  SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 511;
  DWORD64 disp = 0;
  DbgHelp& d = Dbg();
  if (d.ok && d.SymFromAddrFn(GetCurrentProcess(), a, &disp, sym)) {
    char line[256] = "";
    if (d.SymLineFn) {
      IMAGEHLP_LINE64 il;
      std::memset(&il, 0, sizeof(il));
      il.SizeOfStruct = sizeof(il);
      DWORD ldisp = 0;
      if (d.SymLineFn(GetCurrentProcess(), a, &ldisp, &il) && il.FileName) {
        std::snprintf(line, sizeof(line), "  (%s:%lu)", il.FileName, static_cast<unsigned long>(il.LineNumber));
      }
    }
    Write("  #%02d %s+0x%llx  %s+0x%llx%s\n", index, base, static_cast<unsigned long long>(a - modbase),
          sym->Name, static_cast<unsigned long long>(disp), line);
  } else {
    Write("  #%02d %s+0x%llx  (no symbol)\n", index, base, static_cast<unsigned long long>(a - modbase));
  }
}

inline void WriteStack(const char* title) {
  Write("\n=== %s (thread %lu, pid %lu) ===\n", title, static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long>(GetCurrentProcessId()));
  void* frames[60];
  const USHORT n = CaptureStackBackTrace(0, 60, frames, nullptr);
  InitSymbols();
  for (USHORT i = 0; i < n; ++i) {
    WriteFrame(static_cast<int>(i), frames[i]);
  }
  Write("=== end of report ===\n");
}

// ---- What were the other threads doing? -----------------------------------
//
// The addresses of each other thread's stack are collected with the threads
// frozen for a very short time (no memory is allocated and no symbols are
// looked up while they are frozen, so nothing can get stuck). The threads are
// let go again and only then are the addresses turned into names.

constexpr int kMaxOtherThreads = 160;
constexpr int kMaxOtherFrames = 40;

struct OtherThread {
  DWORD id;
  char name[64];
  int count;
  void* frames[kMaxOtherFrames];
};

inline OtherThread* OtherThreads() {
  static OtherThread list[kMaxOtherThreads];
  return list;
}

inline int UnwindContext(CONTEXT* c, void** out, int max) {
#if defined(_M_X64) || defined(__x86_64__)
  int n = 0;
  while (n < max) {
    const DWORD64 pc = c->Rip;
    if (pc == 0) {
      break;
    }
    out[n++] = reinterpret_cast<void*>(pc);
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &image_base, nullptr);
    if (!fn) {
      // A function without unwind data: the return address is on top of the stack.
      DWORD64 ret = 0;
      SIZE_T got = 0;
      if (c->Rsp == 0 ||
          !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(c->Rsp), &ret, sizeof(ret), &got) ||
          got != sizeof(ret)) {
        break;
      }
      c->Rip = ret;
      c->Rsp += 8;
    } else {
      PVOID handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, fn, c, &handler_data, &establisher, nullptr);
    }
    if (c->Rsp == 0 || c->Rip == 0) {
      break;
    }
  }
  return n;
#else
  (void)c;
  (void)out;
  (void)max;
  return 0;
#endif
}

inline int CaptureOtherThreads() {
  OtherThread* list = OtherThreads();
  static HANDLE handles[kMaxOtherThreads];
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  int n = 0;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return 0;
  }
  // Freeze every other thread of this process.
  THREADENTRY32 te;
  std::memset(&te, 0, sizeof(te));
  te.dwSize = sizeof(te);
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
        continue;
      }
      if (n >= kMaxOtherThreads) {
        break;
      }
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                            te.th32ThreadID);
      if (!h) {
        continue;
      }
      if (SuspendThread(h) == static_cast<DWORD>(-1)) {
        CloseHandle(h);
        continue;
      }
      list[n].id = te.th32ThreadID;
      list[n].name[0] = '\0';
      list[n].count = 0;
      handles[n] = h;
      ++n;
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);

  // Read each frozen thread's stack addresses.
  for (int i = 0; i < n; ++i) {
    CONTEXT ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(handles[i], &ctx)) {
      list[i].count = UnwindContext(&ctx, list[i].frames, kMaxOtherFrames);
    }
  }

  // Let them all go again.
  for (int i = 0; i < n; ++i) {
    ResumeThread(handles[i]);
  }

  // Thread names (only possible on newer Windows 10 builds).
  typedef HRESULT(WINAPI * GetThreadDescriptionFn)(HANDLE, PWSTR*);
  GetThreadDescriptionFn get_desc = nullptr;
  HMODULE k = GetModuleHandleA("kernelbase.dll");
  if (k) {
    get_desc = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(k, "GetThreadDescription"));
  }
  for (int i = 0; i < n; ++i) {
    if (get_desc) {
      PWSTR w = nullptr;
      if (SUCCEEDED(get_desc(handles[i], &w)) && w) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, list[i].name, static_cast<int>(sizeof(list[i].name)) - 1, nullptr,
                            nullptr);
        list[i].name[sizeof(list[i].name) - 1] = '\0';
        LocalFree(w);
      }
    }
    CloseHandle(handles[i]);
  }
  return n;
}

inline bool FrameIsInExe(void* addr) {
  HMODULE hm = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(addr), &hm) &&
      hm) {
    return hm == GetModuleHandleA(nullptr);
  }
  return false;
}

// Threads that were running the game's own code get a full stack. All others
// (Windows, graphics and audio helper threads) only get one summary line.
inline void WriteOtherThreads(int n) {
  OtherThread* list = OtherThreads();
  Write("\n=== what the %d other threads were doing at the moment of the crash ===\n", n);
  int game_threads = 0;
  for (int i = 0; i < n; ++i) {
    bool is_game = false;
    for (int f = 0; f < list[i].count; ++f) {
      if (FrameIsInExe(list[i].frames[f])) {
        is_game = true;
        break;
      }
    }
    if (!is_game) {
      continue;
    }
    ++game_threads;
    Write("--- GAME thread %lu name='%s' (%d frames) ---\n", static_cast<unsigned long>(list[i].id),
          list[i].name[0] ? list[i].name : "(none)", list[i].count);
    for (int f = 0; f < list[i].count; ++f) {
      WriteFrame(f, list[i].frames[f]);
    }
  }
  Write("--- %d of the %d other threads were running game code; the rest are helper threads: ---\n", game_threads, n);
  for (int i = 0; i < n; ++i) {
    if (list[i].name[0]) {
      Write("    helper thread %lu name='%s'\n", static_cast<unsigned long>(list[i].id), list[i].name);
    }
  }
  Write("=== end of other threads ===\n");
}

inline void AbortHandler(int) {
  WriteStack("abort() was called");
}

inline void TerminateHandler() {
  Write("\nstd::terminate was called\n");
  try {
    std::exception_ptr e = std::current_exception();
    if (e) {
      std::rethrow_exception(e);
    } else {
      Write("(no active C++ exception)\n");
    }
  } catch (const std::exception& ex) {
    Write("uncaught C++ exception: %s\n", ex.what());
  } catch (...) {
    Write("uncaught C++ exception of an unknown type\n");
  }
  WriteStack("terminate");
  std::abort();
}

inline void InvalidParameterHandler(const wchar_t* expr, const wchar_t* func, const wchar_t* file,
                                    unsigned int line, uintptr_t) {
  Write("\nC runtime invalid parameter: function=%ls file=%ls line=%u expression=%ls\n", func ? func : L"?",
        file ? file : L"?", line, expr ? expr : L"?");
  WriteStack("invalid parameter");
  std::abort();
}

inline void PurecallHandler() {
  WriteStack("pure virtual function called");
  std::abort();
}

inline LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
  EnterCriticalSection(&ReportLock());
  // First thing: note what every other thread is doing, before anything slow.
  const int other_count = CaptureOtherThreads();
  if (ep && ep->ExceptionRecord) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    Write("\nUnhandled Windows exception 0x%08lX at address %p\n", static_cast<unsigned long>(r->ExceptionCode),
          r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
      Write("  access violation: %s address %p\n", r->ExceptionInformation[0] == 0   ? "reading"
                                                   : r->ExceptionInformation[0] == 1 ? "writing"
                                                                                     : "executing",
            reinterpret_cast<void*>(r->ExceptionInformation[1]));
    }
    InitSymbols();
    WriteFrame(0, r->ExceptionAddress);
  }
  WriteStack("unhandled exception");
  WriteOtherThreads(other_count);
  LeaveCriticalSection(&ReportLock());
  return EXCEPTION_CONTINUE_SEARCH;
}

inline void Install() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  ReportLock();  // make sure the lock exists before any crash can happen
  char path[MAX_PATH] = {0};
  const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    char* slash = std::strrchr(path, '\\');
    if (slash) {
      *(slash + 1) = '\0';
    }
    if (std::strlen(path) + 20 < sizeof(path)) {
      std::strcat(path, "juiced2_crash.txt");
      LogHandle() = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    }
  }
  std::signal(SIGABRT, AbortHandler);
  std::set_terminate(TerminateHandler);
  _set_invalid_parameter_handler(InvalidParameterHandler);
  _set_purecall_handler(PurecallHandler);
  SetUnhandledExceptionFilter(UnhandledFilter);
  SYSTEMTIME st;
  GetLocalTime(&st);
  Write("--- crash log armed %04d-%02d-%02d %02d:%02d:%02d (pid %lu) ---\n", st.wYear, st.wMonth, st.wDay, st.wHour,
        st.wMinute, st.wSecond, static_cast<unsigned long>(GetCurrentProcessId()));
}

}  // namespace juiced2_crash

inline void Juiced2InstallCrashLog() {
  juiced2_crash::Install();
}

#else

inline void Juiced2InstallCrashLog() {}

#endif