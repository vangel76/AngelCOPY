#include "getopt_dos.h"

static int token_is_option(const WCHAR *token, const WCHAR *option_name, unsigned int len)
{
    // token:        c:para_c
    // option_name:  c
    for (unsigned int i = 0; i < len; i++)
    {
        int c_t = tolower(token[i]);
        int c_o = tolower(option_name[i]);

        if (c_t == ':' && c_o == '\0')
            return 0;
        if (c_t < c_o)
            return -1;
        if (c_t > c_o)
            return 1;
        if (c_o == '\0' && c_t == '\0')
            return 0;
    }
    return 0;
}

int getopt_dos_create_context(gdos_context *ctx, int argc, const WCHAR **argv, const gdos_option *opts)
{
    ctx->argc = argc;
    ctx->argv = argv;
    ctx->opts = opts;
    ctx->optind = 1;

    /* reset opt-arg */
    memset(&(ctx->current_opt_arg), 0, sizeof(gdos_argument));

    /* count options */
    ctx->num_opts = 0;
    const gdos_option *opt = opts;
    while (!(opt->name == 0x0))
    {
        ctx->num_opts++;
        opt++;
    }
    return 0;
}

int getopt_dos_next(gdos_context *ctx)
{
    /* are all options processed? */
    if (ctx->optind == ctx->argc || ctx->argc <= 1)
        return GDOS_NEXT_ALL_OPTIONS_PROCEEDED;

    /* reset opt-arg */
    memset(&(ctx->current_opt_arg), 0, sizeof(gdos_argument));

    const WCHAR *current_token = ctx->argv[ctx->optind];

    /* this token has been processed! */
    ctx->optind++;

    if (current_token[0] && current_token[0] != '/')
    {
        return GDOS_NEXT_NOT_OPTION;
    }

    const gdos_option *found_opt = NULL;

    for (int i = 0; i < ctx->num_opts; i++)
    {
        const gdos_option *opt = ctx->opts + i;

        /* remove the leading slash */
        const WCHAR *token = current_token + 1;
        unsigned int token_len = (unsigned int)wcslen(token);

        if (token_is_option(token, opt->name, token_len) == 0)
        {
            unsigned int name_len = (unsigned int)wcslen(opt->name);
            if (token[name_len] == '\0')
            {
                if (!(opt->arg_type == NO_ARGUMENT || opt->arg_type == SPACE_SEPERATED_ARGUMENT_LIST))
                    return GDOS_NEXT_ARGUMENT_TYPE_MISMATCH;

                /* deal with possible argument list */
                if (!(ctx->optind == ctx->argc || ctx->argv[ctx->optind][0] == '/'))
                {

                    ctx->current_opt_arg.list.argument_list = ctx->argv + ctx->optind;

                    /* count list length, move optind */
                    const int ind_current_flag = ctx->optind;
                    while (ctx->optind < ctx->argc)
                    {
                        if (ctx->argv[ctx->optind][0] == '/')
                            break;
                        ctx->optind++;
                    }
                    ctx->current_opt_arg.list.num_args = ctx->optind - ind_current_flag;
                }
            }
            else if (token[name_len] == ':')
            {
                if (!(opt->arg_type == COLON_SEPERATED_ARGUMENT))
                    return GDOS_NEXT_ARGUMENT_TYPE_MISMATCH;
                ctx->current_opt_arg.single = token + name_len + 1;
            }
            else
            {
                printf("Bad token!\n");
                assert(0); /* this line should not be reached */
            }

            ctx->arg_type = opt->arg_type;
            ctx->current_opt = opt->val;
            return opt->val;
        }
    }

    return GDOS_NEXT_NO_MATCHING_OPTION;
}