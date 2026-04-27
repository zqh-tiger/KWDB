// Copyright (c) 2022-present, Shanghai Yunxi Technology Co, Ltd. All rights reserved.
//
// This software (KWDB) is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
//          http://license.coscl.org.cn/MulanPSL2
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PSL v2 for more details.


#include <signal.h>
#include <execinfo.h>
#include <dirent.h>
#include <stdio.h>
#include <fcntl.h>
#include <ucontext.h>
#include <iostream>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <cerrno>
#include "cm_backtrace.h"
#include "cm_kwdb_context.h"
#include "kwdb_compatible.h"
#include "kwdb_type.h"
#include "lg_api.h"
#include "lt_rw_latch.h"
#include "lt_cond.h"

namespace kwdbts {
const char* kThreadBtName = "thread_backtrace";
k_char kThreadBtPath[FULL_FILE_NAME_MAX_LEN];
k_int32 kThreadBtFd = -1;
struct sigaction oldsa;

extern KString demangle(const char* symbol);

void DumpLatchInfo();

namespace {

char* AppendLiteral(char* dest, const char* end, const char* literal) {
  while (dest < end && *literal != '\0') {
    *dest++ = *literal++;
  }
  return dest;
}

char* AppendUnsignedDec(char* dest, const char* end, k_uint64 value) {
  char digits[32];
  k_size_t digit_count = 0;
  do {
    digits[digit_count++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0 && digit_count < sizeof(digits));

  while (digit_count > 0 && dest < end) {
    *dest++ = digits[--digit_count];
  }
  return dest;
}

char* AppendHex(char* dest, const char* end, uintptr_t value) {
  static const char kHexDigits[] = "0123456789abcdef";
  char digits[2 * sizeof(uintptr_t)];
  k_size_t digit_count = 0;
  do {
    digits[digit_count++] = kHexDigits[value & 0xf];
    value >>= 4;
  } while (value != 0 && digit_count < sizeof(digits));

  dest = AppendLiteral(dest, end, "0x");
  while (digit_count > 0 && dest < end) {
    *dest++ = digits[--digit_count];
  }
  return dest;
}

void WriteAll(k_int32 fd, const char* data, k_size_t size) {
  while (fd >= 0 && size > 0) {
    ssize_t written = write(fd, data, size);
    if (written > 0) {
      data += written;
      size -= static_cast<k_size_t>(written);
      continue;
    }
    if (written == -1 && errno == EINTR) {
      continue;
    }
    break;
  }
}

bool ExtractThreadContext(const void* secret, uintptr_t* pc, uintptr_t* sp) {
  if (secret == nullptr || pc == nullptr || sp == nullptr) {
    return false;
  }

  const auto* ctx = static_cast<const ucontext_t*>(secret);
#if defined(__x86_64__)
  *pc = static_cast<uintptr_t>(ctx->uc_mcontext.gregs[REG_RIP]);
  *sp = static_cast<uintptr_t>(ctx->uc_mcontext.gregs[REG_RSP]);
  return true;
#elif defined(__aarch64__)
  *pc = static_cast<uintptr_t>(ctx->uc_mcontext.pc);
  *sp = static_cast<uintptr_t>(ctx->uc_mcontext.sp);
  return true;
#else
  (void)ctx;
  return false;
#endif
}

k_size_t BuildThreadSignalMessage(char* buffer, k_size_t size, int signr, const void* secret) {
  char* current = buffer;
  char* end = buffer + (size == 0 ? 0 : size - 1);
  uintptr_t pc = 0;
  uintptr_t sp = 0;

  current = AppendLiteral(current, end, "thread_signal=");
  current = AppendUnsignedDec(current, end, static_cast<k_uint64>(signr));
  current = AppendLiteral(current, end, " pid=");
  current = AppendUnsignedDec(current, end, static_cast<k_uint64>(getpid()));
  current = AppendLiteral(current, end, " tid=");
  current = AppendUnsignedDec(current, end, static_cast<k_uint64>(gettid()));
  if (ExtractThreadContext(secret, &pc, &sp)) {
    current = AppendLiteral(current, end, " pc=");
    current = AppendHex(current, end, pc);
    current = AppendLiteral(current, end, " sp=");
    current = AppendHex(current, end, sp);
  }
  current = AppendLiteral(current, end, "\n");
  *current = '\0';
  return static_cast<k_size_t>(current - buffer);
}

k_int32 OpenThreadBacktraceFd(const char* path) {
  return open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
}

void WriteThreadSignalRecord(k_int32 fd, int signr, const void* secret) {
  char buffer[256];
  k_size_t msg_len = BuildThreadSignalMessage(buffer, sizeof(buffer), signr, secret);
  WriteAll(fd, buffer, msg_len);
}

}  // namespace

// send thread backtrace to ostream.
void PrintThreadBacktrace(std::ostream& os) {
  const k_int32 MAX_FRAME_LEVEL = 128;
  void *array[MAX_FRAME_LEVEL];
  size_t size = backtrace(array, MAX_FRAME_LEVEL);
  char **symbols = backtrace_symbols(array, size);
  os << "\nThread 0x"<< std::hex << pthread_self() << std::dec << " pid=" << getpid() << 
     " tid=" << gettid() << std::endl;
  os << "backtrace: size:" << size << std::endl;
  for (size_t i = 0; i < size; i++) {
    os << "#" << i << " " << demangle(symbols[i]) << std::endl;
  }
  free(symbols);
}

// Generate thread stack backtrace file absolute path.
void GetThreadBtFilePath(char *threadBtPath, char* folder, char* nowTimeStamp) {
  if ((std::strlen(folder) + std::strlen(kThreadBtName) + std::strlen(nowTimeStamp) + 6) 
       < FULL_FILE_NAME_MAX_LEN) {
    snprintf(threadBtPath, FULL_FILE_NAME_MAX_LEN, "%s/%s.%s.txt", folder,
             kThreadBtName, nowTimeStamp);
  } else {
    snprintf(threadBtPath, FULL_FILE_NAME_MAX_LEN, "./%s",
             kThreadBtName);
  }
}

// Signal SIGUSR2 to thread by syscall.
int SignalThreadDump(pid_t pid, uid_t uid, pid_t tid) {
  // Similar to pthread_sigqueue(), but usable with a tid since we
  // don't have a pthread_t.
  siginfo_t info;
  sigval nullVal;
  memset(&info, 0, sizeof(info));
  info.si_signo = SIGUSR2;
  info.si_code = SI_QUEUE;
  info.si_pid = pid;
  info.si_uid = uid;
  info.si_value = nullVal;
  return syscall(SYS_rt_tgsigqueueinfo, pid, tid, SIGUSR2, &info);
}

// Send signal SIGUSR2 to all threads in /proc/<PID>/task.
bool DumpAllThreadBacktrace(char* folder, char* nowTimeStamp) {
  DIR *dir;
  struct dirent *entry;
  // dump latch info
  DumpLatchInfo();

  // dump all threads info 
  memset(kThreadBtPath, 0, sizeof(kThreadBtPath));
  GetThreadBtFilePath(kThreadBtPath, folder, nowTimeStamp);
  if (kThreadBtFd >= 0) {
    close(kThreadBtFd);
    kThreadBtFd = -1;
  }
  kThreadBtFd = OpenThreadBacktraceFd(kThreadBtPath);
  if (kThreadBtFd < 0) {
    return false;
  }

  // Get all thread tids.
  char task_dir[] = "/proc/self/task/";
  dir = opendir(task_dir);
  if (dir == NULL) {
    return false;
  }

  while ((entry = readdir(dir)) != NULL) {
    std::string full_path = task_dir;
    full_path += entry->d_name;
    struct stat file_stat{};
    if (stat(full_path.c_str(), &file_stat) != 0) {
      LOG_ERROR("stat[%s] failed", full_path.c_str());
      closedir(dir);
      return false;
    }
    if (S_ISDIR(file_stat.st_mode)) {
      if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') {
        usleep(20000);
        SignalThreadDump(getpid(), getuid(), strtoll(entry->d_name, nullptr, 10));
      }
    }
  }

  closedir(dir);
  return true;
}

// the callback function of SIGUSR2 for dump thread backtrace.
static void DumpThreadBacktrace(int signr, siginfo_t *info, void *secret) {
  (void)info;
  WriteThreadSignalRecord(kThreadBtFd, signr, secret);
}

// Register signal SIGUSR2 action function.
void RegisterBacktraceSignalHandler() {
  // Register SIGUSR2 for dump thread backtrace
  struct sigaction sa;
	sigfillset(&sa.sa_mask);
 	sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO;
 	sa.sa_sigaction = DumpThreadBacktrace;
 	sigaction(SIGUSR2, &sa, &oldsa);
}

#ifdef WITH_TESTS
void WriteThreadBacktraceRecordForTest(int fd, int signr, void* context) {
  WriteThreadSignalRecord(fd, signr, context);
}
#endif

// dump latchs stats
FILE* openNewFile(const char* file_prefix) {
  std::string dump_file_path = Logger::GetInstance().LogRealPath();
  if (dump_file_path.empty()) {
    dump_file_path = "./";
  }
  if (dump_file_path.back() != '/') {
    dump_file_path += "/";
  }

  // file ext name
  time_t now = time(0);
  tm* localTime = localtime(&now);
  char timestamp[20] = {0};
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H_%M_%S", localTime);
  std::string ext_name(timestamp);
  std::string file_name = dump_file_path + file_prefix + "." + ext_name + ".dmp";
  // open file
  FILE* fp = fopen(file_name.c_str(), "a+");
  if (fp == nullptr) {
    LOG_ERROR("open file: %s failed,error: %s", file_name.c_str(), strerror(errno));
    return nullptr;
  }
  return fp;
}
void DumpLatchInfo() {
  // 1. dump latch info
  FILE* fp = openNewFile("latch");
  if (fp == nullptr) {
    return;
  }
  debug_latch_print(fp);
  fclose(fp);

  // 2. dump rwlatch info
  FILE* fp1 = openNewFile("rw_latch");
  if (fp1 == nullptr) {
    return;
  }
  debug_rwlock_print(fp1);
  fclose(fp1);

  // 3. dump cond wait info
  FILE* fp2 = openNewFile("cond_wait");
  if (fp2 == nullptr) {
    return;
  }
  debug_condwait_print(fp2);
  fclose(fp2);
}


}  // namespace kwdbts
