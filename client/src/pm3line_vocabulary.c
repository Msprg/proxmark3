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
// Line editor auto complete vocabulary, built at runtime from the live
// command tree so it can never drift from what "help" shows.
//-----------------------------------------------------------------------------

// this define is needed for scandir/alphasort to work
#define _GNU_SOURCE
#include "pm3line_vocabulary.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include "scandir.h"
#include "cliparser.h"    // CLIParserSetArgtableHook, struct arg_hdr
#include "commonutil.h"   // ARRAYLEN
#include "cmdmain.h"      // getTopLevelCommandTable
#include "fileutils.h"    // path_is_directory
#include "proxmark3.h"    // get_my_executable_directory, get_my_user_directory
#include "ui.h"           // PrintAndLogEx
#include "util.h"         // str_dup, str_endswith, g_printAndLog

#define VOCAB_INITIAL_CAPACITY  1024
#define VOCAB_MAX_PATH          1024

static vocabulary_t *s_vocab = NULL;
static size_t s_count = 0;
static size_t s_capacity = 0;
static bool s_built = false;

// Append an entry with the given name. Returns NULL if the name is already
// present or on allocation failure.
static vocabulary_t *vocab_add(const char *name) {

    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_vocab[i].name, name) == 0) {
            return NULL;
        }
    }

    if (s_count == s_capacity) {
        size_t new_capacity = s_capacity ? s_capacity * 2 : VOCAB_INITIAL_CAPACITY;
        vocabulary_t *tmp = realloc(s_vocab, new_capacity * sizeof(vocabulary_t));
        if (tmp == NULL) {
            PrintAndLogEx(WARNING, "Failed to allocate memory for completion vocabulary");
            return NULL;
        }
        s_vocab = tmp;
        s_capacity = new_capacity;
    }

    vocabulary_t *entry = &s_vocab[s_count];
    memset(entry, 0, sizeof(*entry));
    entry->name = str_dup(name);
    if (entry->name == NULL) {
        return NULL;
    }
    s_count++;
    return entry;
}

// Visitor for walkCommandsRecursive(): one call per leaf command.
static void vocab_visit_command(const command_t *const path[], size_t depth, void *ctx) {
    (void) ctx;

    char name[MAX_PM3_INPUT_ARGS_LENGTH] = {0};
    size_t pos = 0;
    for (size_t i = 0; i < depth; i++) {
        int n = snprintf(name + pos, sizeof(name) - pos, "%s%s", i ? " " : "", path[i]->Name);
        if (n < 0 || (size_t)n >= sizeof(name) - pos) {
            return;
        }
        pos += (size_t)n;
    }

    vocabulary_t *entry = vocab_add(name);
    if (entry == NULL) {
        return;
    }

    entry->depth = (uint8_t)depth;
    for (size_t i = 0; i < depth; i++) {
        entry->is_available[i] = path[i]->IsAvailable;
    }
    entry->cmd = path[depth - 1];
}

// Add "script run <file>" entries for every script with extension ext found
// under dir (recursively), inheriting the availability of "script run" itself.
// rel is the path of dir relative to the script directory ("" or e.g. "examples/"),
// which is what "script run" needs to find a script in a subdirectory.
static void vocab_add_scripts_dir(const char *dir, const char *rel, const char *ext, const vocabulary_t *script_run) {

    struct dirent **namelist;
    int n = scandir(dir, &namelist, NULL, alphasort);
    if (n < 0) {
        return;
    }

    for (int i = 0; i < n; i++) {

        const char *fname = namelist[i]->d_name;

        if (strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0) {
            free(namelist[i]);
            continue;
        }

        char fullpath[VOCAB_MAX_PATH] = {0};
        int len = snprintf(fullpath, sizeof(fullpath), "%s%s", dir, fname);
        if (len < 0 || (size_t)len >= sizeof(fullpath) - 1) {
            free(namelist[i]);
            continue;
        }

        if (path_is_directory(fullpath)) {
            fullpath[len] = PATHSEP[0];
            fullpath[len + 1] = '\0';

            char subrel[VOCAB_MAX_PATH] = {0};
            len = snprintf(subrel, sizeof(subrel), "%s%s%s", rel, fname, PATHSEP);
            if (len > 0 && (size_t)len < sizeof(subrel)) {
                vocab_add_scripts_dir(fullpath, subrel, ext, script_run);
            }
            free(namelist[i]);
            continue;
        }

        if (str_endswith(fname, ext)) {
            char name[MAX_PM3_INPUT_ARGS_LENGTH] = {0};
            len = snprintf(name, sizeof(name), "%s %s%s", script_run->name, rel, fname);
            if (len > 0 && (size_t)len < sizeof(name)) {
                vocabulary_t *entry = vocab_add(name);
                if (entry != NULL) {
                    entry->depth = script_run->depth;
                    memcpy(entry->is_available, script_run->is_available, sizeof(entry->is_available));
                }
            }
        }
        free(namelist[i]);
    }
    free(namelist);
}

// Same search order as searchAndList() / "script list":
// dev tree next to the executable, user directory, installed share directory.
static void vocab_add_scripts(const char *pm3dir, const char *ext, const vocabulary_t *script_run) {

    const char *exec_path = get_my_executable_directory();
    const char *user_path = get_my_user_directory();

    if (exec_path != NULL) {
        char path[VOCAB_MAX_PATH] = {0};
        int n = snprintf(path, sizeof(path), "%s%s", exec_path, pm3dir);
        if (n > 0 && (size_t)n < sizeof(path)) {
            vocab_add_scripts_dir(path, "", ext, script_run);
        }
    }

    if (user_path != NULL) {
        char path[VOCAB_MAX_PATH] = {0};
        int n = snprintf(path, sizeof(path), "%s%s%s", user_path, PM3_USER_DIRECTORY, pm3dir);
        if (n > 0 && (size_t)n < sizeof(path)) {
            vocab_add_scripts_dir(path, "", ext, script_run);
        }
    }

    if (exec_path != NULL) {
        char path[VOCAB_MAX_PATH] = {0};
        int n = snprintf(path, sizeof(path), "%s%s%s", exec_path, PM3_SHARE_RELPATH, pm3dir);
        if (n > 0 && (size_t)n < sizeof(path)) {
            vocab_add_scripts_dir(path, "", ext, script_run);
        }
    }
}

void pm3line_vocabulary_build(void) {

    pm3line_vocabulary_free();

    // The walk goes through the Parse() of every category. Keep it silent:
    // a few commands shown like categories (e.g. "reveng") run their own
    // parser on the walk token and would complain about it.
    uint8_t old_printAndLog = g_printAndLog;
    g_printAndLog = 0;
    walkCommandsRecursive(getTopLevelCommandTable(), vocab_visit_command, NULL);
    g_printAndLog = old_printAndLog;

    // Script files complete as "script run <file>", with the availability of "script run".
    // Take a copy: vocab_add() may realloc the array while scripts are being added.
    vocabulary_t script_run = {0};
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_vocab[i].name, "script run") == 0) {
            script_run = s_vocab[i];
            break;
        }
    }

    if (script_run.name != NULL) {
        vocab_add_scripts(LUA_SCRIPTS_SUBDIR, ".lua", &script_run);
        vocab_add_scripts(CMD_SCRIPTS_SUBDIR, ".cmd", &script_run);
#ifdef HAVE_PYTHON
        vocab_add_scripts(PYTHON_SCRIPTS_SUBDIR, ".py", &script_run);
#endif
    }

    s_built = true;
}

static void vocab_free_args(vocabulary_t *entry) {
    for (size_t i = 0; i < entry->args_count; i++) {
        free(entry->args[i].shortopts);
        free(entry->args[i].longopts);
        free(entry->args[i].datatype);
        free(entry->args[i].glossary);
    }
    free(entry->args);
    entry->args = NULL;
    entry->args_count = 0;
    entry->args_known = false;
}

void pm3line_vocabulary_free(void) {
    for (size_t i = 0; i < s_count; i++) {
        free(s_vocab[i].name);
        vocab_free_args(&s_vocab[i]);
    }
    free(s_vocab);
    s_vocab = NULL;
    s_count = 0;
    s_capacity = 0;
    s_built = false;
}

const vocabulary_t *pm3line_vocabulary_get(size_t *count) {
    if (s_built == false) {
        pm3line_vocabulary_build();
    }
    if (count != NULL) {
        *count = s_count;
    }
    return s_vocab;
}

bool pm3line_vocabulary_is_available(const vocabulary_t *entry) {
    if (entry == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < entry->depth; i++) {
        if (entry->is_available[i] != NULL && entry->is_available[i]() == false) {
            return false;
        }
    }
    return true;
}

const vocabulary_t *pm3line_vocabulary_find_command(const char *line, size_t len, size_t *args_offset) {

    size_t count = 0;
    const vocabulary_t *vocabulary = pm3line_vocabulary_get(&count);

    // Resolve the command in line[0..len) token by token, the way CmdsParse()
    // does, so an abbreviated command ("hf mf dum -f ...") is found as well as a
    // fully spelled out one. At each level the line token is matched against the
    // available command names: an exact token match wins (so "mf" selects the
    // "mf" category and not "mfu"/"mfp"/...), otherwise it must be the prefix of
    // exactly one token, or the command is ambiguous and resolves to nothing.
    // `resolved` accumulates the chosen full tokens, which is what filters the
    // candidates at the next level down.
    char resolved[MAX_PM3_INPUT_ARGS_LENGTH] = {0};
    size_t resolved_len = 0;
    size_t li = 0;

    while (true) {

        while (li < len && line[li] == ' ') {
            li++;
        }
        if (li >= len) {
            return NULL;   // command not (yet) followed by a space and arguments
        }

        size_t lstart = li;
        while (li < len && line[li] != ' ') {
            li++;
        }
        const char *ltok = line + lstart;
        size_t ltok_len = li - lstart;

        // Look at the token right after `resolved` in every candidate command:
        // remember an exact match and, separately, whether the line token is the
        // prefix of exactly one distinct token value.
        const char *exact = NULL, *prefix = NULL;
        size_t exact_len = 0, prefix_len = 0;
        bool prefix_ambiguous = false;

        for (size_t i = 0; i < count; i++) {

            const vocabulary_t *e = &vocabulary[i];
            if (e->cmd == NULL || pm3line_vocabulary_is_available(e) == false) {
                continue;
            }
            if (resolved_len > 0) {
                if (strncmp(e->name, resolved, resolved_len) != 0 || e->name[resolved_len] != ' ') {
                    continue;
                }
            }

            const char *tok = e->name + (resolved_len ? resolved_len + 1 : 0);
            const char *sp = strchr(tok, ' ');
            size_t tok_len = sp ? (size_t)(sp - tok) : strlen(tok);

            if (ltok_len > tok_len || strncmp(tok, ltok, ltok_len) != 0) {
                continue;
            }

            if (tok_len == ltok_len) {
                exact = tok;
                exact_len = tok_len;
            } else if (prefix == NULL) {
                prefix = tok;
                prefix_len = tok_len;
            } else if (prefix_len != tok_len || strncmp(prefix, tok, tok_len) != 0) {
                // a second, different token value also has the line token as a prefix
                prefix_ambiguous = true;
            }
        }

        const char *chosen = exact ? exact : prefix;
        size_t chosen_len = exact ? exact_len : prefix_len;
        if (chosen == NULL || (exact == NULL && prefix_ambiguous)) {
            return NULL;   // nothing matches, or the abbreviation is ambiguous
        }

        // append " " + chosen token to the resolved command
        if (resolved_len + (resolved_len ? 1 : 0) + chosen_len >= sizeof(resolved)) {
            return NULL;
        }
        if (resolved_len > 0) {
            resolved[resolved_len++] = ' ';
        }
        memcpy(resolved + resolved_len, chosen, chosen_len);
        resolved_len += chosen_len;
        resolved[resolved_len] = '\0';

        // Did the chosen token complete a leaf command (rather than descend into
        // a category)? If so, the rest of the line is its arguments.
        for (size_t i = 0; i < count; i++) {
            const vocabulary_t *e = &vocabulary[i];
            if (e->cmd != NULL && pm3line_vocabulary_is_available(e) && strcmp(e->name, resolved) == 0) {
                // the command must be followed by a space, so what comes next are
                // its arguments and not more of the command still being typed
                if (li >= len || line[li] != ' ') {
                    return NULL;
                }
                if (args_offset != NULL) {
                    size_t offset = li;
                    while (offset < len && line[offset] == ' ') {
                        offset++;
                    }
                    *args_offset = offset;
                }
                return e;
            }
        }
    }
}

bool pm3line_vocabulary_resolve_category(const char *line, size_t len, char *out, size_t out_size) {

    size_t count = 0;
    const vocabulary_t *vocabulary = pm3line_vocabulary_get(&count);

    // Resolve token by token exactly like pm3line_vocabulary_find_command(), but
    // consume the whole range instead of stopping at the first leaf: an exact
    // token match wins, otherwise a unique prefix, otherwise it's ambiguous.
    char resolved[MAX_PM3_INPUT_ARGS_LENGTH] = {0};
    size_t resolved_len = 0;
    size_t li = 0;
    bool any = false;

    while (true) {

        while (li < len && line[li] == ' ') {
            li++;
        }
        if (li >= len) {
            break;   // all tokens consumed
        }

        size_t lstart = li;
        while (li < len && line[li] != ' ') {
            li++;
        }
        const char *ltok = line + lstart;
        size_t ltok_len = li - lstart;

        const char *exact = NULL, *prefix = NULL;
        size_t exact_len = 0, prefix_len = 0;
        bool prefix_ambiguous = false;

        for (size_t i = 0; i < count; i++) {

            const vocabulary_t *e = &vocabulary[i];
            if (e->cmd == NULL || pm3line_vocabulary_is_available(e) == false) {
                continue;
            }
            if (resolved_len > 0) {
                if (strncmp(e->name, resolved, resolved_len) != 0 || e->name[resolved_len] != ' ') {
                    continue;
                }
            }

            const char *tok = e->name + (resolved_len ? resolved_len + 1 : 0);
            const char *sp = strchr(tok, ' ');
            size_t tok_len = sp ? (size_t)(sp - tok) : strlen(tok);

            if (ltok_len > tok_len || strncmp(tok, ltok, ltok_len) != 0) {
                continue;
            }

            if (tok_len == ltok_len) {
                exact = tok;
                exact_len = tok_len;
            } else if (prefix == NULL) {
                prefix = tok;
                prefix_len = tok_len;
            } else if (prefix_len != tok_len || strncmp(prefix, tok, tok_len) != 0) {
                prefix_ambiguous = true;
            }
        }

        const char *chosen = exact ? exact : prefix;
        size_t chosen_len = exact ? exact_len : prefix_len;
        if (chosen == NULL || (exact == NULL && prefix_ambiguous)) {
            return false;   // nothing matches, or the abbreviation is ambiguous
        }

        if (resolved_len + (resolved_len ? 1 : 0) + chosen_len >= sizeof(resolved)) {
            return false;
        }
        if (resolved_len > 0) {
            resolved[resolved_len++] = ' ';
        }
        memcpy(resolved + resolved_len, chosen, chosen_len);
        resolved_len += chosen_len;
        resolved[resolved_len] = '\0';
        any = true;
    }

    if (any == false) {
        return false;   // nothing to resolve
    }

    // The resolved name must be a category: not a leaf command itself, but some
    // command continues past it ("resolved <child> ...").
    bool is_leaf = false, has_child = false;
    for (size_t i = 0; i < count; i++) {
        const vocabulary_t *e = &vocabulary[i];
        if (e->cmd == NULL || pm3line_vocabulary_is_available(e) == false) {
            continue;
        }
        if (strcmp(e->name, resolved) == 0) {
            is_leaf = true;
        } else if (strncmp(e->name, resolved, resolved_len) == 0 && e->name[resolved_len] == ' ') {
            has_child = true;
        }
    }
    if (is_leaf || has_child == false) {
        return false;
    }

    if (resolved_len >= out_size) {
        return false;
    }
    memcpy(out, resolved, resolved_len + 1);
    return true;
}

// The value scan function of arg_file entries, to recognise them without
// relying on argtable internals.
static arg_scanfn *vocab_file_scanfn(void) {
    static arg_scanfn *scanfn = NULL;
    if (scanfn == NULL) {
        struct arg_file *probe = arg_file0(NULL, NULL, NULL, NULL);
        if (probe != NULL) {
            scanfn = probe->hdr.scanfn;
            free(probe);
        }
    }
    return scanfn;
}

// A value is a file or path name when its type says so, e.g. "<fn>",
// "<fn w/o ext>", "<filename>", "<path>" or "<hex|pem|der|path>",
// but not "<profile>".
static bool vocab_datatype_is_file(const char *datatype) {

    static const char *const words[] = { "fn", "file", "filename", "path" };

    if (datatype == NULL) {
        return false;
    }

    const char *p = datatype;
    while (*p) {

        while (*p && isalnum((unsigned char)*p) == 0) {
            p++;
        }

        const char *word = p;
        while (*p && isalnum((unsigned char)*p)) {
            p++;
        }
        size_t word_len = p - word;

        for (size_t i = 0; i < ARRAYLEN(words); i++) {
            if (word_len != strlen(words[i])) {
                continue;
            }
            size_t j = 0;
            while (j < word_len && tolower((unsigned char)word[j]) == words[i][j]) {
                j++;
            }
            if (j == word_len) {
                return true;
            }
        }
    }
    return false;
}

// CLIParser hook, see pm3line_vocabulary_get_args(): take a copy of the
// argument table of the command being probed.
static void vocab_argtable_hook(const char *program_name, void *argtable[], size_t argtable_len, void *hookctx) {

    (void) program_name;
    vocabulary_t *entry = hookctx;

    // A command may run the parser more than once, keep the first table
    if (entry->args != NULL) {
        return;
    }

    entry->args = calloc(argtable_len, sizeof(vocabulary_arg_t));
    if (entry->args == NULL) {
        return;
    }

    for (size_t i = 0; i < argtable_len; i++) {

        const struct arg_hdr *hdr = argtable[i];

        // arg_end
        if (hdr->flag & ARG_TERMINATOR) {
            break;
        }

        bool has_value = (hdr->flag & ARG_HASVALUE) != 0;

        // arg_rem entries are documentation only
        if (hdr->shortopts == NULL && hdr->longopts == NULL && has_value == false) {
            continue;
        }

        vocabulary_arg_t *arg = &entry->args[entry->args_count++];
        arg->shortopts = hdr->shortopts ? str_dup(hdr->shortopts) : NULL;
        arg->longopts = hdr->longopts ? str_dup(hdr->longopts) : NULL;
        arg->datatype = hdr->datatype ? str_dup(hdr->datatype) : NULL;
        arg->glossary = hdr->glossary ? str_dup(hdr->glossary) : NULL;
        arg->maxcount = hdr->maxcount;
        arg->has_value = has_value;
        arg->is_file = has_value && ((hdr->scanfn == vocab_file_scanfn()) || vocab_datatype_is_file(hdr->datatype));
    }
}

bool pm3line_vocabulary_get_args(const vocabulary_t *centry, const vocabulary_arg_t **args, size_t *count) {

    // The entries are ours, only their lazily learned part is touched here
    vocabulary_t *entry = (vocabulary_t *)centry;

    if (entry->args_known == false) {
        entry->args_known = true;

        if (entry->cmd != NULL && entry->cmd->Parse != NULL) {
            // Run the command with "-h": a CLIParser command hands its argument
            // table to the parser before doing anything else and, with the hook
            // installed, the parser makes it return right away. Keep it silent:
            // the few commands not using CLIParser print their usage instead.
            uint8_t old_printAndLog = g_printAndLog;
            g_printAndLog = 0;
            CLIParserSetArgtableHook(vocab_argtable_hook, entry);
            entry->cmd->Parse("-h");
            CLIParserSetArgtableHook(NULL, NULL);
            g_printAndLog = old_printAndLog;
        }
    }

    *args = entry->args;
    *count = entry->args_count;
    return (entry->args != NULL);
}

void pm3line_vocabulary_print_help(const vocabulary_t *entry) {
    if (entry == NULL || entry->cmd == NULL || entry->cmd->Parse == NULL) {
        return;
    }
    // Print only, help requests don't belong in the session log
    uint8_t old_printAndLog = g_printAndLog;
    g_printAndLog &= PRINTANDLOG_PRINT;
    entry->cmd->Parse("-h");
    g_printAndLog = old_printAndLog;
}
