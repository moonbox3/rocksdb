//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
#include "port/stack_trace.h"

#if !(defined(ROCKSDB_BACKTRACE) || defined(OS_MACOSX)) || defined(CYGWIN) || \
    defined(OS_SOLARIS) || defined(OS_WIN)

// noop

namespace ROCKSDB_NAMESPACE {
namespace port {
void InstallStackTraceHandler() {}
void PrintStack(int /*first_frames_to_skip*/) {}
void PrintAndFreeStack(void* /*callstack*/, int /*num_frames*/) {}
void* SaveStack(int* /*num_frames*/, int /*first_frames_to_skip*/) {
  return nullptr;
}
}  // namespace port
}  // namespace ROCKSDB_NAMESPACE

#else

#include <cxxabi.h>
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef OS_FREEBSD
#include <sys/sysctl.h>
#endif  // OS_FREEBSD
#ifdef OS_LINUX
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#if __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 30)
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#endif  // GLIBC version
#endif  // OS_LINUX

#include "port/lang.h"

namespace ROCKSDB_NAMESPACE {
namespace port {

namespace {

#if defined(OS_LINUX) || defined(OS_FREEBSD) || defined(OS_GNU_KFREEBSD)
const char* GetExecutableName() {
  static char name[1024];

#if !defined(OS_FREEBSD)
  char link[1024];
  snprintf(link, sizeof(link), "/proc/%d/exe", getpid());
  auto read = readlink(link, name, sizeof(name) - 1);
  if (-1 == read) {
    return nullptr;
  } else {
    name[read] = 0;
    return name;
  }
#else
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  size_t namesz = sizeof(name);

  auto ret = sysctl(mib, 4, name, &namesz, nullptr, 0);
  if (-1 == ret) {
    return nullptr;
  } else {
    return name;
  }
#endif
}

void PrintStackTraceLine(const char* symbol, void* frame) {
  static const char* executable = GetExecutableName();
  if (symbol) {
    fprintf(stderr, "%s ", symbol);
  }
  if (executable) {
    // out source to addr2line, for the address translation
    const int kLineMax = 256;
    char cmd[kLineMax];
    snprintf(cmd, kLineMax, "addr2line %p -e %s -f -C 2>&1", frame, executable);
    auto f = popen(cmd, "r");
    if (f) {
      char line[kLineMax];
      while (fgets(line, sizeof(line), f)) {
        line[strlen(line) - 1] = 0;  // remove newline
        fprintf(stderr, "%s\t", line);
      }
      pclose(f);
    }
  } else {
    fprintf(stderr, " %p", frame);
  }

  fprintf(stderr, "\n");
}
#elif defined(OS_MACOSX)

void PrintStackTraceLine(const char* symbol, void* frame) {
  static int pid = getpid();
  // out source to atos, for the address translation
  const int kLineMax = 256;
  char cmd[kLineMax];
  snprintf(cmd, kLineMax, "xcrun atos %p -p %d  2>&1", frame, pid);
  auto f = popen(cmd, "r");
  if (f) {
    char line[kLineMax];
    while (fgets(line, sizeof(line), f)) {
      line[strlen(line) - 1] = 0;  // remove newline
      fprintf(stderr, "%s\t", line);
    }
    pclose(f);
  } else if (symbol) {
    fprintf(stderr, "%s ", symbol);
  }

  fprintf(stderr, "\n");
}

#endif

}  // namespace

void PrintStack(void* frames[], int num_frames) {
  auto symbols = backtrace_symbols(frames, num_frames);

  for (int i = 0; i < num_frames; ++i) {
    fprintf(stderr, "#%-2d  ", i);
    PrintStackTraceLine((symbols != nullptr) ? symbols[i] : nullptr, frames[i]);
  }
  free(symbols);
}

void PrintStack(int first_frames_to_skip) {
#if defined(ROCKSDB_DLL) && defined(OS_LINUX)
  // LIB_MODE=shared build produces mediocre information from the above
  // backtrace+addr2line stack trace method. Try to use GDB in that case, but
  // only on Linux where we know how to attach to a particular thread.
  bool linux_dll = true;
#else
  bool linux_dll = false;
#endif
  // Also support invoking interactive debugger on stack trace, with this
  // envvar set to non-empty
  char* debug_env = getenv("ROCKSDB_DEBUG");
  bool debug = debug_env != nullptr && strlen(debug_env) > 0;

  if (linux_dll || debug) {
    // Allow ouside debugger to attach, even with Yama security restrictions
#ifdef PR_SET_PTRACER_ANY
    (void)prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
    // Try to invoke GDB, either for stack trace or debugging.
    long long attach_id = getpid();

    // `gdb -p PID` seems to always attach to main thread, but `gdb -p TID`
    // seems to be able to attach to a particular thread in a process, which
    // makes sense as the main thread TID == PID of the process.
    // But I haven't found that gdb capability documented anywhere, so leave
    // a back door to attach to main thread.
#ifdef OS_LINUX
    if (getenv("ROCKSDB_DEBUG_USE_PID") == nullptr) {
      attach_id = gettid();
    }
#endif
    char attach_id_str[20];
    snprintf(attach_id_str, sizeof(attach_id_str), "%lld", attach_id);
    pid_t child_pid = fork();
    if (child_pid == 0) {
      // child process
      if (debug) {
        fprintf(stderr, "Invoking GDB for debugging (ROCKSDB_DEBUG=%s)...\n",
                debug_env);
        execlp(/*cmd in PATH*/ "gdb", /*arg0*/ "gdb", "-p", attach_id_str,
               (char*)nullptr);
        return;
      } else {
        fprintf(stderr, "Invoking GDB for stack trace...\n");

        // Skip top ~4 frames here in PrintStack
        // See https://stackoverflow.com/q/40991943/454544
        auto bt_in_gdb =
            "frame apply level 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 "
            "21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 "
            "42 43 44 -q frame";
        // Redirect child stdout to original stderr
        dup2(2, 1);
        // No child stdin (don't use pager)
        close(0);
        // -n : Loading config files can apparently cause failures with the
        // other options here.
        // -batch : non-interactive; suppress banners as much as possible
        execlp(/*cmd in PATH*/ "gdb", /*arg0*/ "gdb", "-n", "-batch", "-p",
               attach_id_str, "-ex", bt_in_gdb, (char*)nullptr);
        return;
      }
    } else {
      // parent process; wait for child
      int wstatus;
      waitpid(child_pid, &wstatus, 0);
      if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == EXIT_SUCCESS) {
        // Good
        return;
      }
    }
    fprintf(stderr, "GDB failed; falling back on backtrace+addr2line...\n");
  }

  const int kMaxFrames = 100;
  void* frames[kMaxFrames];

  auto num_frames = backtrace(frames, kMaxFrames);
  PrintStack(&frames[first_frames_to_skip], num_frames - first_frames_to_skip);
}

void PrintAndFreeStack(void* callstack, int num_frames) {
  PrintStack(static_cast<void**>(callstack), num_frames);
  free(callstack);
}

void* SaveStack(int* num_frames, int first_frames_to_skip) {
  const int kMaxFrames = 100;
  void* frames[kMaxFrames];

  auto count = backtrace(frames, kMaxFrames);
  *num_frames = count - first_frames_to_skip;
  void* callstack = malloc(sizeof(void*) * *num_frames);
  memcpy(callstack, &frames[first_frames_to_skip], sizeof(void*) * *num_frames);
  return callstack;
}

static void StackTraceHandler(int sig) {
  // reset to default handler
  signal(sig, SIG_DFL);
  fprintf(stderr, "Received signal %d (%s)\n", sig, strsignal(sig));
  // skip the top three signal handler related frames
  PrintStack(3);

  // Efforts to fix or suppress TSAN warnings "signal-unsafe call inside of
  // a signal" have failed, so just warn the user about them.
#ifdef __SANITIZE_THREAD__
  fprintf(stderr,
          "==> NOTE: any above warnings about \"signal-unsafe call\" are\n"
          "==> ignorable, as they are expected when generating a stack\n"
          "==> trace because of a signal under TSAN. Consider why the\n"
          "==> signal was generated to begin with, and the stack trace\n"
          "==> in the TSAN warning can be useful for that. (The stack\n"
          "==> trace printed by the signal handler is likely obscured\n"
          "==> by TSAN output.)\n");
#endif

  // re-signal to default handler (so we still get core dump if needed...)
  raise(sig);
}

void InstallStackTraceHandler() {
  // just use the plain old signal as it's simple and sufficient
  // for this use case
  signal(SIGILL, StackTraceHandler);
  signal(SIGSEGV, StackTraceHandler);
  signal(SIGBUS, StackTraceHandler);
  signal(SIGABRT, StackTraceHandler);
  // Allow ouside debugger to attach, even with Yama security restrictions.
  // This is needed even outside of PrintStack() so that external mechanisms
  // can dump stacks if they suspect that a test has hung.
#ifdef PR_SET_PTRACER_ANY
  (void)prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
}

}  // namespace port
}  // namespace ROCKSDB_NAMESPACE

#endif
