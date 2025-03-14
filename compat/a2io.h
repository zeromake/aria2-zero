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
#ifndef D_A2IO_H
#define D_A2IO_H
#include "common.h"

#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif

#ifdef HAVE_SYS_STAT_H
#  include <sys/stat.h>
#endif

#ifdef HAVE_STDINT_H
#  include <stdint.h>
#endif // HAVE_STDINT_H

#ifdef HAVE_STDIO_H
#  include <stdio.h>
#endif // HAVE_STDINT_H

#ifdef HAVE_STDLIB_H
#  include <stdlib.h>
#endif // HAVE_STDLIB_H

#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif // HAVE_UNISTD_H

#ifdef HAVE_FCNTL_H
#  include <fcntl.h>
#endif // HAVE_FCNTL_H

#include <cerrno>
#ifdef HAVE_POLL_H
#  include <poll.h>
#endif // HAVE_POLL_H
#ifdef HAVE_IO_H
#  include <io.h>
#endif // HAVE_IO_H
#ifdef HAVE_WINIOCTL_H
#  include <winioctl.h>
#endif // HAVE_WINIOCTL_H
#ifdef HAVE_SHARE_H
#  include <share.h>
#endif // HAVE_SHARE_H

// in some platforms following definitions are missing:
#ifndef EINPROGRESS
#  define EINPROGRESS (WSAEINPROGRESS)
#endif // EINPROGRESS

#ifndef O_NONBLOCK
#  define O_NONBLOCK (O_NDELAY)
#endif // O_NONBLOCK

#ifndef O_BINARY
#  define O_BINARY (0)
#endif // O_BINARY

// st_mode flags
#ifndef S_IRUSR
#  define S_IRUSR 0000400 /* read permission, owner */
#endif                    /* S_IRUSR       */
#ifndef S_IWUSR
#  define S_IWUSR 0000200 /* write permission, owner */
#endif                    /* S_IWUSR       */
#ifndef S_IXUSR
#  define S_IXUSR 0000100 /* execute/search permission, owner */
#endif                    /* S_IXUSR */
#ifndef S_IRWXU
#  define S_IRWXU (S_IRUSR | S_IWUSR | S_IXUSR)
#endif /* S_IRWXU       */
#ifndef S_IRGRP
#  define S_IRGRP 0000040 /* read permission, group */
#endif                    /* S_IRGRP       */
#ifndef S_IWGRP
#  define S_IWGRP 0000020 /* write permission, group */
#endif                    /* S_IWGRP       */
#ifndef S_IXGRP
#  define S_IXGRP 0000010 /* execute/search permission, group */
#endif                    /* S_IXGRP */
#ifndef S_IRWXG
#  define S_IRWXG (S_IRGRP | S_IWGRP | S_IXGRP)
#endif /* S_IRWXG               */
#ifndef S_IROTH
#  define S_IROTH 0000004 /* read permission, other */
#endif                    /* S_IROTH       */
#ifndef S_IWOTH
#  define S_IWOTH 0000002 /* write permission, other */
#endif                    /* S_IWOTH       */
#ifndef S_IXOTH
#  define S_IXOTH 0000001 /* execute/search permission, other */
#endif                    /* S_IXOTH */
#ifndef S_IRWXO
#  define S_IRWXO (S_IROTH | S_IWOTH | S_IXOTH)
#endif /* S_IRWXO               */

// Use 'nul' instead of /dev/null in win32.
#ifdef HAVE_WINSOCK2_H
#  define DEV_NULL "nul"
#else
#  define DEV_NULL "/dev/null"
#endif // HAVE_WINSOCK2_H

// Use 'con' instead of '/dev/stdin' and '/dev/stdout' in win32.
#ifdef HAVE_WINSOCK2_H
#  define DEV_STDIN "con"
#  define DEV_STDOUT "con"
#else
#  define DEV_STDIN "/dev/stdin"
#  define DEV_STDOUT "/dev/stdout"
#endif // HAVE_WINSOCK2_H

#ifdef _WIN32
#  define a2lseek(fd, offset, origin) _lseeki64(fd, offset, origin)
#  define a2fseek(fd, offset, origin) _fseeki64(fd, offset, origin)
#  define a2fstat(fd, buf) _fstat64(fd, buf)
#  define a2ftell(fd) _ftelli64(fd)
#  ifdef stat
#    undef stat
#  endif // stat
typedef struct _stat64 a2_struct_stat;
typedef struct __utimbuf64 a2utimbuf;
int a2stat(const wchar_t*, a2_struct_stat*);
int a2mkdir(const wchar_t* path, int openMode);
int a2unlink(const wchar_t* path);
int a2rmdir(const wchar_t* path);
int a2open(const wchar_t* path, int flags, int mode);
FILE* a2fopen(const wchar_t* path, const wchar_t* mode);
int a2rename(const wchar_t* oldpath, const wchar_t* newpath);
int a2access(const wchar_t* path, int mode);
wchar_t* a2getcwd(wchar_t* buf, int size);
HANDLE a2CreateFileW(const wchar_t* lpFileName, DWORD dwDesiredAccess,
                     DWORD dwShareMode,
                     LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                     DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                     HANDLE hTemplateFile);
#  ifdef HAVE_SYS_UTIME_H
int a2utime(const wchar_t* path, a2utimbuf* t);
#  endif
#  define a2wstat(path, buf) a2stat(path, buf)
#  define a2tell(handle) _telli64(handle)
#  define a2isatty(fd) _isatty(fd)
// For Windows, we share files for reading and writing.
// # define a2ftruncate(fd, length): We don't use ftruncate in Mingw build
#  ifdef HAVE_STDINT_H
#    define a2_off_t int64_t
#  else
#    ifdef HAVE_LONG_LONG
#      define a2_off_t long long
#    else
#      define a2_off_t __int64
#    endif
#  endif

#  define a2close(fd) _close(fd)
#  define a2dup(fd) _dup(fd)
#  define a2dup2(fd) _dup2(fd)
#  define a2fileno(fp) _fileno(fp)
#  ifdef _MSC_VER
#    define mode_t int
#  endif // _MSC_VER

#  ifndef S_ISTYPE
#    define S_ISTYPE(mode, mask) (((mode) & S_IFMT) == (mask))
#  endif /* S_ISTYPE */

#  ifndef S_ISFIFO
#    define S_ISFIFO(mode) S_ISTYPE(mode, S_IFIFO)
#  endif /* S_ISFIFO */

#  ifndef S_ISCHR
#    define S_ISCHR(mode) S_ISTYPE(mode, S_IFCHR)
#  endif /* S_ISCHR */

#  ifndef S_ISDIR
#    define S_ISDIR(mode) S_ISTYPE(mode, S_IFDIR)
#  endif /* S_ISDIR */

#  ifndef S_ISBLK
#    define S_ISBLK(mode) S_ISTYPE(mode, S_IFBLK)
#  endif /* S_ISBLK */

#  ifndef S_ISREG
#    define S_ISREG(mode) S_ISTYPE(mode, S_IFREG)
#  endif /* S_ISREG */

#  ifndef S_ISLNK
#    define S_ISLNK(mode) S_ISTYPE(mode, S_IFLNK)
#  endif /* S_ISLNK */

#  ifndef S_ISSOCK
#    define S_ISSOCK(mode) S_ISTYPE(mode, S_IFSOCK)
#  endif /* S_ISSOCK */

#  ifndef STDIN_FILENO
#    define STDIN_FILENO _fileno(stdin)
#  endif /* STDIN_FILENO */

#  ifndef STDOUT_FILENO
#    define STDOUT_FILENO _fileno(stdout)
#  endif /* STDOUT_FILENO */

#  ifndef STDERR_FILENO
#    define STDERR_FILENO _fileno(stderr)
#  endif /* STDERR_FILENO */

#elif defined(__ANDROID__) || defined(ANDROID)
#  define a2lseek(fd, offset, origin) lseek64(fd, offset, origin)
// # define a2fseek(fp, offset, origin): No fseek64 and not used in aria2
#  define a2fstat(fd, buf) fstat64(fd, buf)
// # define a2ftell(fd): No ftell64 and not used in aria2
#  define a2_struct_stat struct stat64
#  define a2stat(path, buf) stat64(path, buf)
#  define a2mkdir(path, openMode) mkdir(path, openMode)
#  define a2utimbuf utimbuf
#  define a2utime(path, times) ::utime(path, times)
#  define a2unlink(path) unlink(path)
#  define a2rmdir(path) rmdir(path)
#  define a2open(path, flags, mode) open(path, flags, mode)
#  define a2fopen(path, mode) fopen(path, mode)
#  define a2close(fd) close(fd)
#  define a2dup(fd) dup(fd)
#  define a2dup2(fd) dup2(fd)
#  define a2fileno(fp) fileno(fp)
#  define a2rename(oldpath, newpath) rename(oldpath, newpath)
#  define a2getcwd(buf, size) getcwd(buf, size)
#  define a2access(path, mode) access(path, mode)
#  define a2isatty(fd) isatty(fd)
// Android NDK R8e does not provide ftruncate64 prototype, so let's
// define it here.
#  ifdef __cplusplus
extern "C" {
#  endif
extern int ftruncate64(int fd, off64_t length);
#  ifdef __cplusplus
}
#  endif
#  define a2ftruncate(fd, length) ftruncate64(fd, length)
// Use off64_t directly since android does not offer transparent
// switching between off_t and off64_t.
#  define a2_off_t off64_t
#else // !_WIN32 && !(defined(__ANDROID__) || defined(ANDROID))
#  define a2lseek(fd, offset, origin) lseek(fd, offset, origin)
#  define a2fseek(fp, offset, origin) fseek(fp, offset, origin)
#  define a2fstat(fp, buf) fstat(fp, buf)
#  define a2ftell(fp) ftell(fp)
#  define a2_struct_stat struct stat
#  define a2stat(path, buf) stat(path, buf)
#  define a2mkdir(path, openMode) mkdir(path, openMode)
#  define a2utimbuf utimbuf
#  define a2utime(path, times) ::utime(path, times)
#  define a2unlink(path) unlink(path)
#  define a2rmdir(path) rmdir(path)
#  define a2open(path, flags, mode) open(path, flags, mode)
#  define a2fopen(path, mode) fopen(path, mode)
#  define a2ftruncate(fd, length) ftruncate(fd, length)
#  define a2close(fd) close(fd)
#  define a2dup(fd) dup(fd)
#  define a2dup2(fd) dup2(fd)
#  define a2fileno(fp) fileno(fp)
#  define a2rename(oldpath, newpath) rename(oldpath, newpath)
#  define a2getcwd(buf, size) getcwd(buf, size)
#  define a2access(path, mode) access(path, mode)
#  define a2isatty(fd) isatty(fd)
#  define a2_off_t off_t
#endif

#define OPEN_MODE S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH
#define DIR_OPEN_MODE S_IRWXU | S_IRWXG | S_IRWXO

#ifdef _WIN32
#  define A2_BAD_FD INVALID_HANDLE_VALUE
#else // !_WIN32
#  define A2_BAD_FD -1
#endif // !_WIN32

#endif // D_A2IO_H
