/*
 * Copyright 2021 Weiwen Chen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __ROBOCOPY_H__
#define __ROBOCOPY_H__

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "resource.h"
#include "getopt_dos.h"

/* return values */

/* 0: No files were copied. No failure was encountered.
      No files were mismatched. The files already exist in
      the destination directory; therefore, the copy
      operation was skipped. */
#define ROBOCOPY_EXIT_SKIPPED 0 
/* 1: All files were copied successfully. */
#define ROBOCOPY_EXIT_SUCCESS 1
/* 2: There are some additional files in the destination
      directory that are not present in the source directory.
      No files were copied.  */
#define ROBOCOPY_EXIT_ADDITIONAL_FILE_NO_COPY 2
/* 3: Some files were copied. Additional files were present.
      No failure was encountered. */
#define ROBOCOPY_EXIT_ADDITIONAL_FILE_SOME_COPIED 3
/* 5: Some files were copied. Some files were mismatched. No 
      failure was encountered. */
#define ROBOCOPY_EXIT_MISMATCH_FILE_SOME_COPIED 5
/* 6: Additional files and mismatched files exist. No files 
      were copied and no failures were encountered. This means
      that the files already exist in the destination directory. */
#define ROBOCOPY_EXIT_ADDITIONAL_MISMATCH_FILE_NO_COPY 6
/* 7: Files were copied, a file mismatch was present, and
      additional files were present. */
#define ROBOCOPY_EXIT_ADDITIONAL_MISMATCH_FILE_COPIED 7
/* 8: Several files did not copy. */
#define ROBOCOPY_EXIT_SEVERAL_FILES_NOT_COPY 8

/* FIXME: temp return value placeholder */
#define INTERNAL_ERROR_PLACEHOLDER -65535

/* return value indicator for internal functions. for clearance. */
#define INTERNAL_SUCCESS 0
#define INTERNAL_FAILURE -65534  /* UGLY: not to collapse with GDOS_NEXT_* errors */

typedef enum {
    COPY_SUBDIR_TYPE_NOT,
    COPY_SUBDIR_TYPE_EXCLUDE_EMPTY,
    COPY_SUBDIR_TYPE_INCLUDE_EMPTY,
} copy_subdirectory_type;

typedef struct {
    /* point to argv array */
    const WCHAR *source;
    const WCHAR *destination;
    const WCHAR **files;
    int num_files;

    /* copy options */
    /* /s, /e, /mir */
    copy_subdirectory_type copy_subdir;
    /* /purge */
    BOOL purge_not_in_source;

    /* file selection options */
    /* /xf <filename>[...] */
    const WCHAR **exclude_file_patterns;
    int num_exclude_file_patterns;
    /* /xd <directory>[...]*/
    const WCHAR **exclude_directory_patterns;
    int num_exclude_directory_patterns;
    /* /xo */
    BOOL exclude_older_file;

    /* retry options */
    /* /r:<n> */
    int num_retries_on_failed_copies;
    /* /w:<n> */
    int wait_second_between_retries;

    /* logging options */
    /* /ns */
    BOOL not_log_file_size;
    /* /nc */
    BOOL not_log_file_class;
    /* /np */
    BOOL not_display_progress;
    /* /nfl */
    BOOL not_log_file_names;
    /* /ndl */
    BOOL not_log_directory_names;
    /* /njh */
    BOOL no_job_header;
    /* /njs */
    BOOL not_job_summary;

    /* job options */
} robocopy_context;

/* robocopy.c */

int robocopy_init_default_options(robocopy_context *x);
int robocopy_parse_options(robocopy_context *x, int argc, const WCHAR **argvW);

/* output.c */

void output_writeconsole(const WCHAR *str, DWORD wlen);
void WINAPIV output_message(unsigned int id, ...);
void WINAPIV output_string(const WCHAR *fmt, ...);

#endif /* __ROBOCOPY_H__ */
