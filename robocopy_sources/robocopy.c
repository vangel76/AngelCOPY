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

#include "robocopy.h"
#include <wine/debug.h>

#define DEBUG_DEV_WEIWEN

enum opt_indicator
{
    I_COPY_SUBDIRECTORIES_EXCLUDE_EMPTY,
    I_COPY_SUBDIRECTORIES_INCLUDE_EMPTY,
    I_PURGE_NOT_IN_SOURCE,
    I_MIRROR_DIRECTORY_TREE,
    I_EXCLUDE_FILES,
    I_EXCLUDE_DIRECTORY,
    I_EXCLUDE_OLDER_FILE,
    I_RETRY_NUM_ON_FAILED,
    I_WAIT_TIME_BETWEEN_RETRIES,
    I_NO_LOG_FILE_SIZE,
    I_NO_LOG_FILE_CLASS,
    I_NO_LOG_COPY_PROGRESS,
    I_NO_FILE_NAME_LOGGED,
    I_NO_DIRECTORY_NAME_LOGGED,
    I_NO_JOB_HEADER,
    I_NO_JOB_SUMMARY,
};

const static WCHAR *file_patterns_default_value[] = {L"*.*"};

static gdos_option const opts[] = {
    /* copy options */
    {L"s", NO_ARGUMENT, I_COPY_SUBDIRECTORIES_EXCLUDE_EMPTY},
    {L"e", NO_ARGUMENT, I_COPY_SUBDIRECTORIES_INCLUDE_EMPTY},

    /* FIXME: What means "allows the destination directory
              security settings to not be overwritten." in doc? */
    {L"purge", NO_ARGUMENT, I_PURGE_NOT_IN_SOURCE},
    {L"mir", NO_ARGUMENT, I_MIRROR_DIRECTORY_TREE},

    /* file selection options */
    {L"xf", SPACE_SEPERATED_ARGUMENT_LIST, I_EXCLUDE_FILES},
    {L"xd", SPACE_SEPERATED_ARGUMENT_LIST, I_EXCLUDE_DIRECTORY},
    {L"xo", NO_ARGUMENT, I_EXCLUDE_OLDER_FILE},

    /* retry options */
    {L"r", COLON_SEPERATED_ARGUMENT, I_RETRY_NUM_ON_FAILED},
    {L"w", COLON_SEPERATED_ARGUMENT, I_WAIT_TIME_BETWEEN_RETRIES},

    /* logging options */
    {L"ns", NO_ARGUMENT, I_NO_LOG_FILE_SIZE},
    {L"nc", NO_ARGUMENT, I_NO_LOG_FILE_CLASS},
    {L"np", NO_ARGUMENT, I_NO_LOG_COPY_PROGRESS},
    {L"nfl", NO_ARGUMENT, I_NO_FILE_NAME_LOGGED},
    {L"ndl", NO_ARGUMENT, I_NO_DIRECTORY_NAME_LOGGED},
    {L"njh", NO_ARGUMENT, I_NO_JOB_HEADER},
    {L"njs", NO_ARGUMENT, I_NO_JOB_SUMMARY},
    /* job options */
};

#ifdef DEBUG_DEV_WEIWEN

void print_bool(const BOOL b)
{
    printf(b ? "TRUE\n" : "FALSE\n");
    return;
}

void print_robocopy_context(const robocopy_context *x)
{
    printf("source: %S\n", x->source);
    printf("destination: %S\n", x->destination);
    printf("files num %d:\n", x->num_files);
    for (int i = 0; i < x->num_files; i++)
        printf("    %S\n", x->files[i]);

    switch (x->copy_subdir)
    {
    case COPY_SUBDIR_TYPE_NOT:
        printf("copy sub dir: not\n");
        break;
    case COPY_SUBDIR_TYPE_EXCLUDE_EMPTY:
        printf("copy sub dir: /s, exclude empty\n");
        break;
    case COPY_SUBDIR_TYPE_INCLUDE_EMPTY:
        printf("copy sub dir: /e, include empty\n");
        break;
    }
    printf("purge_not_in_source: ");
    print_bool(x->purge_not_in_source);

    printf("exclude_file_patterns num %d:\n", x->num_exclude_file_patterns);
    for (int i = 0; i < x->num_exclude_file_patterns; i++)
    {
        printf("    %S\n", x->exclude_file_patterns[i]);
    }

    printf("exclude_directory_patterns num %d:\n", x->num_exclude_directory_patterns);
    for (int i = 0; i < x->num_exclude_directory_patterns; i++)
    {
        printf("    %S\n", x->exclude_directory_patterns[i]);
    }

    printf("exclude_older_file: ");
    print_bool(x->exclude_older_file);

    printf("num_retries_on_failed_copies: %d\n", x->num_retries_on_failed_copies);
    printf("wait_second_between_retries: %d\n", x->wait_second_between_retries);

    printf("not_log_file_size: ");
    print_bool(x->not_log_file_size);

    printf("not_log_file_class: ");
    print_bool(x->not_log_file_class);

    printf("not_display_progress: ");
    print_bool(x->not_display_progress);

    printf("not_log_file_names: ");
    print_bool(x->not_log_file_names);

    printf("not_log_directory_names: ");
    print_bool(x->not_log_directory_names);

    printf("no_job_header: ");
    print_bool(x->no_job_header);

    printf("not_job_summary: ");
    print_bool(x->not_job_summary);
    return;
}

#endif /* DEBUG_DEV_WEIWEN */

int robocopy_init_default_options(robocopy_context *x)
{
    x->source = NULL;
    x->destination = NULL;
    x->files = file_patterns_default_value;
    x->num_files = 1;

    /* copy options */
    x->copy_subdir = COPY_SUBDIR_TYPE_NOT;
    x->purge_not_in_source = FALSE;

    /* file selection options */
    x->exclude_file_patterns = NULL;
    x->num_exclude_file_patterns = 0;
    x->exclude_directory_patterns = NULL;
    x->num_exclude_directory_patterns = 0;
    x->exclude_older_file = FALSE;

    /* retry options */
    x->num_retries_on_failed_copies = 1000000;
    x->wait_second_between_retries = 30;

    /* logging options */
    x->not_log_file_size = FALSE;
    x->not_log_file_class = FALSE;
    x->not_display_progress = FALSE;
    x->not_log_file_names = FALSE;
    x->not_log_directory_names = FALSE;
    x->no_job_header = FALSE;
    x->not_job_summary = FALSE;

    /* job options */
    return INTERNAL_SUCCESS;
}

/* the state of ctx: first flag is parsed */
int robocopy_handle_flags(robocopy_context *x, gdos_context *ctx)
{
    int opt = ctx->current_opt;
    while ((opt = getopt_dos_next(ctx)) != GDOS_NEXT_ALL_OPTIONS_PROCEEDED)
    {
        switch (opt)
        {
        case I_COPY_SUBDIRECTORIES_EXCLUDE_EMPTY:
            x->copy_subdir = COPY_SUBDIR_TYPE_EXCLUDE_EMPTY;
            break;
        case I_COPY_SUBDIRECTORIES_INCLUDE_EMPTY:
            x->copy_subdir = COPY_SUBDIR_TYPE_INCLUDE_EMPTY;
            break;
        case I_PURGE_NOT_IN_SOURCE:
            x->purge_not_in_source = TRUE;
            break;
        case I_MIRROR_DIRECTORY_TREE:
            x->purge_not_in_source = TRUE;
            x->copy_subdir = COPY_SUBDIR_TYPE_INCLUDE_EMPTY;
            break;
        case I_EXCLUDE_FILES:
            x->exclude_file_patterns = ctx->current_opt_arg.list.argument_list;
            x->num_exclude_file_patterns = ctx->current_opt_arg.list.num_args;
            break;
        case I_EXCLUDE_DIRECTORY:
            x->exclude_directory_patterns = ctx->current_opt_arg.list.argument_list;
            x->num_exclude_directory_patterns = ctx->current_opt_arg.list.num_args;
            break;
        case I_EXCLUDE_OLDER_FILE:
            x->exclude_older_file = TRUE;
            break;
        case I_RETRY_NUM_ON_FAILED:
            /* TODO: check if not pass legal string */
            x->num_retries_on_failed_copies = _wtoi(ctx->current_opt_arg.single);
            break;
        case I_WAIT_TIME_BETWEEN_RETRIES:
            /* TODO: check if not pass legal string */
            x->wait_second_between_retries = _wtoi(ctx->current_opt_arg.single);
            break;
        case I_NO_LOG_FILE_SIZE:
            x->not_log_file_size = TRUE;
            break;
        case I_NO_LOG_FILE_CLASS:
            x->not_log_file_class = TRUE;
            break;
        case I_NO_LOG_COPY_PROGRESS:
            x->not_display_progress = TRUE;
            break;
        case I_NO_FILE_NAME_LOGGED:
            x->not_log_file_names = TRUE;
            break;
        case I_NO_DIRECTORY_NAME_LOGGED:
            x->not_log_directory_names = TRUE;
            break;
        case I_NO_JOB_HEADER:
            x->no_job_header = TRUE;
            break;
        case I_NO_JOB_SUMMARY:
            x->not_job_summary = TRUE;
            break;
        case GDOS_NEXT_NO_MATCHING_OPTION:
        case GDOS_NEXT_ARGUMENT_TYPE_MISMATCH:
        default:
            return INTERNAL_FAILURE;
        }
    }
    return INTERNAL_SUCCESS;
}

int robocopy_parse_options(robocopy_context *x, int argc, const WCHAR **argvW)
{
    gdos_context ctx;
    BOOL has_flag;
    int first_flag_ind;
    int file_list_end;

    /* TODO: handle this properly */
    if (getopt_dos_create_context(&ctx, argc, argvW, opts) != 0)
        return INTERNAL_FAILURE;

    first_flag_ind = 1;
    for (; first_flag_ind < argc; first_flag_ind++)
    {
        if (argvW[first_flag_ind][0] == '/')
        {
            has_flag = TRUE;
            break;
        }
    }

    file_list_end = has_flag ? first_flag_ind : argc;

    if (file_list_end < 3)
        return INTERNAL_FAILURE;
    x->source = argvW[1];
    x->destination = argvW[2];

    /* if <file>[...] not set, leave default value untouchable */
    if (file_list_end >= 4)
        x->num_files = file_list_end - 3;
        x->files = argvW + 3;

    if (has_flag) {
        ctx.optind = first_flag_ind;
        robocopy_handle_flags(x, &ctx);
    }
    return INTERNAL_SUCCESS;
}

int do_robocopy(robocopy_context *x)
{
    printf("after parse context:\n");
    print_robocopy_context(x);
    return ROBOCOPY_EXIT_SUCCESS;
}

int __cdecl wmain(int argc, WCHAR *argvW[])
{
    robocopy_context x;

    if (robocopy_init_default_options(&x) != INTERNAL_SUCCESS)
    {
        return INTERNAL_ERROR_PLACEHOLDER;
    }

    printf("default context:\n");
    print_robocopy_context(&x);

    if (robocopy_parse_options(&x, argc, (const WCHAR **)argvW) != INTERNAL_SUCCESS)
    {
        return INTERNAL_ERROR_PLACEHOLDER;
    }

    return do_robocopy(&x);
}
