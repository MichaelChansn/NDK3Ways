// Copyright (c) 2010 Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef CLIENT_LINUX_HANDLER_EXCEPTION_HANDLER_H_
#define CLIENT_LINUX_HANDLER_EXCEPTION_HANDLER_H_

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ucontext.h>

#include <string>

#include "scoped_ptr.h"

namespace google_breakpad {

#define HANDLE_EINTR(x) ({ \
  __typeof__(x) eintr_wrapper_result; \
  do { \
    eintr_wrapper_result = (x); \
  } while (eintr_wrapper_result == -1 && errno == EINTR); \
  eintr_wrapper_result; \
})

#ifdef HAS_GLOBAL_STRING
    typedef ::string google_breakpad_string;
#else
    using std::string;
    typedef std::string google_breakpad_string;
#endif

// ExceptionHandler
//
// ExceptionHandler can write a minidump file when an exception occurs,
// or when WriteMinidump() is called explicitly by your program.
//
// To have the exception handler write minidumps when an uncaught exception
// (crash) occurs, you should create an instance early in the execution
// of your program, and keep it around for the entire time you want to
// have crash handling active (typically, until shutdown).
// (NOTE): There should be only be one this kind of exception handler
// object per process.
//
// If you want to write minidumps without installing the exception handler,
// you can create an ExceptionHandler with install_handler set to false,
// then call WriteMinidump.  You can also use this technique if you want to
// use different minidump callbacks for different call sites.
//
// In either case, a callback function is called when a minidump is written,
// which receives the full path or file descriptor of the minidump.  The
// caller can collect and write additional application state to that minidump,
// and launch an external crash-reporting application.
//
// Caller should try to make the callbacks as crash-friendly as possible,
// it should avoid use heap memory allocation as much as possible.

    class ExceptionHandler {
    public:
        // A callback function
        // type: 0: dump begin, 1: dump end, 2: dump give up
        typedef bool (*DumpCallback)(int type, const char *path, bool succeeded);


        // Creates a new ExceptionHandler instance to handle writing minidumps.
        // Before writing a minidump, the optional |filter| callback will be called.
        // Its return value determines whether or not Breakpad should write a
        // minidump.  The minidump content will be written to the file path or file
        // descriptor from |descriptor|, and the optional |callback| is called after
        // writing the dump file, as described above.
        // If install_handler is true, then a minidump will be written whenever
        // an unhandled exception occurs.  If it is false, minidumps will only
        // be written when WriteMinidump is called.
        ExceptionHandler(const string &directory, DumpCallback callback, bool install_handler);

        ~ExceptionHandler();

        // This structure is passed to minidump_writer.h:WriteMinidump via an opaque
        // blob. It shouldn't be needed in any user code.
        struct CrashContext {
            siginfo_t siginfo;
            pid_t tid;  // the crashing thread.
            struct ucontext context;
#if !defined(__ARM_EABI__) && !defined(__mips__)
            // #ifdef this out because FP state is not part of user ABI for Linux ARM.
            // In case of MIPS Linux FP state is already part of struct
            // ucontext so 'float_state' is not required.
            fpstate_t float_state;
#endif
        };

        // Report a crash signal from an SA_SIGINFO signal handler.
        bool HandleSignal(int sig, siginfo_t *info, void *uc);

    private:
        // Save the old signal handlers and install new ones.
        static bool InstallHandlersLocked();

        // Restore the old signal handlers.
        static void RestoreHandlersLocked();

        bool GenerateDump(CrashContext *context);

        void SendContinueSignalToChild();

        void WaitForContinueSignal();

        static void SignalHandler(int sig, siginfo_t *info, void *uc);

        static int ThreadEntry(void *arg);

        bool DoDump(pid_t crashing_process, const void *context,
                    size_t context_size, const char *path);

        bool CheckHandlerValid();

        const DumpCallback callback_;

        // The directory where the dump should be generated.
        string directory_;
        // The full path to the generated dump.
        string path_;
        // The C string of |path_|. Precomputed so it can be access from a compromised
        // context.
        const char *c_path_;

//  scoped_ptr<CrashGenerationClient> crash_generation_client_;

        // We need to explicitly enable ptrace of parent processes on some
        // kernels, but we need to know the PID of the cloned process before we
        // can do this. We create a pipe which we can use to block the
        // cloned process after creating it, until we have explicitly enabled
        // ptrace. This is used to store the file descriptors for the pipe
        int fdes[2];

        // Callers can add extra info about mappings for cases where the
        // dumper code cannot extract enough information from /proc/<pid>/maps.
//  MappingList mapping_list_;

        // Callers can request additional memory regions to be included in
        // the dump.
//  AppMemoryList app_memory_list_;
    };

}  // namespace google_breakpad

#endif  // CLIENT_LINUX_HANDLER_EXCEPTION_HANDLER_H_
