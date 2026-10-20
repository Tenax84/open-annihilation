# Legacy rule files

Reads one legacy `.sm` rule file into rule records. Target
`oa-formats-soft-mutator`, header `oa/formats/soft_mutator.hpp`, namespace
`oa::formats::soft_mutator`. A rule file names records and writes their
fields. The records are what a mutator applies; this module only reads them.
The same records are what a Lua rule builds, so the two spellings can be
compared with `equivalent`.

A text longer than `max_input_bytes` (1 MiB) is refused before it is read.
A bad statement is dropped and the rest of the file is still read. Nothing
here opens a file.

## Entry points

- `parse_rules` reads the bytes of one rule file and returns its blocks and
  its diagnostics. `ParseResult::has_errors` says whether any diagnostic is
  an error.
- `code_name` is a diagnostic code's name, as the enumerator is written.
- `equivalent` reports whether two blocks, or two files, are the same rules.
  Line numbers are ignored. Field names and need texts compare without ASCII
  case, change texts compare exactly, needs compare as a set, and changes
  compare in order after a stable sort by field name.

## Grammar

Keywords, section names and `<AllDepth2>` match without ASCII case. A `//`
outside quotes runs to the end of the line. A line ends at LF, CRLF or a
lone CR. Whitespace is free around `=`, around braces and at the start of a
line. A number is `-` optionally, then digits with an optional fraction, or
a point and digits. It is then read by the same reader the game uses for
the value. An exponent, or a leading `+`, is not a number.

```ebnf
file        = { directive | block | stray_brace } ;
directive   = ( "folder" | "extension" ) "=" text [ ";" ] ;
block       = "[" header "]" "{" body "}" ;
header      = "*all*" | "*group*" | name ;
body        = { change }                         (* *all* and a named record *)
          | { needs | changes } ;                (* a group, either order *)
needs       = "[Needs]" "{" { need } "}" ;
changes     = "[Changes]" "{" { change } "}" ;
need        = field "=" [ "!" ] ( number "," number
                                | string [ "*" ]
                                | '""' ) [ ";" ] ;   (* '""' is lenient *)
change      = ( field | "<AllDepth2>" ) "=" value [ ";" ] ;
value       = number
          | "*" number [ "(" number "," number ")" ]
          | "^" number
          | string
          | string "+"
          | bare ;                                   (* lenient *)
bare        = (* empty, or words with their inner spaces *) ;
string      = '"' { byte - '"' } '"' ;
stray_brace = "}" ;                                  (* lenient, outside a block *)
```

Lenient forms, each kept and warned about:

- A statement or directive may end at the end of the line, or at the `}`
  that closes it, with no `;` (`missing_semicolon`). Several statements may
  share a line when a `;` separates them, and the next header may follow a
  `}` on that same line.
- The `{` of a block may be on a later line than its header.
- A header whose body holds `[Needs]` or `[Changes]` is a group even when
  the header is not `*Group*` (`group_without_asterisks`).
- A number written in quotes is a text change (`quoted_number`).
- Spaces inside an unquoted value, such as `*7 (2,15)`, are ignored
  (`spaces_in_value`).
- A need whose low bound is above its high bound is kept (`empty_range`).
  It matches nothing, or everything when it is negated.
- A group with no `[Needs]` is kept (`group_without_needs`). It changes
  every record of its target.
- A second `[Needs]` or `[Changes]` in one group is merged
  (`repeated_section`).
- A leading byte-order mark is skipped (`byte_order_mark`).
- A change whose value is a bare word or words is a text assignment
  (`unquoted_text`). The words are trimmed at both ends and their inner
  spaces stay as written. These forms come from rule files written in
  2005–2008, which leave the quotes off. A value that starts like a
  number, a scale, a clamp, a meta value or a quote keeps the stricter
  reading, including a malformed one such as `*7 (2,15)` or `*1e2`.
- A change with nothing after `=` assigns empty text (`empty_value`).
  That is not `""`, which removes the field.
- A need written `""` or `!""` is equality with empty text
  (`empty_text_need`), negated when it is written `!""`. `""*` and
  `!""*` stay presence tests.
- A `}` outside any block is ignored (`stray_closing_brace`), including
  one after a middle block and one after the last block. A `}` that ends
  a block still ends it, and a `{` at the top level or inside a body
  stays an error.

A group's body is only `[Needs]` and `[Changes]`. A statement anywhere else
in that body is dropped (`stray_text`) at its own line, before the first
section as well as after it. When a header becomes a group because its body
holds a section, statements already read from that body are dropped the same
way. A `[` inside `[Needs]` or `[Changes]` is `unknown_section`: a section
cannot hold a section. That section's braces are skipped, and reading
carries on. A section is never read inside a section.

`folder` and `extension` apply to every block after them, and each may be
set more than once. `\` in either becomes `/`, and both are stored in lower
case. The extension is a filename suffix with no leading dot, so
`sidedata.tdf` names one file. Until a file sets them, the target is
`units` and `fbi`.

### A header written `[Group]`

Three WG-Fortress rule files write `[Group]`, with `[needs]` and `[changes]`
inside, to set the solars' negative `EnergyUse` to zero. A section named
after a record cannot hold sections of its own, so a body that holds
`[Needs]` or `[Changes]` is read as a group, whatever the header says, and
`group_without_asterisks` is reported. That is the reading under which those
files do what their authors wrote.

### A number written in quotes

A change whose text is a number, such as `MaxDamage="850"`, is a text
change (`assign_text`), as a quoted value is. `quoted_number` is reported.
The bytes inside the quotes are kept, including a trailing space in
`"LIGHTNING "`. An empty quoted value with no `+` removes the field.

## Diagnostics

Warnings keep the statement or the block. Errors drop the statement, or the
block when its braces or header are broken, and reading continues.

| Code | Meaning |
| --- | --- |
| `missing_semicolon` | a kept statement was not closed with `;` |
| `group_without_asterisks` | a header without `*Group*` holds a group body |
| `quoted_number` | a change's quoted text is a number |
| `spaces_in_value` | spaces inside an unquoted value were ignored |
| `empty_range` | a need's low bound is above its high bound |
| `group_without_needs` | a group has no needs, so it matches every record |
| `repeated_section` | a second needs or changes section was merged |
| `byte_order_mark` | a leading byte-order mark was skipped |
| `unquoted_text` | bare words read as a text assignment; the quotes were left off |
| `empty_value` | a change with nothing after `=`, read as empty text rather than a removal |
| `empty_text_need` | `""` or `!""` in a need, equality with empty text rather than a presence test |
| `stray_closing_brace` | a `}` outside any block, ignored so the blocks around it are still read |
| `unknown_section` | a section other than needs or changes, or a section inside a section |
| `unclosed_block` | a header or a brace was never closed |
| `unexpected_brace` | a brace where a block did not call for one |
| `directive_in_block` | `folder` or `extension` inside a block |
| `bad_number` | a number the grammar does not allow |
| `bad_need` | a need that matches no need form |
| `bad_change` | a change that matches no change form |
| `need_form_in_changes` | a need written where a change belongs |
| `change_form_in_needs` | a change written where a need belongs |
| `clamp_low_above_high` | a scale's low clamp is above its high clamp |
| `unterminated_string` | a quote was never closed |
| `block_comment` | `/*`, which is not part of the language |
| `stray_text` | text that is not a directive, a block or a statement, or a statement outside a group's sections |
| `too_large` | the text is longer than 1 MiB |
| `too_many_blocks` | more than 4096 blocks |
| `too_many_statements` | more than 4096 statements in one block |
| `text_too_long` | a name, field or quoted text is longer than 1024 bytes |
| `too_many_diagnostics` | the list stopped at 1000 diagnostics |

## State

None. `parse_rules` reads its arguments and returns a value.

## Invariants

- A text longer than 1 MiB is refused before any of it is read, and the
  reader opens no file and throws nothing of its own.
- At most 4096 blocks are kept, at most 4096 statements in one block, and
  at most 1024 bytes of one name, field, quoted text or bare text. The 1000th
  diagnostic is the last one stored; a further finding replaces it with
  `too_many_diagnostics` and collection stops.
- A bad statement is dropped. A block whose header or braces are broken is
  dropped. Reading continues to the end of the text.
- A number is accepted only as the grammar spells it, then read by
  `oa::formats::tdf::parse_double`. An exponent or a leading `+` is an error.
- `equivalent` ignores line numbers. It folds field names, the block name
  and need texts to lower case, compares needs as a set, and compares
  changes in order after a stable sort by field name. Change texts compare
  exactly.

## Tests

`formats-soft-mutator` checks each need and change form, both selectors and
a repeated named block, a group header with no asterisks, directives, the
lenient layouts, the later lenient forms, every diagnostic, the malformed
texts, and `equivalent`.

The real rule files are not part of the engine. From a build:

```sh
build/src/formats/soft-mutator/oa-formats-soft-mutator-test --corpus DIR
```

That reads every `*.sm` in `DIR`, prints one line per file and then
`N files, N blocks, N warnings, N errors`, and exits non-zero when `DIR`
holds no rule file or any file has an error. It is not a ctest.

Run it on the early set of rule files and on the 2005–2008 set. Both
should report 0 errors. The later set is where the four lenient forms
above are used: bare words (`unquoted_text`), a change with no value
(`empty_value`), `""` and `!""` written as needs (`empty_text_need`),
and a `}` after a block (`stray_closing_brace`).

## Limitations

Needs are all required. The language has no OR between them, and no
arithmetic between fields. `/* */` is not a comment. A number has no
exponent, and a number longer than the game's reader keeps whole is refused.
Applying the records, and the same records written in Lua, are other work.
