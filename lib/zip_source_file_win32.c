/*
  zip_source_file_win32.c -- read-only Windows file source implementation
  Copyright (C) 1999-2020 Dieter Baron and Thomas Klausner

  This file is part of libzip, a library to manipulate ZIP archives.
  The authors can be contacted at <libzip@nih.at>

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
  1. Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in
  the documentation and/or other materials provided with the
  distribution.
  3. The names of the authors may not be used to endorse or promote
  products derived from this software without specific prior
  written permission.

  THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS
  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "zip_source_file_win32.h"

static zip_source_file_operations_t ops_win32_read = {
    _zip_win32_op_close,
    NULL,
    NULL,
    NULL,
    NULL,
    _zip_win32_op_read,
    NULL,
    NULL,
    _zip_win32_op_seek,
    _zip_win32_op_stat,
    NULL,
    _zip_win32_op_tell,
    NULL
};


ZIP_EXTERN zip_source_t *
zip_source_win32handle(zip_t *za, HANDLE h, zip_uint64_t start, zip_int64_t len) {
    if (za == NULL) {
        return NULL;
    }
    
    return zip_source_win32handle_create(h, start, len, &za->error);
}


ZIP_EXTERN zip_source_t *
zip_source_win32handle_create(HANDLE h, zip_uint64_t start, zip_int64_t length, zip_error_t *error) {
    if (h == INVALID_HANDLE_VALUE || length < -1) {
        zip_error_set(error, ZIP_ER_INVAL, 0);
        return NULL;
    }

    return zip_source_file_common_new(NULL, h, start, len, NULL, ops_win32_read, NULL, error);
}


void
zip_win32_op_close(zip_source_file_context_t *ctx) {
    CloseHandle((HANDLE)ctx->f);
}


zip_int64_t
_zip_win32_op_read(zip_source_file_context_t *ctx, void *buf, zip_uint64_t len) {
    DWORD i;

    if (!ReadFile((HANDLE)ctx->f, buf, (DWORD)len, &i, NULL)) {
        zip_error_set(&ctx->error, ZIP_ER_READ, _zip_win32_error_to_errno(GetLastError()));
        return -1;
    }
    
    return (zip_int64_t)i;
}


bool
_zip_win32_op_seek(zip_source_file_context_t *ctx, void *f, zip_int64_t offset, int whence) {
    LARGE_INTEGER li;
    DWORD method;

    switch (whence) {
    case SEEK_SET:
        method = FILE_BEGIN;
        break;
    case SEEK_END:
        method = FILE_END;
        break;
    case SEEK_CUR:
        method = FILE_CURRENT;
        break;
    default:
        zip_error_set(error, ZIP_ER_SEEK, EINVAL);
        return -1;
    }

    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, method)) {
    zip_error_set(error, ZIP_ER_SEEK, _zip_win32_error_to_errno(GetLastError()));
        return false;
    }

    return true;
}


static bool
_zip_win32_op_stat(zip_source_file_context_t *ctx, zip_source_file_stat_t *st) {
    return _zip_stat_win32(ctx, st, (HANDLE)ctx->f);
}


zip_int64_t
_zip_win32_op_write(zip_source_file_context_t *ctx, const void *data, zip_uint64_t len) {
    DWORD ret;
    if (!WriteFile((HANDLE)ctx->fout, data, (DWORD)len, &ret, NULL) || ret != len) {
        zip_error_set(&ctx->error, ZIP_ER_WRITE, _zip_win32_error_to_errno(GetLastError()));
        return -1;
    }

    return (zip_int64_t)ret;
}


zip_int64_t
_zip_win32_op_tell(zip_source_file_context_t *ctx, void *f) {
    LARGE_INTEGER zero;
    LARGE_INTEGER new_offset;
    
    if (!SetFilePointerEx((HANDLE)ctx->f, zero, &new_offset, FILE_CURRENT)) {
        zip_error_set(&ctx->error, ZIP_ER_SEEK, _zip_win32_error_to_errno(GetLastError()));
        return -1;
    }
    
    return (zip_int64_t)new_offset.QuadPart;
}


int
_zip_win32_error_to_errno(DWORD win32err) {
    /* Note: This list isn't exhaustive, but should cover common cases. */
    switch (win32err) {
    case ERROR_INVALID_PARAMETER:
        return EINVAL;
    case ERROR_FILE_NOT_FOUND:
        return ENOENT;
    case ERROR_INVALID_HANDLE:
        return EBADF;
    case ERROR_ACCESS_DENIED:
        return EACCES;
    case ERROR_FILE_EXISTS:
        return EEXIST;
    case ERROR_TOO_MANY_OPEN_FILES:
        return EMFILE;
    case ERROR_DISK_FULL:
        return ENOSPC;
    default:
        return 0; /* TODO: better default error code */
    }
}

zip_int64_t
_zip_win32_create_temp_output(zip_source_file_context_t *ctx, char *tmpname, size_t tmpname_size, _zip_win32_open_t open, _zip_win32_mktmepname_t mktempname) {
    /* Windows has GetTempFileName(), but it closes the file after
     creation, leaving it open to a horrible race condition. So
     we reinvent the wheel.
     */
     
    int i;
    HANDLE th = INVALID_HANDLE_VALUE;
    void *temp = NULL;
    PSECURITY_DESCRIPTOR psd = NULL;
    PSECURITY_ATTRIBUTES psa = NULL;
    SECURITY_ATTRIBUTES sa;
    SECURITY_INFORMATION si;
    DWORD success;
    PACL dacl = NULL;

    /*
     Read the DACL from the original file, so we can copy it to the temp file.
     If there is no original file, or if we can't read the DACL, we'll use the
     default security descriptor.
     */
     
    if ((HANDLE)ctx->f != INVALID_HANDLE_VALUE && GetFileType((HANDLE)ctx->f) == FILE_TYPE_DISK) {
        si = DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION;
        success = GetSecurityInfo(ctx->h, SE_FILE_OBJECT, si, NULL, NULL, &dacl, NULL, &psd);
        if (success == ERROR_SUCCESS) {
            sa.nLength = sizeof(SECURITY_ATTRIBUTES);
            sa.bInheritHandle = FALSE;
            sa.lpSecurityDescriptor = psd;
            psa = &sa;
        }
    }

    #ifndef MS_UWP
        value = GetTickCount();
    #else
        value = (zip_uint32_t)GetTickCount64();
    #endif

    for (i = 0; i < 1024 && th == INVALID_HANDLE_VALUE; i++) {
        mktempname(tmpname, tmpname_size, value + i);
        th = open(ctx, tmpname, true, psa);

        if (th == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
            break;
    }

    LocalFree(psd);

    if (th == INVALID_HANDLE_VALUE) {
        zip_error_set(&ctx->error, ZIP_ER_TMPOPEN, _zip_win32_error_to_errno(GetLastError()));
        return INVALID_HANDLE_VALUE;
    }

    return th;
}

bool
_zip_stat_win32(zip_source_file_context_t *ctx, zip_source_file_stat_t *st, HANDLE h) {
    FILETIME mtimeft;
    time_t mtime;
    LARGE_INTEGER size;
    int regularp;

    if (!GetFileTime(h, NULL, NULL, &mtimeft)) {
        zip_error_set(&ctx->error, ZIP_ER_READ, _zip_win32_error_to_errno(GetLastError()));
        return false;
    }
    if (_zip_filetime_to_time_t(mtimeft, &mtime) < 0) {
        zip_error_set(&ctx->error, ZIP_ER_READ, ERANGE);
        return false;
    }
    
    st->exists = true;
    st->mtime = mtime;

    if (GetFileType(h) == FILE_TYPE_DISK) {
        st->regular_file = 1;

        if (!GetFileSizeEx(h, &size)) {
            zip_error_set(&ctx->error, ZIP_ER_READ, _zip_win32_error_to_errno(GetLastError()));
            return false;
        }

        st->size = size;
    }
    
    /* TODO: fill in ctx->attributes */

    return true;
}


static int
_zip_filetime_to_time_t(FILETIME ft, time_t *t) {
    /*
    Inspired by http://stackoverflow.com/questions/6161776/convert-windows-filetime-to-second-in-unix-linux
    */
    const zip_int64_t WINDOWS_TICK = 10000000LL;
    const zip_int64_t SEC_TO_UNIX_EPOCH = 11644473600LL;
    ULARGE_INTEGER li;
    zip_int64_t secs;
    time_t temp;

    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    secs = (li.QuadPart / WINDOWS_TICK - SEC_TO_UNIX_EPOCH);

    temp = (time_t)secs;
    if (secs != (zip_int64_t)temp)
    return -1;

    *t = temp;
    return 0;
}
