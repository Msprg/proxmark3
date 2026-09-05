//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// API to abstract Readline / Linenoise support
//-----------------------------------------------------------------------------

#include "pm3line.h"
#include <stdlib.h>
#include <stdio.h> // for Mingw readline and for getline
#include <string.h>
#include <signal.h>
#if defined(HAVE_READLINE)
#include <readline/readline.h>
#include <readline/history.h>
#elif defined(HAVE_LINENOISE)
#include "linenoise.h"
#endif
#include "pm3line_vocabulary.h"
#include "pm3_cmd.h"
#include "ui.h"                          // g_session
#include "util.h"                        // str_ndup
#include "util_posix.h"                  // msleep

#if defined(HAVE_READLINE) || defined(HAVE_LINENOISE)

// What Tab should do for the word being typed, see complete_analyse()
typedef enum {
    COMPLETE_NONE,         // nothing sensible to offer
    COMPLETE_COMMAND,      // command names, from the vocabulary
    COMPLETE_COMMAND_HELP, // unambiguous leaf command: finish name, show its help
    COMPLETE_OPTION,       // option names of the command the line is for
    COMPLETE_FILE,         // a file name, left to the line editor
    COMPLETE_HELP,         // command without any parameter yet: show its help
} complete_kind_t;

typedef struct {
    complete_kind_t kind;
    // the word being completed is line[start..end)
    size_t start;
    size_t end;
    // command the line is for and its arguments, if known
    const vocabulary_t *cmd;
    const vocabulary_arg_t *args;
    size_t args_count;
    // COMPLETE_OPTION: the option names matching the word, in help order
    char **candidates;
    size_t candidates_count;
    // COMPLETE_COMMAND: when the command path is abbreviated and lands on a
    // category, the canonical category name whose children are the candidates
    // (e.g. "lf em 410x" for "lf em 41 "). Empty for the plain, literal case.
    char cmd_prefix[MAX_PM3_INPUT_ARGS_LENGTH];
} complete_t;

static complete_t s_complete = {0};

static void complete_reset(complete_t *c) {
    for (size_t i = 0; i < c->candidates_count; i++) {
        free(c->candidates[i]);
    }
    free(c->candidates);
    memset(c, 0, sizeof(*c));
}

// Add name to the candidates if it starts with word
static void complete_add_candidate(complete_t *c, const char *name, const char *word, size_t word_len) {

    if (strncmp(name, word, word_len) != 0) {
        return;
    }

    char **tmp = realloc(c->candidates, (c->candidates_count + 1) * sizeof(char *));
    if (tmp == NULL) {
        return;
    }
    c->candidates = tmp;
    c->candidates[c->candidates_count] = str_dup(name);
    if (c->candidates[c->candidates_count] != NULL) {
        c->candidates_count++;
    }
}

// Next whitespace separated token of line[*pos..to), quotes taken as CLIParser does.
static bool complete_next_token(const char *line, size_t to, size_t *pos, const char **token, size_t *token_len) {

    size_t i = *pos;
    while (i < to && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i >= to) {
        *pos = i;
        return false;
    }

    *token = line + i;
    char quote = 0;
    while (i < to) {
        if (quote) {
            if (line[i] == quote) {
                quote = 0;
            }
        } else if (line[i] == '"' || line[i] == '\'') {
            quote = line[i];
        } else if (line[i] == ' ' || line[i] == '\t') {
            break;
        }
        i++;
    }
    *token_len = (line + i) - *token;
    *pos = i;
    return true;
}

// Is name one of the comma separated longopts?
static bool complete_longopts_contain(const char *longopts, const char *name, size_t name_len) {
    const char *p = longopts;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == name_len && strncmp(p, name, name_len) == 0) {
            return true;
        }
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
    return false;
}

// The argument a token names: "-f", "--file" or "--file=value". NULL if none.
static const vocabulary_arg_t *complete_option_of(const complete_t *c, const char *token, size_t token_len) {

    if (token_len < 2 || token[0] != '-') {
        return NULL;
    }

    for (size_t i = 0; i < c->args_count; i++) {

        const vocabulary_arg_t *arg = &c->args[i];

        if (token[1] == '-') {
            const char *name = token + 2;
            size_t name_len = token_len - 2;
            const char *eq = memchr(name, '=', name_len);
            if (eq != NULL) {
                name_len = eq - name;
            }
            if (arg->longopts != NULL && complete_longopts_contain(arg->longopts, name, name_len)) {
                return arg;
            }
        } else if (token_len == 2) {
            if (arg->shortopts != NULL && strchr(arg->shortopts, token[1]) != NULL) {
                return arg;
            }
        }
    }
    return NULL;
}

typedef struct {
    const vocabulary_arg_t *value_of;
    size_t tokens;
    size_t positionals;
    bool options_done;
    int uses;
} complete_scan_t;

// Track option values and occurrences with the same token rules.
static complete_scan_t complete_scan(const complete_t *c, const char *line, size_t from, size_t to,
                                     const vocabulary_arg_t *count_arg) {
    complete_scan_t scan = {0};
    size_t pos = from;
    const char *token;
    size_t len;
    while (complete_next_token(line, to, &pos, &token, &len)) {
        scan.tokens++;
        if (scan.value_of != NULL) {
            scan.value_of = NULL;
            continue;
        }
        if (scan.options_done || token[0] != '-' || len == 1) {
            scan.positionals++;
            continue;
        }
        if (len == 2 && token[1] == '-') {
            scan.options_done = true;
            continue;
        }
        if (token[1] == '-') {
            const vocabulary_arg_t *arg = complete_option_of(c, token, len);
            if (arg != NULL) {
                scan.uses += (arg == count_arg);
                if (arg->has_value && memchr(token, '=', len) == NULL) {
                    scan.value_of = arg;
                }
            }
            continue;
        }
        for (size_t i = 1; i < len; i++) {
            char option[] = {'-', token[i], '\0'};
            const vocabulary_arg_t *arg = complete_option_of(c, option, 2);
            if (arg == NULL) {
                break;
            }
            scan.uses += (arg == count_arg);
            if (arg->has_value) {
                if (i + 1 == len) {
                    scan.value_of = arg;
                }
                break;
            }
        }
    }
    return scan;
}

// Decide what Tab should do for the word line[start..end).
//
// The rules, in order:
// - "!..." is a shell command: complete file names.
// - the word is (part of) a command name: complete command names; if the name
//   is unambiguous and finishes a leaf command, also show that command's help.
// - the line is not for a known command: nothing.
// - the word is the value of an option: a file name if the option takes
//   one, nothing otherwise (no file names for keys, block numbers, ...).
// - no parameter given yet: show the help of the command.
// - the word is a plain value: a file name if the command takes one as a
//   positional argument, nothing otherwise.
// - otherwise: the option names of the command, not given yet.
static void complete_analyse(complete_t *c, const char *line, size_t start, size_t end) {

    complete_reset(c);
    c->start = start;
    c->end = end;

    if (line[0] == '!') {
        c->kind = COMPLETE_FILE;
        return;
    }

    // Is the word (still) part of a command name? Look at every available
    // command whose name starts with the line up to the cursor and, along the
    // way, work out what the completion of the current word would be: the token
    // that starts at `start` and runs to the next space or the end of the name.
    // If that token is the same for all of them the completion is unambiguous;
    // if on top of that it finishes a leaf command with nothing below it, we
    // can finish the name and show its help in one keystroke.
    size_t count = 0;
    const vocabulary_t *vocabulary = pm3line_vocabulary_get(&count);
    bool is_command = false;
    const char *ctoken = NULL;             // completion of the current word
    size_t ctoken_len = 0;
    bool ctoken_unique = true;
    const vocabulary_t *leaf = NULL;       // command the token completes, if leaf
    bool ctoken_has_child = false;         // some name continues past the token

    for (size_t i = 0; i < count; i++) {

        const vocabulary_t *entry = &vocabulary[i];
        if (strncmp(entry->name, line, end) != 0 || pm3line_vocabulary_is_available(entry) == false) {
            continue;
        }
        is_command = true;

        const char *this_token = entry->name + start;
        const char *space = strchr(this_token, ' ');
        size_t this_len = space ? (size_t)(space - this_token) : strlen(this_token);

        if (ctoken == NULL) {
            ctoken = this_token;
            ctoken_len = this_len;
        } else if (this_len != ctoken_len || strncmp(this_token, ctoken, ctoken_len) != 0) {
            ctoken_unique = false;
        }

        if (space != NULL) {
            ctoken_has_child = true;
        } else if (entry->cmd != NULL) {
            leaf = entry;
        }
    }

    if (is_command) {
        c->kind = COMPLETE_COMMAND;
        // An unambiguous leaf command also gets its help shown in the same Tab,
        // but only when it has parameters to reveal; one whose arguments we can't
        // introspect (e.g. "help") is just completed, without a help dump.
        if (ctoken_unique && ctoken_has_child == false && leaf != NULL) {
            c->cmd = leaf;
            pm3line_vocabulary_get_args(leaf, &c->args, &c->args_count);
            if (c->args_count > 0) {
                c->kind = COMPLETE_COMMAND_HELP;
            }
        }
        return;
    }

    // An abbreviated command path that lands on a category, followed by a space
    // or the start of a subcommand ("lf em 41 ", "lf t55 vi"): none of the
    // literal command names start with the abbreviation, but the path resolves.
    // Offer the category's subcommands, matched against the canonical name.
    if (pm3line_vocabulary_resolve_category(line, start, c->cmd_prefix, sizeof(c->cmd_prefix))) {
        c->kind = COMPLETE_COMMAND;
        return;
    }

    size_t args_offset = 0;
    c->cmd = pm3line_vocabulary_find_command(line, start, &args_offset);
    if (c->cmd == NULL) {
        c->kind = COMPLETE_NONE;
        return;
    }
    pm3line_vocabulary_get_args(c->cmd, &c->args, &c->args_count);

    // Find the whole token even when readline starts after a quote or '='.
    size_t token_start = start;
    size_t pos = args_offset;
    const char *token;
    size_t token_len;
    while (complete_next_token(line, end, &pos, &token, &token_len)) {
        if ((size_t)(token - line) <= start && pos >= start) {
            token_start = token - line;
            break;
        }
    }
    complete_scan_t scan = complete_scan(c, line, args_offset, token_start, NULL);
    const vocabulary_arg_t *value_of = scan.value_of;
    size_t tokens = scan.tokens;
    size_t positionals = scan.positionals;

    if (!scan.options_done && value_of == NULL && token_start < end && line[token_start] == '-') {
        const char *current = line + token_start;
        size_t len = end - token_start;
        const vocabulary_arg_t *arg = complete_option_of(c, current, len);
        if (len > 2 && current[1] == '-') {
            if (arg != NULL && memchr(current, '=', len) != NULL) {
                value_of = arg;
            }
        } else {
            for (size_t i = 1; i < len; i++) {
                char option[] = {'-', current[i], '\0'};
                arg = complete_option_of(c, option, 2);
                if (arg == NULL) {
                    break;
                }
                if (arg->has_value) {
                    if (i + 1 < len) {
                        value_of = arg;
                    }
                    break;
                }
            }
        }
    }

    const char *word = line + start;
    size_t word_len = end - start;

    if (value_of != NULL) {
        // The editor must replace only the value, never an attached option name.
        if (scan.value_of == NULL && start == token_start) {
            c->kind = COMPLETE_NONE;
            return;
        }
        c->kind = value_of->is_file ? COMPLETE_FILE : COMPLETE_NONE;
        return;
    }

    if (tokens == 0 && word_len == 0) {
        // No parameter typed yet: show the command's help to reveal its
        // parameters. Only worth it when it actually has some; commands whose
        // arguments we can't introspect (non-CLIParser ones like "help" or
        // "reveng") have nothing to reveal, so Tab does nothing there.
        c->kind = (c->args_count > 0) ? COMPLETE_HELP : COMPLETE_NONE;
        return;
    }

    if (scan.options_done || (word_len > 0 && word[0] != '-')) {
        // the positional argument the word is for, if any
        size_t seen = 0;
        for (size_t i = 0; i < c->args_count; i++) {
            const vocabulary_arg_t *arg = &c->args[i];
            if (arg->shortopts != NULL || arg->longopts != NULL) {
                continue;
            }
            seen += (arg->maxcount > 0) ? (size_t)arg->maxcount : 1;
            if (seen > positionals) {
                c->kind = arg->is_file ? COMPLETE_FILE : COMPLETE_NONE;
                return;
            }
        }
        c->kind = COMPLETE_NONE;
        return;
    }

    bool want_long = (word_len >= 2 && word[1] == '-');
    for (size_t i = 0; i < c->args_count; i++) {

        const vocabulary_arg_t *arg = &c->args[i];
        if (arg->shortopts == NULL && arg->longopts == NULL) {
            continue;
        }
        if (arg->maxcount > 0 && complete_scan(c, line, args_offset, token_start, arg).uses >= arg->maxcount) {
            continue;
        }
        // Don't push the help option on someone who didn't start typing an
        // option, it would get inserted when it's the only one left
        if (word_len == 0 && arg->shortopts != NULL && strcmp(arg->shortopts, "h") == 0
                && arg->longopts != NULL && strcmp(arg->longopts, "help") == 0) {
            continue;
        }

        char name[128] = {0};
        if (want_long == false && arg->shortopts != NULL) {
            for (const char *s = arg->shortopts; *s; s++) {
                snprintf(name, sizeof(name), "-%c", *s);
                complete_add_candidate(c, name, word, word_len);
            }
            continue;
        }

        const char *l = arg->longopts;
        while (l != NULL && *l) {
            const char *comma = strchr(l, ',');
            int len = comma ? (int)(comma - l) : (int)strlen(l);
            snprintf(name, sizeof(name), "--%.*s", len, l);
            complete_add_candidate(c, name, word, word_len);
            if (comma == NULL) {
                break;
            }
            l = comma + 1;
        }
    }

    c->kind = c->candidates_count ? COMPLETE_OPTION : COMPLETE_NONE;
}

#endif // HAVE_READLINE || HAVE_LINENOISE

#if defined(HAVE_READLINE)

// GNU readline, as opposed to the libedit emulation (see ui.c)
#if defined(RL_READLINE_VERSION) && (RL_READLINE_VERSION >= 0x0600) && defined(RL_STATE_READCMD)
#define HAVE_GNU_READLINE
#endif

// Syntax of an argument as shown in the help, e.g. "-f, --file <fn>"
static void complete_arg_syntax(const vocabulary_arg_t *arg, char *dst, size_t dst_len) {

    size_t pos = 0;
    dst[0] = '\0';

    for (const char *s = arg->shortopts; s != NULL && *s && pos < dst_len; s++) {
        pos += snprintf(dst + pos, dst_len - pos, "%s-%c", pos ? ", " : "", *s);
    }

    const char *l = arg->longopts;
    while (l != NULL && *l && pos < dst_len) {
        const char *comma = strchr(l, ',');
        int len = comma ? (int)(comma - l) : (int)strlen(l);
        pos += snprintf(dst + pos, dst_len - pos, "%s--%.*s", pos ? ", " : "", len, l);
        if (comma == NULL) {
            break;
        }
        l = comma + 1;
    }

    if (arg->datatype != NULL && pos < dst_len) {
        snprintf(dst + pos, dst_len - pos, "%s%s", pos ? " " : "", arg->datatype);
    }
}

static char *rl_command_generator(const char *text, int state) {
    static size_t index;
    static size_t count;
    static const vocabulary_t *vocabulary;
    (void) text;

    if (!state) {
        index = 0;
        vocabulary = pm3line_vocabulary_get(&count);
    }

    // Abbreviated category path: match the subcommands of the canonical category
    // name against the word, returning just the child token so it replaces the
    // word (the already typed, possibly abbreviated, prefix is left as it is).
    if (s_complete.cmd_prefix[0] != '\0') {
        size_t plen = strlen(s_complete.cmd_prefix);
        const char *word = rl_line_buffer + s_complete.start;
        size_t word_len = s_complete.end - s_complete.start;

        while (index < count) {
            const vocabulary_t *entry = &vocabulary[index++];
            if (pm3line_vocabulary_is_available(entry) == false) {
                continue;
            }
            const char *command = entry->name;
            if (strncmp(command, s_complete.cmd_prefix, plen) != 0 || command[plen] != ' ') {
                continue;
            }
            const char *child = command + plen + 1;
            const char *space = strchr(child, ' ');
            size_t child_len = space ? (size_t)(space - child) : strlen(child);
            if (word_len > child_len || strncmp(child, word, word_len) != 0) {
                continue;
            }
            return str_ndup(child, child_len);
        }
        return NULL;
    }

    while (index < count) {

        const vocabulary_t *entry = &vocabulary[index++];

        // Skip commands which are not available right now,
        // using the same rules as "help"
        if (pm3line_vocabulary_is_available(entry) == false) {
            continue;
        }

        const char *command = entry->name;

        if (strncmp(command, rl_line_buffer, s_complete.end) == 0) {
            const char *next = command + s_complete.start;
            const char *space = strstr(next, " ");
            if (space != NULL) {
                return str_ndup(next, space - next);
            }
            return str_dup(next);
        }
    }

    return NULL;
}

static char *rl_option_generator(const char *text, int state) {
    static size_t index;
    (void) text;    // the candidates were already filtered on the word

    if (!state) {
        index = 0;
    }

    if (index < s_complete.candidates_count) {
        return str_dup(s_complete.candidates[index++]);
    }
    return NULL;
}

// Preserve the word on a no-op. Only GNU readline can suppress the trailing
// space it would otherwise append, so elsewhere this falls back to the bell.
static char **rl_no_op_match(const char *text) {
#if defined(HAVE_GNU_READLINE)
    char **matches = calloc(2, sizeof(char *));
    if (matches != NULL) {
        matches[0] = str_dup(text);
        if (matches[0] != NULL) {
            rl_completion_suppress_append = 1;
            rl_completion_suppress_quote = 1;
            return matches;
        }
        free(matches);
    }
#endif
    (void) text;
    return NULL;
}

// Reprint the prompt to rl_outstream, dropping readline's markers for the
// non-printing parts of it (\001 .. \002), which are not meant to be emitted.
static void rl_reprint_prompt(void) {
    for (const char *p = rl_prompt; p != NULL && *p != '\0'; p++) {
        if (*p != RL_PROMPT_START_IGNORE && *p != RL_PROMPT_END_IGNORE) {
            fputc(*p, rl_outstream);
        }
    }
}

// Visible width of the prompt, i.e. its length minus the non-printing parts
// readline marks with \001 .. \002 (RL_PROMPT_START_IGNORE/END_IGNORE).
static size_t rl_visible_prompt_width(void) {
    size_t width = 0;
    bool ignoring = false;
    for (const char *p = rl_prompt; p != NULL && *p != '\0'; p++) {
        if (*p == RL_PROMPT_START_IGNORE) {
            ignoring = true;
        } else if (*p == RL_PROMPT_END_IGNORE) {
            ignoring = false;
        } else if (ignoring == false) {
            width++;
        }
    }
    return width;
}

// Briefly turn the typed line red, as feedback that Tab found nothing to offer.
// Returns false (nothing flashed) when there is no input or no color support, so
// the caller can fall back to the bell.
//
// This is done with self contained terminal escapes and leaves the screen (and
// the cursor) exactly as readline last drew it, so readline's own display state
// stays consistent and repeated presses don't pile up. That relies on the prompt
// and input fitting on one screen row: '\r' only homes the current row and the
// cursor is restored with a single horizontal move. When the line has wrapped to
// more than one row those escapes would corrupt the display, so we bail out and
// let the caller ring the bell instead.
static bool rl_flash_input(void) {

    if (rl_end <= 0 || g_session.supports_colors == false) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)rl_line_buffer; *p; p++) {
        if (*p < 32 || *p >= 127) {
            return false;
        }
    }

    // bail if the prompt plus the input does not fit on a single screen row
    int rows = 0, cols = 0;
    rl_get_screen_size(&rows, &cols);
    if (cols > 0 && rl_visible_prompt_width() + (size_t)rl_end >= (size_t)cols) {
        return false;
    }

    // redraw the line red, hold, then redraw it in its normal colors
    fputc('\r', rl_outstream);
    rl_reprint_prompt();
    fprintf(rl_outstream, ANSI_RED "%s" AEND, rl_line_buffer);
    fflush(rl_outstream);
    msleep(120);

    fputc('\r', rl_outstream);
    rl_reprint_prompt();
    fprintf(rl_outstream, "%s", rl_line_buffer);

    // put the cursor back where it was inside the line
    if (rl_point < rl_end) {
        fprintf(rl_outstream, "\x1b[%dD", rl_end - rl_point);
    }
    fflush(rl_outstream);
    return true;
}

static char **rl_command_completion(const char *text, int start, int end) {

    complete_analyse(&s_complete, rl_line_buffer, (size_t)start, (size_t)end);

    // No file names unless asked for below
    rl_attempted_completion_over = 1;
#if defined(HAVE_GNU_READLINE)
    rl_sort_completion_matches = 1;
#endif

    switch (s_complete.kind) {

        case COMPLETE_COMMAND: {
            char **matches = rl_completion_matches(text, rl_command_generator);
            if (matches == NULL) {
                rl_flash_input();
            }
            return matches;
        }

        case COMPLETE_COMMAND_HELP: {
            // Finish the (unambiguous) command name and show its help, in one
            // Tab. Show the help first, then hand readline the completed word
            // as a single match, so it finishes the name, appends the usual
            // space and repaints the prompt below the help.
            fputc('\n', rl_outstream);
            pm3line_vocabulary_print_help(s_complete.cmd);
            rl_on_new_line();

            char **matches = calloc(2, sizeof(char *));
            if (matches != NULL) {
                matches[0] = str_dup(s_complete.cmd->name + s_complete.start);
                if (matches[0] != NULL) {
                    return matches;
                }
                free(matches);
            }
            return NULL;
        }

        case COMPLETE_OPTION: {
#if defined(HAVE_GNU_READLINE)
            // keep the help order
            rl_sort_completion_matches = 0;
#endif
            return rl_completion_matches(text, rl_option_generator);
        }

        case COMPLETE_FILE: {
            rl_attempted_completion_over = 0;
            return NULL;
        }

        case COMPLETE_HELP: {
            // Show the help below the prompt, then bring the prompt back. The
            // help is the answer, so hand back an empty match rather than let
            // readline ring the bell.
            fputc('\n', rl_outstream);
            pm3line_vocabulary_print_help(s_complete.cmd);
            rl_on_new_line();
            return rl_no_op_match(text);
        }

        case COMPLETE_NONE:
        default: {
            rl_flash_input();
            // Let readline ring the bell; a synthetic match can be listed on Tab.
            return NULL;
        }
    }
}

// Show ambiguous options the way the help does, with their description
static void rl_display_matches(char **matches, int num_matches, int max_length) {

    if (s_complete.kind != COMPLETE_OPTION) {
#if defined(HAVE_GNU_READLINE)
        // Same question readline asks on its own before a long list
        if (rl_completion_query_items > 0 && num_matches >= rl_completion_query_items) {
            fprintf(rl_outstream, "\nDisplay all %d possibilities? (y or n)", num_matches);
            fflush(rl_outstream);
            int key = rl_read_key();
            if (key != 'y' && key != 'Y' && key != ' ') {
                fputc('\n', rl_outstream);
                rl_forced_update_display();
                return;
            }
        }
#endif
        rl_display_match_list(matches, num_matches, max_length);
        rl_forced_update_display();
        return;
    }

    fputc('\n', rl_outstream);

    uint8_t old_printAndLog = g_printAndLog;
    g_printAndLog &= PRINTANDLOG_PRINT;

    for (int i = 1; i <= num_matches; i++) {
        const vocabulary_arg_t *arg = complete_option_of(&s_complete, matches[i], strlen(matches[i]));
        if (arg == NULL) {
            PrintAndLogEx(NORMAL, "    %s", matches[i]);
            continue;
        }
        char syntax[128] = {0};
        complete_arg_syntax(arg, syntax, sizeof(syntax));
        PrintAndLogEx(NORMAL, "    %-30s %s", syntax, arg->glossary ? arg->glossary : "");
    }

    g_printAndLog = old_printAndLog;
    rl_forced_update_display();
}

#elif defined(HAVE_LINENOISE)
// text is the whole line (see the patch in deps/get_linenoise.sh) and a
// completion replaces the whole line.
static void ln_command_completion(const char *text, linenoiseCompletions *lc) {
    const char *prev_match = "";
    size_t prev_match_len = 0;
    size_t len = strlen(text);
    size_t start = len;
    while (start > 0 && text[start - 1] != ' ' && text[start - 1] != '\t') {
        start--;
    }

    complete_analyse(&s_complete, text, start, len);

    if (s_complete.kind == COMPLETE_OPTION) {
        for (size_t i = 0; i < s_complete.candidates_count; i++) {
            char line[MAX_PM3_INPUT_ARGS_LENGTH] = {0};
            int n = snprintf(line, sizeof(line), "%.*s%s", (int)start, text, s_complete.candidates[i]);
            if (n > 0 && (size_t)n < sizeof(line)) {
                linenoiseAddCompletion(lc, line);
            }
        }
        return;
    }

    // Help and file names are not supported by linenoise, so a command that
    // would finish and show its help just finishes here (COMPLETE_COMMAND_HELP)
    if (s_complete.kind != COMPLETE_COMMAND && s_complete.kind != COMPLETE_COMMAND_HELP) {
        return;
    }

    size_t count = 0;
    const vocabulary_t *vocabulary = pm3line_vocabulary_get(&count);

    // Abbreviated category path ("lf em 41 "): offer each subcommand of the
    // canonical category, keeping the typed (abbreviated) prefix in the line.
    if (s_complete.cmd_prefix[0] != '\0') {
        size_t plen = strlen(s_complete.cmd_prefix);
        const char *word = text + start;
        size_t word_len = len - start;
        for (size_t index = 0; index < count; index++) {
            const vocabulary_t *entry = &vocabulary[index];
            if (pm3line_vocabulary_is_available(entry) == false) {
                continue;
            }
            const char *command = entry->name;
            if (strncmp(command, s_complete.cmd_prefix, plen) != 0 || command[plen] != ' ') {
                continue;
            }
            const char *child = command + plen + 1;
            const char *space = strchr(child, ' ');
            size_t child_len = space ? (size_t)(space - child) : strlen(child);
            if (word_len > child_len || strncmp(child, word, word_len) != 0) {
                continue;
            }
            char line[MAX_PM3_INPUT_ARGS_LENGTH] = {0};
            int n = snprintf(line, sizeof(line), "%.*s%.*s", (int)start, text, (int)child_len, child);
            if (n > 0 && (size_t)n < sizeof(line)) {
                linenoiseAddCompletion(lc, line);
            }
        }
        return;
    }

    for (size_t index = 0; index < count; index++) {

        const vocabulary_t *entry = &vocabulary[index];

        // Skip commands which are not available right now,
        // using the same rules as "help"
        if (pm3line_vocabulary_is_available(entry) == false) {
            continue;
        }

        const char *command = entry->name;

        if (strncmp(command, text, len) == 0) {
            const char *space = strstr(command + len, " ");
            if (space != NULL) {
                if ((prev_match_len == 0) || (strncmp(prev_match, command, prev_match_len < space - command ? prev_match_len : space - command) != 0)) {
                    char *partial = str_ndup(command, space - command + 1);
                    if (partial != NULL) {
                        linenoiseAddCompletion(lc, partial);
                        free(partial);
                    }
                    prev_match = command;
                    prev_match_len = space - command + 1;
                }
            } else {
                linenoiseAddCompletion(lc, command);
            }
        }
    }
}
#endif // HAVE_READLINE

#  if defined(_WIN32)
/*
static bool WINAPI terminate_handler(DWORD t) {
    if (t == CTRL_C_EVENT) {
        flush_history();
        return true;
    }
    return false;
}
*/
#  else
static struct sigaction gs_old_sigint_action;
static void sigint_handler(int signum) {

    switch (signum) {
        case SIGINT: {
            sigaction(SIGINT, &gs_old_sigint_action, NULL);
            pm3line_flush_history();
            kill(0, SIGINT);
            break;
        }
        default: {
            break;
        }
    }
}

#endif

void pm3line_install_signals(void) {
#  if defined(_WIN32)
//    SetConsoleCtrlHandler((PHANDLER_ROUTINE)terminate_handler, true);
#  else
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = &sigint_handler;
    sigaction(SIGINT, &action, &gs_old_sigint_action);
#  endif

#if defined(HAVE_READLINE)
    rl_catch_signals = 1;
    rl_set_signals();
#endif // HAVE_READLINE
}

void pm3line_init(void) {
#if defined(HAVE_READLINE) || defined(HAVE_LINENOISE)
    // Build the completion vocabulary from the live command tree
    pm3line_vocabulary_build();
#endif
#if defined(HAVE_READLINE)
    /* initialize history */
    using_history();
    rl_readline_name = "PM3";
    rl_completer_quote_characters = "\"'";
    rl_attempted_completion_function = rl_command_completion;
    rl_completion_display_matches_hook = rl_display_matches;

// don't hook signal in MINGW
#if defined(__MINGW32__) || defined(__MINGW64__)
#else
    rl_getc_function = getc;
#endif

    pm3line_install_signals();

#ifdef RL_STATE_READCMD
    rl_extend_line_buffer(1024);
#endif // RL_STATE_READCMD
#elif defined(HAVE_LINENOISE)
    linenoiseInstallWindowChangeHandler();
    linenoiseSetCompletionCallback(ln_command_completion);
#endif // HAVE_READLINE
}

char *pm3line_read(const char *s) {
#if defined(HAVE_READLINE)
    return readline(s);
#elif defined(HAVE_LINENOISE)
    return linenoise(s);
#else
    printf("%s", s);
    // MinGW/ProxSpace builds do not provide getline() in this fallback path.
    char input[1024] = {0};
    if (fgets(input, sizeof(input), stdin) == NULL) {
        return NULL;
    }

    size_t len = strlen(input);
    while (len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r')) {
        input[--len] = '\0';
    }

    char *answer = calloc(len + 1, sizeof(char));
    if (answer == NULL) {
        return NULL;
    }

    memcpy(answer, input, len);
    return answer;
#endif
}

void pm3line_free(void *ref) {
    free(ref);
}

void pm3line_cleanup(void) {
#if defined(HAVE_READLINE) || defined(HAVE_LINENOISE)
    complete_reset(&s_complete);
#endif
    pm3line_vocabulary_free();
}

void pm3line_update_prompt(const char *prompt) {
#if defined(HAVE_READLINE)
    rl_set_prompt(prompt);
    rl_forced_update_display();
#else
    (void) prompt;
#endif
}

int pm3line_load_history(const char *path) {
#if defined(HAVE_READLINE)
    if (read_history(path) == 0) {
        return PM3_SUCCESS;
    } else {
        return PM3_ESOFT;
    }
#elif defined(HAVE_LINENOISE)
    if (linenoiseHistoryLoad(path) == 0) {
        return PM3_SUCCESS;
    } else {
        return PM3_ESOFT;
    }
#else
    (void) path;
    return PM3_ENOTIMPL;
#endif
}

void pm3line_add_history(const char *line) {
#if defined(HAVE_READLINE)
    HIST_ENTRY *entry = history_get(history_length);
    // add if not identical to latest recorded line
    if ((!entry) || (strcmp(entry->line, line) != 0)) {
        add_history(line);
    }
#elif defined(HAVE_LINENOISE)
    // linenoiseHistoryAdd takes already care of duplicate entries
    linenoiseHistoryAdd(line);
#else
    (void) line;
#endif
}

void pm3line_flush_history(void) {
    if (g_session.history_path) {
#if defined(HAVE_READLINE)
        write_history(g_session.history_path);
#elif defined(HAVE_LINENOISE)
        linenoiseHistorySave(g_session.history_path);
#endif // HAVE_READLINE
        free(g_session.history_path);
        g_session.history_path = NULL;
    }
}

void pm3line_check(int (check)(void)) {
#if defined(HAVE_READLINE)
    rl_event_hook = check;
#else
    check();
#endif
}

// TODO:
// src/ui.c print_progress()
