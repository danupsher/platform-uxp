// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/file_util.h"

#import <Cocoa/Cocoa.h>

#include <AvailabilityMacros.h>
#if defined(MAC_OS_X_VERSION_10_5) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_5
#include <copyfile.h>
#else
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "base/file_path.h"
#include "base/logging.h"
#include "base/string_util.h"
#include "base/scoped_nsautorelease_pool.h"

namespace file_util {

bool GetTempDir(FilePath* path) {
  base::ScopedNSAutoreleasePool autorelease_pool;
  NSString* tmp = NSTemporaryDirectory();
  if (tmp == nil)
    return false;
  *path = FilePath([tmp fileSystemRepresentation]);
  return true;
}

bool GetShmemTempDir(FilePath* path) {
  return GetTempDir(path);
}

bool CopyFile(const FilePath& from_path, const FilePath& to_path) {
#if defined(MAC_OS_X_VERSION_10_5) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_5
  return (copyfile(from_path.value().c_str(),
                   to_path.value().c_str(), NULL, COPYFILE_ALL) == 0);
#else
  // Tiger fallback: simple byte copy
  int src = open(from_path.value().c_str(), O_RDONLY);
  if (src < 0) return false;
  int dst = open(to_path.value().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (dst < 0) { close(src); return false; }
  char buf[8192];
  ssize_t n;
  bool ok = true;
  while ((n = read(src, buf, sizeof(buf))) > 0) {
    if (write(dst, buf, n) != n) { ok = false; break; }
  }
  if (n < 0) ok = false;
  close(dst);
  close(src);
  return ok;
#endif
}

}  // namespace
