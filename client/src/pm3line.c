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
#ifndef _WIN32
#include <unistd.h>                      // write, isatty, STDOUT_FILENO
#endif
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
#include "frame_data2.h"                 // fx_terminal_restore

static void pm3line_claim_signals(void);

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

// How many times an argument is given in line[from..to)
static int complete_option_uses(const complete_t *c, const char *line, size_t from, size_t to, const vocabulary_arg_t *arg) {
    int uses = 0;
    size_t pos = from;
    const char *token = NULL;
    size_t token_len = 0;
    while (complete_next_token(line, to, &pos, &token, &token_len)) {
        if (complete_option_of(c, token, token_len) == arg) {
            uses++;
        }
    }
    return uses;
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

    // Parameters given so far: the option the word would be the value of,
    // and how many positional values came before the word
    const vocabulary_arg_t *value_of = NULL;
    size_t tokens = 0;
    size_t positionals = 0;
    size_t pos = args_offset;
    const char *token = NULL;
    size_t token_len = 0;
    while (complete_next_token(line, start, &pos, &token, &token_len)) {
        tokens++;

        // the value of the option before it
        if (value_of != NULL) {
            value_of = NULL;
            continue;
        }

        const vocabulary_arg_t *arg = complete_option_of(c, token, token_len);
        if (arg != NULL) {
            // "--file=value" carries its value, except "--file=" right before the word
            const char *eq = (token[1] == '-') ? memchr(token, '=', token_len) : NULL;
            if (arg->has_value && (eq == NULL || (eq == token + token_len - 1 && token + token_len == line + start))) {
                value_of = arg;
            }
            continue;
        }

        if (token_len > 1 && token[0] == '-') {
            // an option we don't know of
            continue;
        }
        positionals++;
    }

    const char *word = line + start;
    size_t word_len = end - start;

    if (value_of != NULL) {
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

    if (word_len > 0 && word[0] != '-') {
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
        if (arg->maxcount > 0 && complete_option_uses(c, line, args_offset, start, arg) >= arg->maxcount) {
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

// An empty completion match: readline inserts nothing and, unlike a NULL
// return, does not ring the bell. Only GNU readline can suppress the trailing
// space it would otherwise append, so elsewhere this falls back to the bell.
static char **rl_no_op_match(void) {
#if defined(HAVE_GNU_READLINE)
    char **matches = calloc(2, sizeof(char *));
    if (matches != NULL) {
        matches[0] = str_dup("");
        if (matches[0] != NULL) {
            rl_completion_suppress_append = 1;
            return matches;
        }
        free(matches);
    }
#endif
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
            return rl_completion_matches(text, rl_command_generator);
        }

        case COMPLETE_COMMAND_HELP: {
            // Finish the (unambiguous) command name and show its help, in one
            // Tab. Show the help first, then hand readline the completed word
            // as a single match, so it finishes the name, appends the usual
            // space and repaints the prompt below the help.
            fputc('\n', rl_outstream);
            pm3line_vocabulary_print_help(s_complete.cmd);
            rl_forced_update_display();

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
            rl_forced_update_display();
            return rl_no_op_match();
        }

        case COMPLETE_NONE:
        default: {
            // Nothing to offer. The "nothing" feedback (a red blink, see
            // pm3_complete()) is given by our Tab handler, which intercepts this
            // case before readline would ring the bell; reaching here anyway,
            // just do nothing.
            return NULL;
        }
    }
}

// Tab handler. It wraps readline's completion so the "nothing to offer" case can
// blink the input red instead of ringing the bell, without readline redrawing
// the prompt on top (which piles up on repeated presses). Everything else is
// left to rl_complete(), which drives rl_command_completion() as usual.
static int pm3_complete(int count, int key) {

    // find the current word the way rl_complete() does, using readline's own
    // word break characters, so our decision matches the real completion
    int end = rl_point;
    int start = rl_point;
    const char *breaks = rl_completer_word_break_characters ? rl_completer_word_break_characters : " \t\n";
    while (start > 0 && strchr(breaks, rl_line_buffer[start - 1]) == NULL) {
        start--;
    }

    complete_analyse(&s_complete, rl_line_buffer, (size_t)start, (size_t)end);

    if (s_complete.kind == COMPLETE_NONE) {
        // ring the bell (audible cue) and flash the input red (visible cue);
        // the flash redraws last so the cursor is left in the right place
        rl_ding();
        rl_flash_input();
        return 0;
    }

    return rl_complete(count, key);
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

static int (*gs_check_hook)(void) = NULL;

static int pm3line_startup_hook(void) {
    pm3line_claim_signals();
    return 0;
}

#elif defined(HAVE_LINENOISE)
// text is the whole line (see the patch in deps/get_linenoise.sh) and a
// completion replaces the whole line.
static void ln_command_completion(const char *text, linenoiseCompletions *lc) {
    const char *prev_match = "";
    size_t prev_match_len = 0;
    size_t len = strlen(text);
    size_t start = len;
    while (start > 0 && text[start - 1] != ' ') {
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

static volatile sig_atomic_t gs_sigint_caught = 0;
static volatile sig_atomic_t gs_at_prompt = 0;

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
static struct sigaction gs_old_sigtstp_action;
static volatile sig_atomic_t gs_echo_ctrl_c = 0;
static void sigtstp_handler(int signum);
static void sigint_handler(int signum) {

    switch (signum) {
        case SIGINT: {
            // Second CTRL-C. The graceful path did not take, restore the
            // default disposition and let this one through.
            if (gs_sigint_caught) {
                fx_terminal_restore();
                sigaction(SIGINT, &gs_old_sigint_action, NULL);
                raise(SIGINT);
                break;
            }
            // Only set a flag here. Saving the history means malloc and stdio,
            // neither is safe to call from a signal handler in a threaded
            // client. write() is, and the terminal no longer echoes the
            // character for us since readline turned ECHO off
            gs_sigint_caught = 1;

            if (gs_echo_ctrl_c) {
                static const char at_prompt[] = "^C";
                // the terminal is out of raw mode while a command runs, so it
                // echoed the ^C itself. Only say what happens next
                static const char in_command[] = "\nquitting once this command is done. CTRL-C again to force\n";
                ssize_t ignored;
                if (gs_at_prompt) {
                    ignored = write(STDOUT_FILENO, at_prompt, sizeof(at_prompt) - 1);
                } else {
                    ignored = write(STDOUT_FILENO, in_command, sizeof(in_command) - 1);
                }
                (void) ignored;
            }
            break;
        }
        default: {
            break;
        }
    }
}

// CTRL-Z. Put the terminal back the way the shell expects it, stop for real
// on this thread, and set the line editor up again once we are continued
static void sigtstp_handler(int signum) {

    if (signum != SIGTSTP) {
        return;
    }

    int at_prompt = gs_at_prompt;
    (void) at_prompt;

    fx_terminal_restore();

#if defined(HAVE_READLINE)
    if (at_prompt) {
        rl_cleanup_after_signal();
    }
#endif

    sigaction(SIGTSTP, &gs_old_sigtstp_action, NULL);

    sigset_t set;
    sigprocmask(SIG_BLOCK, NULL, &set);
    sigdelset(&set, SIGTSTP);

    raise(SIGTSTP);

    // the signal raised above is blocked while we are inside the handler,
    // unblocking it is what stops us. We resume here on SIGCONT
    sigprocmask(SIG_SETMASK, &set, NULL);

    pm3line_claim_signals();
    fx_terminal_resume();

#if defined(HAVE_READLINE)
    if (at_prompt) {
        rl_reset_after_signal();
    }
#endif
}

// Leave the terminal usable when the client is killed instead of quit
static void sigfatal_handler(int signum) {

    fx_terminal_restore();

#if defined(HAVE_READLINE)
    if (gs_at_prompt) {
        rl_cleanup_after_signal();
    }
#endif

    signal(signum, SIG_DFL);
    raise(signum);
}

static void claim_one_signal(int signum, void (*handler)(int)) {

    struct sigaction current;
    if (sigaction(signum, NULL, &current) != 0) {
        return;
    }

    if (current.sa_handler == handler) {
        return;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    sigaction(signum, &action, NULL);
}

#endif

// Readline answers a caught signal by cleaning up, re-raising it and then
// reinstalling its handler. In a threaded client that re-raise lands on
// whichever handler is installed at that instant, which can be readline's own
// again, and the cycle repeats. Measured on CTRL-C and CTRL-Z alike, one
// keypress gave a dozen echoes and a coin flip over whether it did anything.
// So the client owns them, and takes them back from anything that grabs one,
// like the flasher progress bar, which installs a handler it never restores
static void pm3line_claim_signals(void) {
#  if !defined(_WIN32)
    claim_one_signal(SIGINT, &sigint_handler);
    claim_one_signal(SIGTSTP, &sigtstp_handler);
#  endif
}

void pm3line_install_signals(void) {
#  if defined(_WIN32)
//    SetConsoleCtrlHandler((PHANDLER_ROUTINE)terminate_handler, true);
#  else
    gs_echo_ctrl_c = (isatty(STDOUT_FILENO) == 1);

    struct sigaction action;
    memset(&action, 0, sizeof(action));

    action.sa_handler = &sigint_handler;
    sigaction(SIGINT, &action, &gs_old_sigint_action);

    action.sa_handler = &sigtstp_handler;
    sigaction(SIGTSTP, &action, &gs_old_sigtstp_action);

    action.sa_handler = &sigfatal_handler;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
#  endif

#if defined(HAVE_READLINE)
    // Readline must not catch these itself, see pm3line_claim_signals().
    // rl_catch_sigwinch is a separate flag, window resizes stay with readline
    rl_catch_signals = 0;
    rl_startup_hook = pm3line_startup_hook;
#endif // HAVE_READLINE
}

#if defined(HAVE_READLINE)
// readline calls this roughly ten times a second while it waits for input
static int pm3line_event_hook(void) {

    pm3line_claim_signals();

    if (gs_sigint_caught) {
        // Drop the line being edited and make readline() return, so the caller
        // reaches the normal shutdown instead of dying inside a handler
        rl_free_line_state();
        rl_replace_line("", 0);
        rl_done = 1;
        return 0;
    }

    if (gs_check_hook) {
        return gs_check_hook();
    }
    return 0;
}
#endif // HAVE_READLINE

void pm3line_init(void) {
#if defined(HAVE_READLINE) || defined(HAVE_LINENOISE)
    // Build the completion vocabulary from the live command tree
    pm3line_vocabulary_build();
#endif
#if defined(HAVE_READLINE)
    /* initialize history */
    using_history();
    rl_readline_name = "PM3";
    rl_attempted_completion_function = rl_command_completion;
    rl_completion_display_matches_hook = rl_display_matches;
    rl_bind_key('\t', pm3_complete);

// don't hook signal in MINGW
#if defined(__MINGW32__) || defined(__MINGW64__)
#else
    rl_getc_function = getc;
#endif

#ifdef RL_STATE_READCMD
    rl_extend_line_buffer(1024);
#endif // RL_STATE_READCMD
#elif defined(HAVE_LINENOISE)
    linenoiseInstallWindowChangeHandler();
    linenoiseSetCompletionCallback(ln_command_completion);
#endif // HAVE_READLINE

    pm3line_install_signals();
}

char *pm3line_read(const char *s) {

    pm3line_claim_signals();

    // CTRL-C already asked for a shutdown, do not put up another prompt.
    // NULL is what CTRL-D returns, the caller exits cleanly on it and that
    // path flushes the history
    if (gs_sigint_caught) {
        return NULL;
    }

    gs_at_prompt = 1;

#if defined(HAVE_READLINE)
    char *line = readline(s);
    gs_at_prompt = 0;
    if (gs_sigint_caught) {
        free(line);
        return NULL;
    }
    return line;
#elif defined(HAVE_LINENOISE)
    char *line = linenoise(s);
    gs_at_prompt = 0;
    if (gs_sigint_caught) {
        free(line);
        return NULL;
    }
    return line;
#else
    printf("%s", s);
    // MinGW/ProxSpace builds do not provide getline() in this fallback path.
    char input[1024] = {0};
    if (fgets(input, sizeof(input), stdin) == NULL) {
        gs_at_prompt = 0;
        return NULL;
    }

    gs_at_prompt = 0;
    if (gs_sigint_caught) {
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
        // keep the file in sync, so a crash or a kill -9 does not take the
        // history with it. append_history fails when the file is not there yet
        if (g_session.history_path) {
            if (append_history(1, g_session.history_path) != 0) {
                write_history(g_session.history_path);
            }
        }
    }
#elif defined(HAVE_LINENOISE)
    // linenoiseHistoryAdd takes already care of duplicate entries
    linenoiseHistoryAdd(line);
    if (g_session.history_path) {
        linenoiseHistorySave(g_session.history_path);
    }
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
    gs_check_hook = check;
    rl_event_hook = pm3line_event_hook;
#else
    check();
#endif
}

// TODO:
// src/ui.c print_progress()
