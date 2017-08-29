/* Copyright (c) 2013, Philipp Tölke
 * Copyright (c) 2017, Benjamin Koch
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef INCLUDE_VFS_H
#define INCLUDE_VFS_H

#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/dirent.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "ftpd";

#define vfs_load_plugin(x)
#define bcopy(src, dest, len) memmove(dest, src, len)


typedef DIR vfs_dir_t;
typedef FILE vfs_file_t;
typedef struct stat vfs_stat_t;
typedef struct dirent vfs_dirent_t;
typedef struct {
  // We have two buffers for absolute paths. The first cwdlen characters
  // will always contain the current working directory (cwd). The last
  // character of the cwd will always be a slash (unless temporary
  // replaced). You can use abspath or abspath2 to make a path absolute
  // and put it into one of the buffers. The first rootlen characters
  // will never be replaced by something else because they constitute
  // the path to the root of our virtual filesystem. The first cwdlen
  // characters won't usually be replaced (i.e. mkdir("../abc") will
  // *not* work) but they can be replaced by vfs_chdir.
  char file1[CONFIG_FATFS_MAX_LFN+1], file2[CONFIG_FATFS_MAX_LFN+1];
  size_t rootlen, cwdlen;
} vfs_t;

// The mapping of most functions is actually fairly straightforward.
// However, we have to keep track of the current directory because
// the vfs is configured not to support relative paths and we would
// want a cwd per connection anyway.
//TODO This only exposes the SD card, for now. We should add a virtual root
//     directory that lists all mounted filesystems.
#define VFS_ROOT "/sdcard/"

//#define time(x)

#define vfs_read fread
#define vfs_write fwrite
#define vfs_eof feof

#define vfs_readdir readdir
#define vfs_closedir closedir

#define VFS_ISDIR(st_mode) S_ISDIR(st_mode)
#define VFS_ISREG(st_mode) S_ISREG(st_mode)
#define VFS_IRWXU 0
#define VFS_IRWXG 0
#define VFS_IRWXO 0

static inline vfs_t* vfs_openfs() {
  vfs_t* vfs = (vfs_t*)malloc(sizeof(vfs_t));
  if (!vfs)
    return NULL;
  strcpy(vfs->file1, VFS_ROOT);
  strcpy(vfs->file2, VFS_ROOT);
  vfs->rootlen = vfs->cwdlen = strlen(VFS_ROOT);
  return vfs;
}

static inline void vfs_close(vfs_t* vfs) {
  free(vfs);
}

static inline void vfs_close_file(vfs_file_t* file) {
  fclose(file);
}

static inline void normalize_path(char* path) {
  while (*path == '/' || (*path == '.' && path[1] == '/'))
    memmove(path, path+1, strlen(path+1)+1);

  char* pos = path;
  while ((pos = strstr(pos, "//"))) {
    memmove(pos, pos+1, strlen(pos+1)+1);
  }

  pos = path;
  while ((pos = strstr(pos, "/."))) {
    if (memcmp(pos, "//", 2) == 0) {
      memmove(pos, pos+1, strlen(pos+1)+1);
    } else if (memcmp(pos, "/.\0", 3) == 0 || memcmp(pos, "/./", 3) == 0) {
      memmove(pos, pos+2, strlen(pos+2)+1);
    } else if (memcmp(pos, "/..\0", 4) == 0 || memcmp(pos, "/../", 4) == 0) {
      *pos = 0;
      char* slash = strrchr(path, '/');
      if (!slash) {
        slash = path;
        if (pos[3] == '/')
          pos++;
      }
      memmove(slash, pos+3, strlen(pos+3)+1);
      pos = slash;
    } else {
      pos += 2;
    }
  }
}

static inline const char* abspath_(vfs_t* vfs, char* buffer, size_t buflen, const char* path, bool limit_to_cwd) {
  size_t len = strlen(path);
  if (buflen < vfs->cwdlen + len + 1) {
    ESP_LOGE(TAG, "path too long: %s", path);
    return NULL;
  }

  // The last slash in cwd is sometimes set to '\0' so we restore it.
  buffer[vfs->cwdlen-1] = '/';

  if (*path == '/') {
    if (limit_to_cwd) {
      // It is an absolute path. We always get this for vfs_opendir and the user may pass
      // an absolute path to many other commands.
      //NOTE We mustn't overwrite the cwd so we only support absolute paths that point to
      //     a location inside the cwd.
      if (len < vfs->cwdlen-vfs->rootlen
          || memcmp(buffer+vfs->rootlen-1, path, vfs->cwdlen-vfs->rootlen) != 0
          || (path[vfs->cwdlen-vfs->rootlen] != 0 && path[vfs->cwdlen-vfs->rootlen] != '/')) {
        ESP_LOGW(TAG, "refusing absolute path which doesn't point into the current cwd");
        return NULL;
      }
  
      memcpy(buffer+vfs->cwdlen-1, path+vfs->cwdlen-vfs->rootlen, len-(vfs->cwdlen-vfs->rootlen)+1);
    } else {
      memcpy(buffer+vfs->rootlen-1, path, len+1);
    }
  } else {
    memcpy(buffer+vfs->cwdlen, path, len+1);
  }

  //NOTE This will not work for paths that reference something above cwd. This is on purpose
  //     because that would overwrite our only copy of the cwd. I think clients will hardly
  //     ever do that in practice.
  if (limit_to_cwd)
    normalize_path(buffer+vfs->cwdlen);
  else
    normalize_path(buffer+vfs->rootlen);

  return buffer;
}

static inline const char* abspath(vfs_t* vfs, const char* path) {
  return abspath_(vfs, vfs->file1, sizeof(vfs->file1), path, true);
}

static inline const char* abspath2(vfs_t* vfs, const char* path) {
  return abspath_(vfs, vfs->file2, sizeof(vfs->file2), path, true);
}

static inline int vfs_chdir(vfs_t* vfs, const char* path) {
  // We usually don't go higher than cwd in abspath but passing false for the
  // last argument allows that.
  path = abspath_(vfs, vfs->file1, sizeof(vfs->file1), path, false);
  if (!path)
    return 1;

  int len = strlen(path);
  if (path[len-1] != '/') {
    if (len+2 >= sizeof(vfs->file1)) {
      ESP_LOGE(TAG, "path too long in vfs_chdir");
      memcpy(vfs->file1, vfs->file2, vfs->cwdlen);
      vfs->file1[vfs->cwdlen] = 0;
      return 1;
    } else {
      vfs->file1[len] = '/';
      len++;
      vfs->file1[len] = 0;
    }
  }

  struct stat st;
  vfs->file1[len-1] = 0;
  if (len == vfs->rootlen || (stat(path, &st) == 0 && S_ISDIR(st.st_mode))) {
    vfs->file1[len-1] = '/';
    vfs->cwdlen = len;
    memcpy(vfs->file2, vfs->file1, len+1);
    return 0;
  } else {
    ESP_LOGW(TAG, "FTP client tried to chdir to a directory that doesn't exist: %s", path);
    memcpy(vfs->file1, vfs->file2, vfs->cwdlen);
    vfs->file1[vfs->cwdlen] = 0;
    return 1;
  }
}

static inline int vfs_mkdir(vfs_t* vfs, const char* path, int mode) {
  path = abspath(vfs, path);
  return path && mkdir(path, mode) == 0 ? 0 : 1;
}

static inline int vfs_rmdir(vfs_t* vfs, const char* path) {
  path = abspath(vfs, path);
  return path && rmdir(path) == 0 ? 0 : 1;
}

static inline int vfs_remove(vfs_t* vfs, const char* path) {
  path = abspath(vfs, path);
  return path && unlink(path) == 0 ? 0 : 1;
}

static inline int vfs_rename(vfs_t* vfs, const char* from, const char* to) {
  from = abspath (vfs, from);
  to   = abspath2(vfs, to);
ESP_LOGI(TAG, "vfs_rename: %s -> %s", from, to);
  return from && to && rename(from, to) ? 1 : 0;
}

static inline char* vfs_getcwd(vfs_t* vfs, void* dummy1, int dummy2) {
  // Second argument is always NULL so we have to allocate memory.
  // We could use one of our buffers but ftpd will free the string.
  if (vfs->cwdlen > vfs->rootlen)
    // remove the trailing slash and other content
    vfs->file1[vfs->cwdlen-1] = 0;
  else
    // keep the slash to avoid returning an empty path
    vfs->file1[vfs->cwdlen] = 0;
  return strdup(vfs->file1 + vfs->rootlen-1);
}

static inline vfs_file_t* vfs_open(vfs_t* vfs, const char* path, const char* mode) {
  path = abspath(vfs, path);
  if (!path)
    return NULL;
  return fopen(path, mode);
}

static inline int vfs_stat(vfs_t* vfs, const char* path, vfs_stat_t* st) {
  path = abspath(vfs, path);
  if (path && stat(path, st) == 0) {
    return 0;
  } else {
    // The return value of vfs_stat isn't checked in many places so we need
    // to put something sensible into st.
    memset(st, 0, sizeof(vfs_stat_t));
    return -1;
  }
}

static inline vfs_dir_t* vfs_opendir(vfs_t* vfs, const char* path) {
  path = abspath(vfs, path);
  if (!path)
    return NULL;
  return opendir(path);
}

// ftpd.c redefines some of the POSIX stuff so we undefine it here to avoid warnings.
#undef EINVAL
#undef ENOMEM
#undef ENODEV

// ftpd.c tries to use dirent->name but it is actually called dirent->dname. This define
// will rename a lot of unrelated things but that shouldn't matter because it renames
// all the references, as well.
#define name d_name

#define FTPD_DEBUG

#ifdef FTPD_DEBUG
#define ftpd_logd(...) ESP_LOGD(TAG, __VA_ARGS__)
#define ftpd_logi(...) ESP_LOGI(TAG, __VA_ARGS__)
#define ftpd_logw(...) ESP_LOGW(TAG, __VA_ARGS__)
#define ftpd_loge(...) ESP_LOGE(TAG, __VA_ARGS__)
#endif

#endif /* INCLUDE_VFS_H */

