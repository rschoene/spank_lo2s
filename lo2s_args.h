/*
 * lo2s_args.h — Argument validation for the lo2s daemon.
 *
 * Copyright (C) 2026  Robert Schoene
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * The daemon reads a single line from the FIFO and tokenizes it into argv.
 * Before exec'ing lo2s, we validate that the arguments match the expected
 * schema exactly. This prevents the job user (who owns the FIFO) from
 * injecting arbitrary lo2s arguments between prolog and init_post_opt.
 *
 * Allowed arguments (as constructed by init_post_opt):
 *   -q                    at most once, no value
 *   -A                    exactly once, no value
 *   --dwarf <val>         exactly once, value: non-empty string
 *   --cgroup <val>        exactly once, value: non-empty string
 *   -o <val>              exactly once, value: absolute path (starts with /)
 *   -e <val>              exactly once, value: non-empty string
 *   -c <val>              at most once, value: positive integer
 *   -E <val>              zero or more, value: non-empty string
 *   --metric-frequency <val>  at most once, value: positive integer
 *   --standard-metrics  at most once, no value
 *
 * Any other flag, duplicate (where not allowed), missing value, or
 * non-integer where integer is expected causes rejection.
 */

#ifndef LO2S_ARGS_H
#define LO2S_ARGS_H

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef enum {
    LO2S_ARGS_OK = 0,
    LO2S_ARGS_UNKNOWN_FLAG,
    LO2S_ARGS_DUPLICATE,
    LO2S_ARGS_MISSING_VALUE,
    LO2S_ARGS_BAD_INT,
    LO2S_ARGS_NOT_ABSOLUTE_PATH,
    LO2S_ARGS_MISSING_REQUIRED,
    LO2S_ARGS_EMPTY,
} lo2s_args_error_t;

static const char *lo2s_args_error_str(lo2s_args_error_t e) {
    switch (e) {
        case LO2S_ARGS_OK:                 return "ok";
        case LO2S_ARGS_UNKNOWN_FLAG:       return "unknown flag";
        case LO2S_ARGS_DUPLICATE:          return "duplicate flag";
        case LO2S_ARGS_MISSING_VALUE:      return "missing value";
        case LO2S_ARGS_BAD_INT:            return "invalid integer";
        case LO2S_ARGS_NOT_ABSOLUTE_PATH:  return "path must be absolute";
        case LO2S_ARGS_MISSING_REQUIRED:   return "missing required flag";
        case LO2S_ARGS_EMPTY:              return "empty argument list";
        default:                           return "unknown error";
    }
}

/*
 * Validate a tokenized argument array (excluding argv[0] which is the binary).
 *
 * @param args   NULL-terminated array of argument tokens (starting after the binary)
 * @param argc   number of tokens in args
 * @param err    output: which validation failed (if not OK)
 * @return 0 on success, -1 on validation failure
 */
static int lo2s_args_validate(const char *const args[], int argc, lo2s_args_error_t *err) {
    *err = LO2S_ARGS_OK;

    if (argc == 0) {
        *err = LO2S_ARGS_EMPTY;
        return -1;
    }

    // Track which flags we've seen
    int seen_q = 0, seen_A = 0, seen_dwarf = 0, seen_cgroup = 0;
    int seen_o = 0, seen_e = 0, seen_c = 0, seen_metric_freq = 0, seen_std_metrics = 0;

    int i = 0;
    while (i < argc) {
        const char *flag = args[i];

        if (strcmp(flag, "-q") == 0) {
            if (seen_q++) { *err = LO2S_ARGS_DUPLICATE; return -1; }
            i++;

        } else if (strcmp(flag, "-A") == 0) {
            if (seen_A++) { *err = LO2S_ARGS_DUPLICATE; return -1; }
            i++;

        } else if (strcmp(flag, "--dwarf") == 0) {
            if (seen_dwarf++) { *err = LO2S_ARGS_DUPLICATE; return -1; }
            if (++i >= argc) { *err = LO2S_ARGS_MISSING_VALUE; return -1; }
            if (args[i][0] == '\0') { *err = LO2S_ARGS_MISSING_VALUE; return -1; }
            i++;

        } else if (strcmp(flag, "--cgroup") == 0) {
            if (seen_cgroup++) { *err = LO2S_ARGS_DUPLICATE; return -1; }
            if (++i >= argc) { *err = LO2S_ARGS_MISSING_VALUE; return -1; }
            if (args[i][0] == '\0') { *err = LO2S_ARGS_MISSING_VALUE; return -1; }
            i++;

        } else if (strcmp(flag, "-o") == 0) {
            if (seen_o++) { *err = LO2S_ARGS_DUPLICATE; return -1; }
            if (++i >= argc) { *err = LO2S_ARGS_MISSING_VALUE; return -1; }
            // Must be an absolute path
            if (args[i][0] != '/') { *err = LO2S_ARGS_NOT_ABSOLUTE_PATH; return -1; }
            i++;

        } else if (strcmp(flag, "-e") == 0) {
            if (seen_e++) { *err = LO2S_ARGS_DUPLICATE; return -1; }
            if (++i >= argc) { *err = LO2S_ARGS_MISSING_VALUE; return -1; }
            if (args[i][0] == '\0') { *err = LO2S_ARGS_MISSING_VALUE; return -1; }
            i++;

        } else if (strcmp(flag, "-c") == 0) {
            if (seen_c++) { *err = LO2S_ARGS_DUPLICATE; return -1; }
            if (++i >= argc) { *err = LO2S_ARGS_MISSING_VALUE; return -1; }
            // Must be a positive integer
            char *endptr;
            long val = strtol(args[i], &endptr, 10);
            if (*endptr != '\0' || val <= 0) { *err = LO2S_ARGS_BAD_INT; return -1; }
            i++;

        } else if (strcmp(flag, "-E") == 0) {
            // Zero or more — no duplicate check
            if (++i >= argc) { *err = LO2S_ARGS_MISSING_VALUE; return -1; }
            if (args[i][0] == '\0') { *err = LO2S_ARGS_MISSING_VALUE; return -1; }
            i++;

        } else if (strcmp(flag, "--metric-frequency") == 0) {
            if (seen_metric_freq++) { *err = LO2S_ARGS_DUPLICATE; return -1; }
            if (++i >= argc) { *err = LO2S_ARGS_MISSING_VALUE; return -1; }
            char *endptr;
            long val = strtol(args[i], &endptr, 10);
            if (*endptr != '\0' || val <= 0) { *err = LO2S_ARGS_BAD_INT; return -1; }
            i++;

        } else if (strcmp(flag, "--standard-metrics") == 0) {
            if (seen_std_metrics++) { *err = LO2S_ARGS_DUPLICATE; return -1; }
            i++;

        } else {
            *err = LO2S_ARGS_UNKNOWN_FLAG;
            return -1;
        }
    }

    // Check required flags
    if (!seen_A)    { *err = LO2S_ARGS_MISSING_REQUIRED; return -1; }
    if (!seen_dwarf) { *err = LO2S_ARGS_MISSING_REQUIRED; return -1; }
    if (!seen_cgroup) { *err = LO2S_ARGS_MISSING_REQUIRED; return -1; }
    if (!seen_o)    { *err = LO2S_ARGS_MISSING_REQUIRED; return -1; }
    if (!seen_e)    { *err = LO2S_ARGS_MISSING_REQUIRED; return -1; }

    return 0;
}

#endif /* LO2S_ARGS_H */
