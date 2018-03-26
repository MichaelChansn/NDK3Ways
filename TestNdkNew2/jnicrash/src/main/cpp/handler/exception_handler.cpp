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

// The ExceptionHandler object installs signal handlers for a number of
// signals. We rely on the signal handler running on the thread which crashed
// in order to identify it. This is true of the synchronous signals (SEGV etc),
// but not true of ABRT. Thus, if you send ABRT to yourself in a program which
// uses ExceptionHandler, you need to use tgkill to direct it to the current
// thread.
//
// The signal flow looks like this:
//
//   SignalHandler (uses a global stack of ExceptionHandler objects to find
//        |         one to handle the signal. If the first rejects it, try
//        |         the second etc...)
//        V
//   HandleSignal ----------------------------| (clones a new process which
//        |                                   |  shares an address space with
//   (wait for cloned                         |  the crashed process. This
//     process)                               |  allows us to ptrace the crashed
//        |                                   |  process)
//        V                                   V
//   (set signal handler to             ThreadEntry (static function to bounce
//    SIG_DFL and rethrow,                    |      back into the object)
//    killing the crashed                     |
//    process)                                V
//                                          DoDump  (writes minidump)
//                                            |
//                                            V
//                                         sys_exit
//

// This code is a little fragmented. Different functions of the ExceptionHandler
// class run in a number of different contexts. Some of them run in a normal
// context and are easy to code, others run in a compromised context and the
// restrictions at the top of minidump_writer.cc apply: no libc and use the
// alternative malloc. Each function should have comment above it detailing the
// context which it runs in.

#include "exception_handler.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "signal.h"
#include "ucontext.h"
#include <ucontext.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "memory.h"
#include <android/log.h>

extern "C" {
#include "../debuggerd/tombstone.h"
}
#if defined(__ANDROID__)
#include "linux/sched.h"
#endif

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif

#define TAG "jnicrash"

// A wrapper for the tgkill syscall: send a signal to a specific thread.
static int tgkill(pid_t tgid, pid_t tid, int sig) {
    return syscall(__NR_tgkill, tgid, tid, sig);
    return 0;
}

namespace google_breakpad {

    namespace {
// The list of signals which we consider to be crashes. The default action for
// all these signals must be Core (see man 7 signal) because we rethrow the
// signal after handling it and expect that it'll be fatal.
// SIGSEGV 无效的物理地址, SIGILL Illegal instruction, SIGABRT Abnormal termination
// SIGFPE Floating-point exception, SIGBUS Bus error
        const int kExceptionSignals[] = {
                SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS
        };
        const int kNumHandledSignals =
                sizeof(kExceptionSignals) / sizeof(kExceptionSignals[0]);
        struct sigaction old_handlers[kNumHandledSignals];
        bool handlers_installed = false;

// InstallAlternateStackLocked will store the newly installed stack in new_stack
// and (if it exists) the previously installed stack in old_stack.
        stack_t old_stack;
        stack_t new_stack;
        bool stack_installed = false;

        const string FLAG_FILE = "flagfile";

// Create an alternative stack to run the signal handlers on. This is done since
// the signal might have been caused by a stack overflow.
// Runs before crashing: normal context.
        void InstallAlternateStackLocked() {
            if (stack_installed)
                return;

            memset(&old_stack, 0, sizeof(old_stack));
            memset(&new_stack, 0, sizeof(new_stack));

            // SIGSTKSZ may be too small to prevent the signal handlers from overrunning
            // the alternative stack. Ensure that the size of the alternative stack is
            // large enough.
            static const unsigned kSigStackSize = std::max(16384, SIGSTKSZ);

            // Only set an alternative stack if there isn't already one, or if the current
            // one is too small.
            if (sys_sigaltstack(NULL, &old_stack) == -1 || !old_stack.ss_sp ||
                old_stack.ss_size < kSigStackSize) {
                new_stack.ss_sp = calloc(1, kSigStackSize);
                new_stack.ss_size = kSigStackSize;

                if (sys_sigaltstack(&new_stack, NULL) == -1) {
                    free(new_stack.ss_sp);
                    return;
                }
                stack_installed = true;
            }
        }

// Runs before crashing: normal context.
        void RestoreAlternateStackLocked() {
            if (!stack_installed)
                return;

            stack_t current_stack;
            if (sys_sigaltstack(NULL, &current_stack) == -1)
                return;

            // Only restore the old_stack if the current alternative stack is the one
            // installed by the call to InstallAlternateStackLocked.
            if (current_stack.ss_sp == new_stack.ss_sp) {
                if (old_stack.ss_sp) {
                    if (sys_sigaltstack(&old_stack, NULL) == -1)
                        return;
                } else {
                    stack_t disable_stack;
                    disable_stack.ss_flags = SS_DISABLE;
                    if (sys_sigaltstack(&disable_stack, NULL) == -1)
                        return;
                }
            }

            free(new_stack.ss_sp);
            stack_installed = false;
        }

        void InstallDefaultHandler(int sig) {
#if defined(__ANDROID__)
            // Android L+ expose signal and sigaction symbols that override the system
            // ones. There is a bug in these functions where a request to set the handler
            // to SIG_DFL is ignored. In that case, an infinite loop is entered as the
            // signal is repeatedly sent to breakpad's signal handler.
            // To work around this, directly call the system's sigaction.
            struct kernel_sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sys_sigemptyset(&sa.sa_mask);
            sa.sa_handler_ = SIG_DFL;
            sa.sa_flags = SA_RESTART;
            sys_rt_sigaction(sig, &sa, NULL, sizeof(kernel_sigset_t));
#else
            signal(sig, SIG_DFL);
#endif
        }

// The global exception handler stack. This is needed because there may exist
// multiple ExceptionHandler instances in a process. Each will have itself
// registered in this stack.
        std::vector<ExceptionHandler *> *g_handler_stack_ = NULL;
        pthread_mutex_t g_handler_stack_mutex_ = PTHREAD_MUTEX_INITIALIZER;

// sizeof(CrashContext) can be too big w.r.t the size of alternatate stack
// for SignalHandler(). Keep the crash context as a .bss field. Exception
// handlers are serialized by the |g_handler_stack_mutex_| and at most one at a
// time can use |g_crash_context_|.
        ExceptionHandler::CrashContext g_crash_context_;

    }  // namespace

// Runs before crashing: normal context.
    ExceptionHandler::ExceptionHandler(const string &directory, DumpCallback callback,
                                       bool install_handler)
            : directory_(directory),
              callback_(callback) {
        pthread_mutex_lock(&g_handler_stack_mutex_);

        // Pre-fault the crash context struct. This is to avoid failing due to OOM
        // if handling an exception when the process ran out of virtual memory.
        memset(&g_crash_context_, 0, sizeof(g_crash_context_));

        if (!g_handler_stack_)
            g_handler_stack_ = new std::vector < ExceptionHandler * >;
        if (install_handler) {
            InstallAlternateStackLocked();
            InstallHandlersLocked();
        }
        g_handler_stack_->push_back(this);
        pthread_mutex_unlock(&g_handler_stack_mutex_);
    }

// Runs before crashing: normal context.
    ExceptionHandler::~ExceptionHandler() {
        pthread_mutex_lock(&g_handler_stack_mutex_);
        std::vector<ExceptionHandler *>::iterator handler =
                std::find(g_handler_stack_->begin(), g_handler_stack_->end(), this);
        g_handler_stack_->erase(handler);
        if (g_handler_stack_->empty()) {
            delete g_handler_stack_;
            g_handler_stack_ = NULL;
            RestoreAlternateStackLocked();
            RestoreHandlersLocked();
        }
        pthread_mutex_unlock(&g_handler_stack_mutex_);
    }

// Runs before crashing: normal context.
// static
    bool ExceptionHandler::InstallHandlersLocked() {
        if (handlers_installed)
            return false;

        // Fail if unable to store all the old handlers.
        for (int i = 0; i < kNumHandledSignals; ++i) {
            if (sigaction(kExceptionSignals[i], NULL, &old_handlers[i]) == -1)
                return false;
        }

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sigemptyset(&sa.sa_mask);

        // Mask all exception signals when we're handling one of them.
        for (int i = 0; i < kNumHandledSignals; ++i)
            sigaddset(&sa.sa_mask, kExceptionSignals[i]);

        sa.sa_sigaction = SignalHandler;
        sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
        for (int i = 0; i < kNumHandledSignals; ++i) {
            if (sigaction(kExceptionSignals[i], &sa, NULL) == -1) {
                // At this point it is impractical to back out changes, and so failure to
                // install a signal is intentionally ignored.
            }
        }
        handlers_installed = true;
        return true;
    }

// This function runs in a compromised context: see the top of the file.
// Runs on the crashing thread.
// static
    void ExceptionHandler::RestoreHandlersLocked() {
        if (!handlers_installed)
            return;
        for (int i = 0; i < kNumHandledSignals; ++i) {
            if (sigaction(kExceptionSignals[i], &old_handlers[i], NULL) == -1) {
                InstallDefaultHandler(kExceptionSignals[i]);
            }
        }
        handlers_installed = false;
    }

// This function runs in a compromised context: see the top of the file.
// Runs on the crashing thread.
// static
    void ExceptionHandler::SignalHandler(int sig, siginfo_t *info, void *uc) {
        // All the exception signals are blocked at this point.
        // for statistic
        pthread_mutex_lock(&g_handler_stack_mutex_);

        // Sometimes, Breakpad runs inside a process where some other buggy code
        // saves and restores signal handlers temporarily with 'signal'
        // instead of 'sigaction'. This loses the SA_SIGINFO flag associated
        // with this function. As a consequence, the values of 'info' and 'uc'
        // become totally bogus, generally inducing a crash.
        //
        // The following code tries to detect this case. When it does, it
        // resets the signal handlers with sigaction + SA_SIGINFO and returns.
        // This forces the signal to be thrown again, but this time the kernel
        // will call the function withe th right arguments.
        struct sigaction cur_handler;
        if (sigaction(sig, NULL, &cur_handler) == 0 &&
            (cur_handler.sa_flags & SA_SIGINFO) == 0) {
            // Reset signal handler with the right flags.
            sigemptyset(&cur_handler.sa_mask);
            sigaddset(&cur_handler.sa_mask, sig);

            cur_handler.sa_sigaction = SignalHandler;
            cur_handler.sa_flags = SA_ONSTACK | SA_SIGINFO;

            if (sigaction(sig, &cur_handler, NULL) == -1) {
                // When resetting the handler fails, try to reset the
                // default one to avoid an infinite loop here.
                InstallDefaultHandler(sig);
            }
            pthread_mutex_unlock(&g_handler_stack_mutex_);
            return;
        }

        bool handled = false;
        for (int i = g_handler_stack_->size() - 1; !handled && i >= 0; --i) {
            handled = (*g_handler_stack_)[i]->HandleSignal(sig, info, uc);
        }

        // Upon returning from this signal handler, sig will become unmasked and then
        // it will be retriggered. If one of the ExceptionHandlers handled it
        // successfully, restore the default handler. Otherwise, restore the
        // previously installed handler. Then, when the signal is retriggered, it will
        // be delivered to the appropriate handler.

        if (handled) {
            InstallDefaultHandler(sig);
        } else {
            RestoreHandlersLocked();
        }

        pthread_mutex_unlock(&g_handler_stack_mutex_);

        // info->si_code <= 0 iff SI_FROMUSER (SI_FROMKERNEL otherwise).
        if (info->si_code <= 0 || sig == SIGABRT) {
            // This signal was triggered by somebody sending us the signal with kill().
            // In order to retrigger it, we have to queue a new signal by calling
            // kill() ourselves.  The special case (si_pid == 0 && sig == SIGABRT) is
            // due to the kernel sending a SIGABRT from a user request via SysRQ.
            if (tgkill(getpid(), syscall(__NR_gettid), sig) < 0) {
                // If we failed to kill ourselves (e.g. because a sandbox disallows us
                // to do so), we instead resort to terminating our process. This will
                // result in an incorrect exit code.
                _exit(1);
            }
        } else {
            // This was a synchronous signal triggered by a hard fault (e.g. SIGSEGV).
            // No need to reissue the signal. It will automatically trigger again,
            // when we return from the signal handler.
        }
    }

    struct ThreadArgument {
        pid_t pid;  // the crashing process
        ExceptionHandler *handler;
        const void *context;  // a CrashContext structure
        size_t context_size;
        const char *path;
    };

// This is the entry function for the cloned process. We are in a compromised
// context here: see the top of the file.
// static
    int ExceptionHandler::ThreadEntry(void *arg) {
        const ThreadArgument *thread_arg = reinterpret_cast<ThreadArgument *>(arg);

        // Block here until the crashing process unblocks us when
        // we're allowed to use ptrace
        thread_arg->handler->WaitForContinueSignal();

        return thread_arg->handler->DoDump(thread_arg->pid, thread_arg->context,
                                           thread_arg->context_size, thread_arg->path) == false;
    }

    bool ExceptionHandler::CheckHandlerValid() {
        string file_path = directory_ + "/" + FLAG_FILE;
        FILE* fp = fopen(file_path.c_str(), "r");
        if (fp != NULL) {
            return false;
        } else {
            fp = fopen(file_path.c_str(), "w");
            fclose(fp);
        }

        return true;
    }

    int signal;

// This function runs in a compromised context: see the top of the file.
// Runs on the crashing thread.
    bool ExceptionHandler::HandleSignal(int sig, siginfo_t *info, void *uc) {

        if (!CheckHandlerValid()) {
            if (callback_)
                callback_(2, c_path_, 0);
            return false;
        }

        if (callback_)
            callback_(0, c_path_, 0);
        signal = sig;
        // Allow ourselves to be dumped if the signal is trusted.
        bool signal_trusted = info->si_code > 0;
        bool signal_pid_trusted = info->si_code == SI_USER ||
                                  info->si_code == SI_TKILL;
        if (signal_trusted || (signal_pid_trusted && info->si_pid == getpid())) {
            sys_prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
        }

        // Fill in all the holes in the struct to make Valgrind happy.
        memset(&g_crash_context_, 0, sizeof(g_crash_context_));
        memcpy(&g_crash_context_.siginfo, info, sizeof(siginfo_t));
        memcpy(&g_crash_context_.context, uc, sizeof(struct ucontext));
#if defined(__aarch64__)
        struct ucontext *uc_ptr = (struct ucontext*)uc;
        struct fpsimd_context *fp_ptr =
            (struct fpsimd_context*)&uc_ptr->uc_mcontext.__reserved;
        if (fp_ptr->head.magic == FPSIMD_MAGIC) {
          memcpy(&g_crash_context_.float_state, fp_ptr,
                 sizeof(g_crash_context_.float_state));
        }
#elif !defined(__ARM_EABI__)  && !defined(__mips__)
        // FP state is not part of user ABI on ARM Linux.
        // In case of MIPS Linux FP state is already part of struct ucontext
        // and 'float_state' is not a member of CrashContext.
        struct ucontext *uc_ptr = (struct ucontext *) uc;
        if (uc_ptr->uc_mcontext.fpregs) {
            memcpy(&g_crash_context_.float_state, uc_ptr->uc_mcontext.fpregs,
                   sizeof(g_crash_context_.float_state));
        }
#endif
        g_crash_context_.tid = syscall(__NR_gettid);
        return GenerateDump(&g_crash_context_);
    }

    void my_memset(void *ip, char c, size_t len) {
        char *p = (char *) ip;
        while (len--)
            *p++ = c;
    }

// This function may run in a compromised context: see the top of the file.
    bool ExceptionHandler::GenerateDump(CrashContext *context) {
//  if (IsOutOfProcess())
//    return crash_generation_client_->RequestDump(context, sizeof(*context));

        // Allocating too much stack isn't a problem, and better to err on the side
        // of caution than smash it into random locations.
        static const unsigned kChildStackSize = 16000;
        PageAllocator allocator;
        uint8_t *stack = reinterpret_cast<uint8_t *>(allocator.Alloc(kChildStackSize));
        if (!stack)
            return false;
        // clone() needs the top-most address. (scrub just to be safe)
        stack += kChildStackSize;
        my_memset(stack - 16, 0, 16);

        path_.clear();
        time_t clock;
        time(&clock);
        struct tm tm_struct;
        localtime_r(&clock, &tm_struct);
        char time_string[20];
        strftime(time_string, sizeof(time_string), "%Y%m%d%H%M%S", &tm_struct);
        path_ = directory_ + "/" + time_string;
        c_path_ = path_.c_str();

        ThreadArgument thread_arg;
        thread_arg.handler = this;
        thread_arg.pid = getpid();
        thread_arg.context = context;
        thread_arg.context_size = sizeof(*context);
        thread_arg.path = c_path_;
        // We need to explicitly enable ptrace of parent processes on some
        // kernels, but we need to know the PID of the cloned process before we
        // can do this. Create a pipe here which we can use to block the
        // cloned process after creating it, until we have explicitly enabled ptrace
        if (sys_pipe(fdes) == -1) {
            // Creating the pipe failed. We'll log an error but carry on anyway,
            // as we'll probably still get a useful crash report. All that will happen
            // is the write() and read() calls will fail with EBADF

            // Ensure fdes[0] and fdes[1] are invalid file descriptors.
            fdes[0] = fdes[1] = -1;
        }
        const pid_t child = sys_clone(
                ThreadEntry, stack, CLONE_FILES | CLONE_FS | CLONE_UNTRACED,
                &thread_arg, NULL, NULL, NULL);
        if (child == -1) {
            sys_close(fdes[0]);
            sys_close(fdes[1]);
            return false;
        }

        // Allow the child to ptrace us
        sys_prctl(PR_SET_PTRACER, child, 0, 0, 0);
        SendContinueSignalToChild();
        int status;
        const int r = HANDLE_EINTR(sys_waitpid(child, &status, __WALL));

        sys_close(fdes[0]);
        sys_close(fdes[1]);

        if (r == -1) {
            __android_log_print(6, TAG, "generate fail");
        }

        bool success = r != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;

        // 删除标记文件
        string file_path = directory_ + "/" + FLAG_FILE;
        int result = remove(file_path.c_str());

        if (callback_)
            success = callback_(1, c_path_, success);
        __android_log_print(6, TAG, "finish");
        return false;
    }

// This function runs in a compromised context: see the top of the file.
    void ExceptionHandler::SendContinueSignalToChild() {
        static const char okToContinueMessage = 'a';
        int r;
        r = HANDLE_EINTR(sys_write(fdes[1], &okToContinueMessage, sizeof(char)));
        if (r == -1) {

        }
    }

// This function runs in a compromised context: see the top of the file.
// Runs on the cloned process.
    void ExceptionHandler::WaitForContinueSignal() {
        int r;
        char receivedMessage;
        r = HANDLE_EINTR(sys_read(fdes[0], &receivedMessage, sizeof(char)));
        if (r == -1) {
        }
    }


// This function runs in a compromised context: see the top of the file.
// Runs on the cloned process.
    bool ExceptionHandler::DoDump(pid_t crashing_process, const void *context,
                                  size_t context_size, const char *path) {
        const ExceptionHandler::CrashContext *crashContext = reinterpret_cast<const ExceptionHandler::CrashContext *>(context);

        return engrave_tombstone(crashing_process, crashContext->tid, signal, 0,
                                 &crashContext->context, path);
    }

// In order to making using EBP to calculate the desired value for ESP
// a valid operation, ensure that this function is compiled with a
// frame pointer using the following attribute. This attribute
// is supported on GCC but not on clang.
#if defined(__i386__) && defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-omit-frame-pointer")))
#endif

}  // namespace google_breakpad
