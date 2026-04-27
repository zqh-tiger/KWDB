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
#include <cxxabi.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "cm_exception.h"
#include "cm_kwdb_context.h"
#include "kwdb_type.h"
#include "kwdb_compatible.h"


namespace kwdbts {

const char* kErrlogName = "errlog.log";
k_char kErrlogPath[FULL_FILE_NAME_MAX_LEN];

char kEmergencyBuf[512];
volatile sig_atomic_t kHandlingException = 0;
k_int32 kErrlogFd = -1;

#define EXCEPTION_SIGNAL_CNT (6)
static k_int32 kExceptionSignals[EXCEPTION_SIGNAL_CNT] = {
  SIGSEGV, SIGABRT, SIGBUS, SIGSYS, SIGFPE, SIGILL
};
static struct sigaction kOldSigactions[EXCEPTION_SIGNAL_CNT];

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

char* AppendSignedDec(char* dest, const char* end, k_int64 value) {
  if (value < 0) {
    if (dest < end) {
      *dest++ = '-';
    }
    return AppendUnsignedDec(dest, end, static_cast<k_uint64>(-(value + 1)) + 1);
  }
  return AppendUnsignedDec(dest, end, static_cast<k_uint64>(value));
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

k_size_t BuildExceptionMessage(char* buffer, k_size_t size, int sig, const siginfo_t* info) {
  char* current = buffer;
  char* end = buffer + (size == 0 ? 0 : size - 1);
  const k_int32 si_code = info == nullptr ? 0 : info->si_code;
  const uintptr_t signal_addr =
      reinterpret_cast<uintptr_t>(info == nullptr ? nullptr : info->si_addr);

  current = AppendLiteral(current, end, "signal=");
  current = AppendSignedDec(current, end, sig);
  current = AppendLiteral(current, end, " pid=");
  current = AppendUnsignedDec(current, end, static_cast<k_uint64>(getpid()));
  current = AppendLiteral(current, end, " tid=");
  current = AppendUnsignedDec(current, end, static_cast<k_uint64>(gettid()));
  current = AppendLiteral(current, end, " si_code=");
  current = AppendSignedDec(current, end, si_code);
  current = AppendLiteral(current, end, " si_addr=");
  current = AppendHex(current, end, signal_addr);
  current = AppendLiteral(current, end, "\n");
  *current = '\0';
  return static_cast<k_size_t>(current - buffer);
}

void RestorePreviousSignalAction(int sig) {
  for (k_int32 i = 0; i < EXCEPTION_SIGNAL_CNT; ++i) {
    if (sig == kExceptionSignals[i]) {
      sigaction(sig, &kOldSigactions[i], nullptr);
      return;
    }
  }
}

k_int32 OpenAppendFd(const char* path) {
  return open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

void WriteExceptionRecord(k_int32 fd, int sig, const siginfo_t* info) {
  k_size_t msg_len = BuildExceptionMessage(kEmergencyBuf, sizeof(kEmergencyBuf), sig, info);
  WriteAll(fd, kEmergencyBuf, msg_len);
}

}  // namespace

KString demangle(const char* symbol) {
  // extract symbol from - me.so(_ZN6kwdbts14PrintBacktraceERSo+0x34) [0xffff974b3530]
  static constexpr k_char OPEN = '(';
  const k_char* begin = nullptr;
  const k_char* end = nullptr;
  for (const k_char *j = symbol; *j; ++j) {
    if (*j == OPEN) {
      begin = j + 1;
    } else if (*j == '+') {
      end = j;
    }
  }
  if (begin && end && begin < end) {
    KString mangled(begin, end);
    if (mangled.compare(0, 2, "_Z") == 0) {
      char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, nullptr);
      if (demangled) {
        KString full_name(symbol, begin);
        full_name += demangled;
        full_name += end;
        free(demangled);
        return full_name;
      }
    }
    // C function
    return symbol;
  } else {
    return symbol;
  }
}

void PrintBacktrace(std::ostream& os) {
  const k_int32 max_frame_level = 32;
  void *array[max_frame_level];
  size_t size = backtrace(array, max_frame_level);
  char **symbols = backtrace_symbols(array, size);
  os << "backtrace: size:" << size << std::endl;
  for (size_t i = 0; i < size; i++) {
    os << "#" << i << " " << demangle(symbols[i]) << std::endl;
  }
  free(symbols);
}

void ExceptionHandler(const int sig, siginfo_t* const info, void*) {
  if (kHandlingException != 0) {
    _exit(128 + sig);
  }
  kHandlingException = 1;

  WriteExceptionRecord(STDERR_FILENO, sig, info);
  if (kErrlogFd >= 0 && kErrlogFd != STDERR_FILENO) {
    WriteExceptionRecord(kErrlogFd, sig, info);
  }

  RestorePreviousSignalAction(sig);
}

int32_t RegisterExceptionHandler(char *dir, PostExceptionCb cb) {
  const char* kwdb_data_root;
  (void)cb;
  if ( (kwdb_data_root = std::getenv("KWDB_DATA_ROOT")) &&
    ((std::strlen(kwdb_data_root) + 1 + std::strlen(kErrlogName)) < FULL_FILE_NAME_MAX_LEN) ) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(kErrlogPath, FULL_FILE_NAME_MAX_LEN, "%s/%s", kwdb_data_root,
             kErrlogName);
#pragma GCC diagnostic pop
  } else {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(kErrlogPath, FULL_FILE_NAME_MAX_LEN, "%s/%s", dir,
             kErrlogName);
#pragma GCC diagnostic pop
  }

  if (kErrlogFd >= 0) {
    close(kErrlogFd);
    kErrlogFd = -1;
  }
  kErrlogFd = OpenAppendFd(kErrlogPath);
  kHandlingException = 0;
  for (k_int32 i = 0; i < EXCEPTION_SIGNAL_CNT; i++) {
    struct sigaction sa;
    sa.sa_sigaction = ExceptionHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    // Golang will register the exception handler at the very begin.
    // so save it to kOldSigactions for reraise. Other program(AE/T_ME/KSQL) will
    // save a DFL handler, it's fine.
    if (sigaction(kExceptionSignals[i], &sa, &kOldSigactions[i]) == -1) {
      return -1;
    }
  }
  return 0;
}

#ifdef WITH_TESTS
void WriteExceptionRecordForTest(int sig, int si_code, void* si_addr) {
  siginfo_t info{};
  info.si_code = si_code;
  info.si_addr = si_addr;
  if (kErrlogFd >= 0) {
    WriteExceptionRecord(kErrlogFd, sig, &info);
  }
}
#endif

}  // namespace kwdbts
