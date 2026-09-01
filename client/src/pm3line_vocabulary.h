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

#ifndef PM3LINE_VOCABULARY_H__
#define PM3LINE_VOCABULARY_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cmdparser.h"   // CMD_WALK_MAX_DEPTH

// One argument of a command, as declared in its CLIParser argument table
typedef struct vocabulary_arg_s {
    // option names, e.g. "f" and "file" ("file,filename" if several), NULL if none:
    // an argument without any name is positional
    char *shortopts;
    char *longopts;
    // type of the value, e.g. "<fn>", NULL if none
    char *datatype;
    // one line description
    char *glossary;
    // how many times the argument may be given
    int maxcount;
    // the argument takes a value
    bool has_value;
    // the value is a file or path name
    bool is_file;
} vocabulary_arg_t;

typedef struct vocabulary_s {
    // full command line, e.g. "hf mf dump" or "script run foo.lua"
    char *name;
    // IsAvailable predicates of the command and of every category on the way
    // down to it, in tree order. All of them must hold for the command to be
    // listed by "help", so the same rule is applied for completion.
    uint8_t depth;
    bool (*is_available[CMD_WALK_MAX_DEPTH])(void);
    // the command itself, NULL for entries which are not commands (script files)
    const command_t *cmd;
    // arguments of the command, learned on first use, see pm3line_vocabulary_get_args()
    bool args_known;
    vocabulary_arg_t *args;
    size_t args_count;
} vocabulary_t;

// (Re)build the vocabulary from the command tree and the script directories.
// Called automatically on first use.
void pm3line_vocabulary_build(void);

// Release the vocabulary.
void pm3line_vocabulary_free(void);

// Get the vocabulary, building it if needed.
// Returns the entries and stores their number in *count.
const vocabulary_t *pm3line_vocabulary_get(size_t *count);

// Evaluate the availability predicates of an entry, right now.
bool pm3line_vocabulary_is_available(const vocabulary_t *entry);

// Find the command a line is addressing: the longest available command whose
// name is followed by a space in line[0..len). Returns NULL if there is none.
// *args_offset receives the offset of the first character after the command
// name and the spaces following it, i.e. where its parameters start.
const vocabulary_t *pm3line_vocabulary_find_command(const char *line, size_t len, size_t *args_offset);

// Resolve line[0..len) as a (possibly abbreviated) command path that lands on a
// category rather than a leaf, e.g. "lf em 41" or "lf t55". On success writes the
// canonical, fully spelled out category name ("lf em 410x", "lf t55xx") to out
// and returns true; returns false when the path is empty, ambiguous, unknown, or
// resolves to a leaf command. Used to list a category's subcommands even when the
// path leading to it was abbreviated.
bool pm3line_vocabulary_resolve_category(const char *line, size_t len, char *out, size_t out_size);

// Arguments accepted by a command, learned on first use from the argument
// table the command hands to CLIParser. Returns false, with *count = 0, when
// they can't be learned (command not using CLIParser).
bool pm3line_vocabulary_get_args(const vocabulary_t *entry, const vocabulary_arg_t **args, size_t *count);

// Print the help of a command, as "<command> -h" does.
void pm3line_vocabulary_print_help(const vocabulary_t *entry);

#ifdef __cplusplus
}
#endif

#endif // PM3LINE_VOCABULARY_H__
