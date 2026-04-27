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

#include "gtest/gtest.h"

#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "cm_backtrace.h"
#include "cm_exception.h"

namespace kwdbts {
namespace {

GTEST_API_ int main(int argc, char **argv) {
  std::cout << "Running main() from gtest_main.cc\n";
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

std::string ReadFile(const char* path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return "";
  }

  std::string content;
  char buffer[256];
  ssize_t bytes_read = 0;
  while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
    content.append(buffer, static_cast<size_t>(bytes_read));
  }
  close(fd);
  return content;
}

TEST(SignalHandlerTest, ExceptionHandlerWritesMinimalRecord) {
  char dir_template[] = "/tmp/kwdb_sigaction_XXXXXX";
  char* dir = mkdtemp(dir_template);
  ASSERT_NE(dir, nullptr);

  ASSERT_EQ(RegisterExceptionHandler(dir), 0);
  WriteExceptionRecordForTest(SIGSEGV, SEGV_MAPERR, reinterpret_cast<void*>(0x1234));

  std::string log_path = std::string(dir) + "/errlog.log";
  std::string content = ReadFile(log_path.c_str());

  EXPECT_NE(content.find(std::string("signal=") + std::to_string(SIGSEGV)), std::string::npos);
  EXPECT_NE(content.find("pid="), std::string::npos);
  EXPECT_NE(content.find("tid="), std::string::npos);
  EXPECT_NE(content.find(std::string("si_code=") + std::to_string(SEGV_MAPERR)), std::string::npos);
  EXPECT_NE(content.find("si_addr=0x1234"), std::string::npos);
  EXPECT_EQ(content.find("Exception time"), std::string::npos);
  EXPECT_EQ(content.find("backtrace: size:"), std::string::npos);

  EXPECT_EQ(unlink(log_path.c_str()), 0);
  EXPECT_EQ(rmdir(dir), 0);
}

TEST(SignalHandlerTest, ThreadSignalHandlerWritesMinimalRecord) {
  char file_template[] = "/tmp/kwdb_thread_signal_XXXXXX";
  int fd = mkstemp(file_template);
  ASSERT_TRUE(fd >= 0);

  ucontext_t context{};
  ASSERT_EQ(getcontext(&context), 0);
  WriteThreadBacktraceRecordForTest(fd, SIGUSR2, &context);
  ASSERT_TRUE(lseek(fd, 0, SEEK_SET) >= 0);

  char buffer[256] = {0};
  ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
  ASSERT_TRUE(bytes_read > 0);
  std::string content(buffer, static_cast<size_t>(bytes_read));

  EXPECT_NE(content.find(std::string("thread_signal=") + std::to_string(SIGUSR2)), std::string::npos);
  EXPECT_NE(content.find("pid="), std::string::npos);
  EXPECT_NE(content.find("tid="), std::string::npos);
#if defined(__x86_64__) || defined(__aarch64__)
  EXPECT_NE(content.find("pc=0x"), std::string::npos);
  EXPECT_NE(content.find("sp=0x"), std::string::npos);
#endif
  EXPECT_EQ(content.find("backtrace: size:"), std::string::npos);
  EXPECT_EQ(content.find("#0 "), std::string::npos);

  close(fd);
  EXPECT_EQ(unlink(file_template), 0);
}

}  // namespace
}  // namespace kwdbts
