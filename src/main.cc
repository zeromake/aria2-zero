/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "common.h"

#include "a2io.h"

#ifdef _WIN32
#  include <shellapi.h>
#endif // _WIN32

#include <aria2/aria2.h>
#include "Context.h"
#include "MultiUrlRequestInfo.h"
#include "message.h"
#include "Platform.h"
#include "Exception.h"
#include "console.h"
#include "LogFactory.h"
#include "util.h"
#include "File.h"
#include <iostream>

#ifdef _WIN32
#include <client/windows/handler/exception_handler.h>
#elif defined(__APPLE__)
#include <client/mac/handler/exception_handler.h>
#endif // _WIN32


namespace aria2 {

error_code::Value main(int argc, char** argv)
{
#ifdef _WIN32
  int winArgc;
  auto winArgv = CommandLineToArgvW(GetCommandLineW(), &winArgc);
  if (winArgv == nullptr) {
    A2_LOG_ERROR("Reading command-line failed");
    return error_code::UNKNOWN_ERROR;
  }
  std::vector<std::unique_ptr<char>> winArgStrs;
  winArgStrs.reserve(winArgc);
  auto pargv = aria2::make_unique<char*[]>(winArgc);
  for (int i = 0; i < winArgc; ++i) {
    winArgStrs.emplace_back(strdup(wCharToUtf8(winArgv[i]).c_str()));
    pargv[i] = winArgStrs.back().get();
  }

  Context context(true, winArgc, pargv.get(), KeyVals());
#else  // !_WIN32
  Context context(true, argc, argv, KeyVals());
#endif // !_WIN32

  error_code::Value exitStatus = error_code::FINISHED;
  if (context.reqinfo) {
    exitStatus = context.reqinfo->execute();
  }
  return exitStatus;
}

} // namespace aria2

#ifdef ENABLE_BREAKPAD
#ifdef _WIN32
static bool callback(const wchar_t* dump_path,
  const wchar_t* minidump_id,
  void* context,
  EXCEPTION_POINTERS* exinfo,
  MDRawAssertionInfo* assertion,
  bool succeeded) {
  std::wcerr << L"Crash dump:" << dump_path << L"\\" << minidump_id << std::endl;
  return succeeded;
}

static std::unique_ptr<google_breakpad::ExceptionHandler> initExceptionHandler() {
  std::string localeDir = aria2::util::applyDir(aria2::util::getProgramLocation(), "dumps");
  std::replace(localeDir.begin(), localeDir.end(), '/', '\\');
  aria2::File dir(localeDir);
  if (!dir.exists()) {
    a2mkdir(aria2::utf8ToWChar(localeDir).c_str(), DIR_OPEN_MODE);
  }
  std::wstring wlocaleDir = aria2::utf8ToWChar(localeDir);
  auto eh = aria2::make_unique<google_breakpad::ExceptionHandler>(wlocaleDir.c_str(),
    nullptr,
    callback,
    nullptr, -1);
  return eh;
}
#elif defined(__APPLE__)
static bool callback(const char* dump_dir,
  const char* minidump_id,
  void* context,
  bool succeeded) {
  std::cerr << "Crash dump:" << dump_dir << "/" << minidump_id << std::endl;
    return succeeded;
}
static std::unique_ptr<google_breakpad::ExceptionHandler> initExceptionHandler() {
  std::string localeDir = aria2::util::applyDir(aria2::util::getProgramLocation(), "dumps");
  aria2::File dir(localeDir);
  if (!dir.exists()) {
    a2mkdir(localeDir.c_str(), DIR_OPEN_MODE);
  }
  auto eh = aria2::make_unique<google_breakpad::ExceptionHandler>(
    localeDir.c_str(),
    nullptr,
    callback,
    nullptr,
    true,
    nullptr);
  return eh;
}
#endif // _WIN32

#endif // ENABLE_BREAKPAD

int main(int argc, char** argv)
{
#ifdef ENABLE_BREAKPAD
  auto eh = initExceptionHandler();
#endif // ENABLE_BREAKPAD
  aria2::error_code::Value r;
  aria2::global::initConsole(false);
  try {
    aria2::Platform platform;
    r = aria2::main(argc, argv);
  }
  catch (aria2::Exception& ex) {
    aria2::global::cerr()->printf("%s\n%s\n", EX_EXCEPTION_CAUGHT,
                                  ex.stackTrace().c_str());
    r = ex.getErrorCode();
  }
  return r;
}
