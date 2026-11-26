# jsonb and json in the SQL layer

Status: **design, approved by the owner on 2026-11-21 with these
answers: Q1 binary, Q2–Q3, Q5, Q7–Q11 as recommended; Q4 and Q6 deferred
to the next phase.** Stages 0, 1, 3 built: runtime errors carry their
SQLSTATE, DETAIL, HINT and CONTEXT (`BC_E_DATA`, plan API revision 20);
the jsonb column type with its text in and out, casts with text, `DEFAULT`,
`IS [NOT] NULL`, `COUNT`, `CASE` and `COALESCE` (`sql/jsonb.cppm`, plan API
revision 21); and the accessors `->`, `->>`, `#>`, `#>>`, `jsonb_typeof`,
`jsonb_array_length` and the casts to `boolean` and the numbers (plan API
revision 22). Stage 4 is built but for `json_build_array` and
`json_build_object`, which wait for stage 1c: containment, existence,
modification and construction (plan API revision 25). Stage 2 is built:
comparison, order, groups, `DISTINCT` and set operations by the jsonb sort
key (plan API revision 26). *Stages of jsonb* says what each built and how
it differs from this design. Stages 1b, 1c, 5 and 6 are not built.

This document covers PostgreSQL's `jsonb` type, and `json` where it is
cheap: what the layer matches exactly, how a value is stored, the plan API
calls and binder rules that serve the operators, aggregates, set-returning
functions, indexes, the catalog change, the guarantees it adds, its tests
and the stages that build it. Behaviour observed on PostgreSQL 17.12 is
cited as "PG 17"; every PG 17 claim was run on a local server with `\set
VERBOSITY verbose` (database `postgres`, collation C.UTF-8, encoding UTF8,
`max_stack_depth` 2MB), binary-format parameters through libpq. Code
citations are to `bytecaskdb-sql-layer/` on `origin/main` at bc0492a7.

**The design in one paragraph.** `jsonb` is a new SQL kind (`SqlKind` 19)
stored as a **binary tree**: a format byte, then tagged nodes, with offset
tables for arrays and objects, keys sorted for binary search, strings
de-escaped and numbers as sign, weight, digits and display scale. Input is
parsed once, on write, by an iterative parser with PG 17's grammar, errors
and normalisation (last duplicate key wins, keys in PG 17's storage
order); output writes PG 17's `jsonb_out` text byte for byte. Every read
is bounds-checked and overflow-safe, and a damaged value is
`BC_E_CORRUPT`. The binary form lives only inside the layer; at every edge
(input, output, the wire, Python, the catalog's defaults) a value is its
canonical text. Inside a plan a jsonb value has its own handle type,
`bc_jsonb`, so a C compiler refuses a plan that hands one to a bytes call.
Equality, order and hashing go through one order-preserving byte encoding,
the jsonb sort key, which makes `=`, `ORDER BY`, `GROUP BY` and `DISTINCT`
reuse the layer's sort keys. `json` (`SqlKind` 20) is the validated input
text, kept verbatim, and is a result type only: a json column is refused.
Every allocation is charged to the statement's budget; every runtime error
carries its own SQLSTATE. The stages ship the error mechanism first, then
the type, then the operators Mattermost and the ORMs use, then comparison
and the aggregates; set-returning functions get their own later design,
GIN indexes and jsonb keys wait for the next phase, and everything not
built is refused with 0A000.

## Goal and scope of jsonb

### Why jsonb now

- **Mattermost (#889).** Its PostgreSQL schema has about 20 `jsonb`
  columns (`users.props`, `posts.props`, `channelmembers.notifyprops`, …)
  and no `json` column. A fresh install runs migrations that convert text
  columns to `jsonb`, and the store's runtime queries use `->>`, `->`,
  `@>`, `||`, `?`, `jsonb_set`, `jsonb_typeof`, `jsonb_build_array`,
  `json_agg … FILTER`, `jsonb_object_agg` and `jsonb_array_elements[_text]`
  (counts and call sites in *Mattermost's use of jsonb*, below). No
  Mattermost table can be created today: `CREATE TABLE t (j jsonb)` is
  42601 "type jsonb is not supported" (`sql/binder.cppm:309`).
- **ORMs.** Django's `JSONField` is `jsonb` and its lookups emit `->`
  (with a text key or an integer index), `->>`, `#>`, `#>>`, `@>`, `<@`,
  `?`, `?|` and `?&`. Rails registers the type and leaves the operators to
  hand-written SQL. Prisma's Json filters (not verified here) emit `#>`,
  `#>>`, `@>`, `jsonb_typeof` and `jsonb_build_object`/`jsonb_agg`.
- **What a missing type costs.** An application with one `jsonb` column
  cannot create its schema at all. The type alone, with its text in and
  out, unblocks every table; the operators are what its queries then
  need.

### Mattermost's use of jsonb

Read from Mattermost at commit 2d6c2852 (`server/channels/store/sqlstore/`,
S; `server/channels/db/migrations/postgres/`, M):

- Writes bind `?::jsonb` from text (8 sites, e.g. `property_value_store.go:249`)
  or send the parameter in binary format, `0x01` and the text, with
  `binary_parameters=yes` (`utils.go:181-184`, `store.go:408`; about 21
  sites).
- `->>`: `COALESCE(Props->>'last_removed_device_id','')`
  (`session_store.go:191`), `Timezone->>'useAutomaticTimezone' = 'true'`
  (`channel_store.go:2842`), `LOWER(Attrs->>?) LIKE LOWER(?)`
  (`property_field_store_listing.go:127`), and in `GROUP BY` and `ORDER
  BY`.
- `->` compared as jsonb: `Data->? = ?` (`job_store.go:474`, parameters
  `{text,jsonb}`), `Attrs->? <> '[]'::jsonb`, `pv.Value IN
  ('null'::jsonb,'""'::jsonb,'[]'::jsonb)`.
- `@>`: array-of-object containment and the top-level-scalar rule
  (`access_control_policy_store.go:62, 73, 723`).
- `||`: object merge (`channel_store.go:2152`) and array append guarded
  by `NOT participants ? $3` (`thread_store.go:1161-1163`).
- `jsonb_set(Props, $2, $3)` with `$2` a `text[]` (`post_store.go:1026`).
- `json_agg(…) FILTER (WHERE …)` under `COALESCE(…, '[]')`
  (`post_store.go:2931`), `jsonb_object_agg`.
- `jsonb_array_elements[_text]` in `FROM`, `JOIN … ON`, `CROSS JOIN
  LATERAL` and the select list (`access_control_policy_store.go:723-878`).
- Migrations add `jsonb_to_recordset`, `jsonb_agg … ORDER BY`, `WITH
  ORDINALITY`, `#>>'{}'`, `- 'key'`, `ALTER TABLE … ALTER COLUMN … TYPE
  jsonb USING …` (the text-to-jsonb conversion), and two `CREATE INDEX
  CONCURRENTLY … USING GIN (… jsonb_path_ops)` (M 000228, 000229).

So jsonb alone does not run Mattermost: it also needs `LATERAL`,
`CONCURRENTLY`, `ALTER COLUMN … TYPE` (the parser takes only `ADD COLUMN`,
`sql/parser.cppm:1163-1170`) and more. This design builds the jsonb part
and names the rest as dependencies.

### What each stage builds and refuses

| Stage | Builds | Still refused (0A000 unless stated) |
|---|---|---|
| 0 | `BC_E_DATA` and a SQLSTATE carried by the error, with DETAIL, HINT and CONTEXT; the three `BC_E_ARITH` message matches move to it | nothing jsonb |
| 1 | the `jsonb` column type; input, output, casts with text; literals, parameters, `DEFAULT`, `ADD COLUMN … DEFAULT`; `IS [NOT] NULL`, `COUNT`, `CASE`/`COALESCE` of jsonb; wire OID 3802 | `json`; every comparison, `ORDER BY`, `GROUP BY`, `DISTINCT`, set operations but `UNION ALL`, keys, every operator and function, as NUMERIC is refused today; a domain over jsonb, a procedure argument or PL/pgSQL variable of jsonb |
| 1b | binary-format parameters of `jsonb`, `json`, `text`, `varchar` and `bytea` | binary results; binary parameters of every other type |
| 1c | the `json` type as a value and result: its own type class and PG 17's refusals, casts with jsonb and text, OID 114 | a `json` column (owner's decision 11); every operator on json |
| 3 | `->`, `->>` (text key and integer index), `#>`, `#>>` with a path `text[]` (owner's decision 2), `jsonb_typeof`, `jsonb_array_length`, casts from jsonb to `boolean`, the integers, `real`, `double precision`, `numeric` | containment, existence, modification |
| 2 | `= <> < <= > >=`, `IN`, `BETWEEN`, `NULLIF`, `ORDER BY`, `GROUP BY`, `DISTINCT`, `UNION`/`INTERSECT`/`EXCEPT` on jsonb | jsonb as a key, unique or index column (next phase); `MIN`/`MAX` (PG 17: 42883, kept); `GREATEST`/`LEAST` (the layer has neither for any type) |
| 4 | `@>`, `<@`, `?`, `?|`, `?&`, `||`, `-` (text, int, text[]), `#-`, `jsonb_set`, `jsonb_set_lax`, `jsonb_insert`, `jsonb_build_array`, `jsonb_build_object`, `to_jsonb`, `jsonb_strip_nulls`, `jsonb_pretty`, `jsonb_extract_path[_text]`, `json_build_array`, `json_build_object` | jsonpath (`@?`, `@@`, `jsonb_path_*`), SQL/JSON (`JSON_VALUE`, `IS JSON`, `JSON_TABLE`, …), subscripting, every operator on `json` |
| 5 | `jsonb_agg`, `json_agg`, `jsonb_object_agg`, `json_object_agg` (and `_strict`, `_unique`), with `FILTER (WHERE …)` and `ORDER BY` inside the call, both general | `DISTINCT` inside them |
| 6 | set-returning functions in `FROM` and `LATERAL` (own design addendum first) | until then: any function in `FROM`, `LATERAL`, `WITH ORDINALITY`, a set-returning function in the select list |

Stage 3 comes before stage 2 because `->>` returns text, which already
compares, and Mattermost's most frequent uses are `->>` in `WHERE`, `GROUP
BY` and `ORDER BY`; the two stages are independent, and either can go
first. `Data->? = ?` needs both. Using `->>` in a `WHERE` also needs
expression operands there, which are not jsonb's own work (*Expressions in
WHERE and ON*).

Refused in every stage, each with PG 17's SQLSTATE where PG 17 also
refuses (42883, 42846, 42804, 42725, 42704) and 0A000 where the layer
stops short of PG 17: `jsonb[]` and `json[]` columns and arrays generally,
jsonpath, SQL/JSON standard syntax (features T811–T838, `mandatory:
false` in `tests/functional/features.yaml:1921-2006`), subscripting
(`j['a']` and `UPDATE … SET j['a'] = …`), `jsonb_populate_record[set]`,
`jsonb_to_record[set]` before stage 6, `row_to_json` and `to_jsonb` of a
row or array (the layer has neither as values), `jsonb_object(text[])`,
GIN indexes and jsonb as a key, unique or index column (both the next
phase), a `json` column, and every operator of `json` other than its
casts.

## jsonb and json semantics the layer matches

The rules below are what the functional cases check, each recorded from
PG 17.

### jsonb input grammar and its errors

The grammar is RFC 8259, strictly, as PG 17's `jsonb_in`.

- **Values.** Any JSON value at top level, scalars included. Literals are
  lowercase only: `True`, `nul`, `NaN` and `Infinity` are invalid.
- **Whitespace.** Space, `\t`, `\n` and `\r` only. `\f` and U+00A0 are
  invalid tokens.
- **Numbers.** `-? (0 | [1-9][0-9]*) (\.[0-9]+)? ([eE][+-]?[0-9]+)?`. `01`,
  `-01`, `.5`, `5.`, `+1`, `1e`, `0x10` and `1_000` are invalid. The
  layer's `numeric_in` (`sql/text.cppm:1127`) accepts `0x10`, `_` and
  `NaN`, so the lexer checks the JSON grammar first and only then reads
  the token's value (*Parsing a jsonb value*).
- **Strings.** U+0000 to U+001F must be escaped. The escapes are `\" \\ \/
  \b \f \n \r \t \uXXXX`; a `\u` escape decodes to UTF-8, a surrogate
  pair to one code point. The input must be valid UTF-8 (22021, as a
  `TEXT` value, before parsing).
- **Errors.** A syntax error is 22P02 `invalid input syntax for type
  json` with PG 17's DETAIL and CONTEXT. These are the DETAILs `jsonb_in`
  gives in a UTF8 database:

  | Input | DETAIL |
  |---|---|
  | `''`, `'   '`, `'[1,2'`, `'{"a"'` | `The input string ended unexpectedly.` |
  | `'[1,]'`, `'[}'`, `'{"a":}'` | `Expected JSON value, but found "]".` (or `"}"`) |
  | `'{"a":1,}'` | `Expected string, but found "}".` |
  | `'{1:2}'`, `'{,}'` | `Expected string or "}", but found "1".` (or `","`) |
  | `'{"a" 1}'`, `'{"a",1}'` | `Expected ":", but found "1".` (or `","`) |
  | `'{"a":1 "b":2}'` | `Expected "," or "}", but found ""b"".` |
  | `'[1 2]'`, `'[1,2}'` | `Expected "," or "]", but found "2".` (or `"}"`) |
  | `'1 2'`, `'[1]]'` | `Expected end of input, but found "2".` (or `"]"`) |
  | `'{a:1}'`, `'01'`, `'.5'`, `'+1'` | `Token "a" is invalid.`, `Token "01" is invalid.`, `Token "." is invalid.`, `Token "+" is invalid.` |
  | `'"abc'` (never closed) | `Token ""abc" is invalid.` |
  | `E'"a\tb"'`, `E'"a\nb'` | `Character with value 0x09 must be escaped.` (or `0x0a`) |
  | `'"\x"'` | `Escape sequence "\x" is invalid.` |
  | `'"\u00"'`, `'"\u00zz"'` | `"\u" must be followed by four hexadecimal digits.` |
  | `'"\ud83d"'`, `'"\ud83dx"'`, `'"\ude00"'`, `'"\ud83dA"'` | `Unicode low surrogate must follow a high surrogate.` |
  | `'"\ud83d\ud83d"'` | `Unicode high surrogate must not follow a high surrogate.` |

  **The extent of an invalid token.** A word (letters, digits, `_` and
  bytes ≥ 0x80) is one token: `truex`, `True`, `é`. A number is read by
  its grammar, an unfinished part included, and then runs over the word
  bytes that follow it: `-01`, `5.`, `1e`, `1.5e+`, `1e5x`, `1_000`, `1é`,
  and `1` followed by U+00A0 (both of its bytes inside the token). A
  string never closed runs to the end of the input. Any other byte is a
  token of one byte: `.` in `.5`, `1.2.3` and `1e5.`, `+`, `-` alone, and
  a raw form feed, written inside the quotes as the byte itself.

  **CONTEXT** is `JSON data, line N: …`: the line of the error, shown
  from its start, or from `...` and at most 49 bytes ending at the bad
  token's end; when that window cuts a multibyte character, it starts at
  the next character boundary (40 `é` before the token show 48 bytes).
  `...` follows when input remains after the token. PG 17: `E'{\n"a":\n
  [1,\n x]}'` gives `JSON data, line 4:  x...`; an 80-byte bad token
  shows `...` and its last 49 bytes. For an error inside a `\u` escape
  the shown text is PG 17's own (`'"\ud83dA"'` shows `"\ud83dA...`),
  recorded per case rather than derived.
- **`\u0000`.** jsonb refuses it: 22P05 `unsupported Unicode escape
  sequence`, DETAIL `\u0000 cannot be converted to text.`
- **Number range.** A number past `numeric`'s range is 22003 `value
  overflows numeric format` (`1e131072`, `1e-16384`, `0e-16384`).
  `1e131071` and `9.99e131071` are accepted; each prints 131072 bytes.
  PG 17's edge rules, which `numeric_in` (`sql/text.cppm:1126-1235`)
  already follows and `numeric_parse` keeps:
  - The exponent is bounded first, at INT32_MAX / 2: `0e1073741824` and
    `1e1073741824` are 22003.
  - Within that bound a zero has no weight bound: `0e131072`, `0e1000000`
    and `0e1073741823` are `0`.
  - The weight counts from the first significant digit: `0.0001e131075`
    is accepted, equal to `1e131071`.
  - The scale is the fraction digits minus the exponent, trailing zeros
    included: `100e-16385` is 22003 though its value is `1e-16383`, and
    `-0.0e-16382` is accepted as `0.` and 16383 zeros, 16385 bytes.
- **Depth.** PG 17 has no fixed bound: nesting is limited by the stack
  (54001 `stack depth limit exceeded`, with a HINT about
  `max_stack_depth`). At 2MB the largest nesting accepted is about 14.5k
  for objects and about 16.4k for arrays, and varies by a few levels with
  the call path; 10000 parses as arrays and as objects. The layer parses
  iteratively and fixes the bound at 10000 (owner's decision 9), past
  which it raises the same 54001 and message, without the HINT.
- **Size.** A string past 268435455 bytes is 54000 `string too long to
  represent as jsonb string` with PG 17's DETAIL `Due to an implementation
  restriction, jsonb strings cannot exceed 268435455 bytes.`; a container
  past the same bound is 54000 `total size of jsonb array elements exceeds
  the maximum of 268435455 bytes` (or `object`). The statement budget and
  the engine's value limit usually refuse first (*Limits on a jsonb
  value*).

### jsonb normalisation

- **Duplicate keys: the last wins**, whatever the types.
  `'{"a":1, "a":{"x":1}, "a":[2]}'` is `{"a": [2]}`.
- **Key storage order: shorter first, then bytewise**, at every level,
  under any collation (PG 17 printed the same order in an ICU `en`
  database). `'{"aaa":1,"b":2,"ab":3,"B":4,"é":5,"z":6}'` is `{"B": 4,
  "b": 2, "z": 6, "ab": 3, "é": 5, "aaa": 1}`.
- Array order is kept. Whitespace is dropped. Strings are rewritten in
  the output's escaping.

### jsonb output text

Output is PG 17's `jsonb_out`, byte for byte:

- Separators are exactly `", "` and `": "`, with no space inside brackets;
  empty containers print `{}` and `[]`. `'  [1 , 2,{ "x" : [ ] ,
  "y":{}} ]  '` is `[1, 2, {"x": [], "y": {}}]`.
- Numbers print as `numeric_out`: `1.0` stays `1.0`, `1e2` is `100`,
  `1.000e2` is `100.0`, `-0` is `0`, `-0.0` is `0.0`, `0.0e-3` is
  `0.0000`.
- Strings escape `"` and `\`, write `\b \f \n \r \t` in their short form
  and every other byte below 0x20 as `\u00xx` in lowercase hex; `/`, 0x7F
  and all non-ASCII are written raw.
- `jsonb::text` is the same text: `'"abc"'::jsonb::text` is `"abc"` with
  its quotes.

### jsonb numbers

A jsonb number is a `numeric`: its digits and its display scale are kept,
and it compares by value. Its text is `numeric_out`'s (`-0` becomes `0`,
`-0.0` becomes `0.0`, as in PG 17).
`'123456789012345678901234567890.123456789012345678901234567890'`
round-trips exactly.

### jsonb equality, ordering and hashing

PG 17's btree `jsonb_ops`, in a C or C.UTF-8 database:

- **Equality** is structural, with numeric equality: `'1.0' = '1'`,
  `'-0' = '0'`, key order irrelevant, array order relevant (`'[1,2]' =
  '[2,1]'` is false).
- **Order between types:** the empty top-level array below everything,
  then null < string < number < boolean < array < object.
  `'[]' < 'null'`, `'[]' < '""'` and `'[]' < '{}'` are true; `'"z"' <
  '[]'` is false. Inside an array or object the rule is the plain type
  order: `'[null]' < '[[]]'`, and `'[[]]' < '[null]'` and `'{"a":[]}' <
  '{"a":null}'` are false.
- **Arrays:** fewer elements first, then element by element (`'[9]' <
  '[1,1]'`).
- **Objects:** fewer pairs first, then key 1, value 1, key 2, value 2, …
  walking the pairs **in storage order**: `'{"b":1,"aa":1}' <
  '{"c":1,"a":1}'` is false, because the storage orders are [b, aa] and
  [a, c]. Keys compare bytewise, not length first: `'{"z":1,"aa":1}' <
  '{"ab":1,"ac":1}'` is false.
- **Strings and keys** compare under the database's default collation;
  the layer's one collation is C, so bytewise. PG 17's verified 39-value
  sort, which the functional cases replay:

  ```
  [] < null < "" < "10" < "9" < "B" < "a" < "ab" < "b" < "é" < -1 < 0 < 1.00 = 1 = 1.0 < 100 = 1e2 < false < true
   < [null] < ["a"] < [1] < [2] < [true] < [[]] < [[1]] < [{}] < [1, 2] < [1, 1, 1]
   < {} < {"a": null} < {"a": 1} < {"a": 2} < {"a": [1]} < {"aa": 0} < {"b": 0} < {"z": 0} < {"a": 1, "b": 1} < {"b": 1, "c": 1}
  ```

- **Hashing** agrees with equality: `GROUP BY` and `count(DISTINCT j)`
  merge `1`, `1.0` and `1.00`, by hash or by sort. A group prints one of
  its input values, whichever PG 17 met first (`GROUP BY` over 1.0, 1 and
  1.00 printed `1.0`; `DISTINCT` printed `1.00`); the layer prints its
  first row's value, never a normalised form (*jsonb as a sort key and
  group key*).
- `COLLATE` on jsonb is 42804 `collations are not supported by type
  jsonb`, in an expression and in `ORDER BY j COLLATE "C"`.
- `MIN` and `MAX` of jsonb are 42883 `function max(jsonb) does not
  exist`, in PG 17 and here. PG 17 has `GREATEST`, `LEAST` and `BETWEEN`
  on jsonb (`greatest('{"a":1}','[1]')` is `{"a": 1}`).

### Casts to and from jsonb and json

From PG 17's `pg_cast` and its automatic I/O casts:

- **Text to jsonb or json**: explicit only (`'…'::text::jsonb`,
  `varchar::jsonb`), parsing the text. An unknown literal is read by
  `jsonb_in` wherever jsonb is expected: `INSERT … VALUES ('{"a":1}')`,
  `j = '{"a":1}'`. A text expression assigned to a jsonb column is 42804
  `column "j" is of type jsonb but expression is of type text`, HINT `You
  will need to rewrite or cast the expression.`
- **jsonb or json to text, varchar, char**: explicit, and in assignment
  to a string column, giving the output text. An explicit cast cuts it
  (`::varchar(5)` is `{"a":`, `::char` is `{`); assignment to a
  `varchar(5)` or `char(3)` column does not cut it but is 22001 `value too
  long for type character varying(5)` (`character(3)`), as for any text
  value. The binder's `type_refusal` (`sql/binder.cppm:479-486`) already
  lets any class assign to a string.
- **json to jsonb and jsonb to json**: assignment casts (`pg_cast`
  context `a`), not implicit: `json @> jsonb`, `jsonb || json` and
  `jsonb = json` are 42883. `json::jsonb` normalises, and raises what
  `jsonb_in` raises on the json's text (22P02 for an unpaired surrogate,
  22P05 for `\u0000`, 22003 past numeric's range); `jsonb::json` is the
  jsonb text.
- **jsonb to boolean, smallint, integer, bigint, real, double precision,
  numeric**: explicit only. A number to an integer rounds half away from
  zero (`'1.5'` is 2, `'-2.5'` is -3); a type mismatch is 22023 `cannot
  cast jsonb <string|null|array|object|boolean|numeric> to type <target>`;
  a range error is the target's (22003 `integer out of range`, `"1000…0"
  is out of range for type double precision`).
- **Anything else** to or from jsonb is 42846 `cannot cast type integer
  to jsonb`, `cannot cast type jsonb to bytea`.
- `jsonb || text` and `text || jsonb` are text concatenation:
  `'{"a":1}'::jsonb || 'x'::text` is `{"a": 1}x` and `'x'::text ||
  '{}'::jsonb` is `x{}`, type text. The layer's `||` of a string and a
  non-string (`bc_concat`) covers it once the operand is the jsonb's text.

### SQL NULL and JSON null

- A jsonb `null` is a value, not SQL NULL: `'null'::jsonb IS NULL` and
  `('{"a":null}'::jsonb -> 'a') IS NULL` are false.
- `->>`, `#>>` and the `_text` functions turn a JSON null into SQL NULL:
  `'null'::jsonb #>> '{}'` is SQL NULL.
- A missing key, an index out of range, a key on an array and an index
  on an object give SQL NULL from `->`, never an error.
- **A top-level scalar** answers `->` with index 0 or -1 as a
  one-element array: `'1'::jsonb -> 0` and `-> -1` are `1`, `'"abc"'::jsonb
  ->> 0` is `abc`; `-> 1` and `-> -2` are SQL NULL, and so is a key
  (`'"abc"'::jsonb -> 'a'`). A path does not do this: `'1'::jsonb #>
  '{0}'` is SQL NULL. Top-level is the node the handle holds, so a
  scalar returned by `->` or `#>` behaves the same: `('{"a":1}'::jsonb ->
  'a') -> 0` is `1` and `('{"a":"x"}'::jsonb -> 'a') ->> -1` is `x`.
- **Negative indexes** count from the end: `'[1,2,3]'::jsonb -> -3` is
  `1`, `-> -4` is SQL NULL, `'[1,2]'::jsonb #> '{-1}'` is `2`.
  `->` and `- integer` take an `integer`, so `-> -2147483648` is SQL NULL.
  A path element is read as an `integer`: past its range or not a number
  (`'{2147483648}'`, `'{x}'`), `#>` gives SQL NULL on an array, while
  `#-`, `jsonb_set` and `jsonb_insert` are 22P02 `path element at position
  1 is not an integer: "x"`. The index arithmetic is overflow-safe
  (*Decoding a stored jsonb value without overflow*).
- **An empty path** returns the whole value: `#> '{}'` is the value;
  `'{"a":1}'::jsonb #>> '{}'` is `{"a": 1}`, `'[1]'::jsonb #>> '{}'` is
  `[1]`, and `'"x"'::jsonb #>> '{}'` is `x`, unquoted (Mattermost's
  migrations use `#>>'{}'`).
- Every operator and almost every function is strict: SQL NULL in gives
  SQL NULL out, `jsonb_set` in any argument included (its documented
  trap). `jsonb_set_lax`, `jsonb_build_*`, `to_jsonb` of NULL and the
  aggregates have their own rules, recorded per function in stage 4 and
  5's cases.

### The json type

`json` (OID 114) is validated by the same parser and kept as its input
text, verbatim: whitespace, key order, duplicates and escapes kept. Its
validation skips two of jsonb's checks: it accepts `\u0000` and unpaired
surrogates (`'"\ud83d"'::json`, `'"\ude00"'::json` and
`'"\ud83d\ud83d"'::json` are stored as written), while a `\u` without
four hex digits is still 22P02. It also accepts numbers past `numeric`'s
range (`'1e1000000'::json` is accepted; its cast to jsonb is 22003). Its
output is its input. Its binary wire form is the text with no version
byte.

json has no equality or order, and PG 17 refuses every use that needs
one:

- `json = json`: 42883 `operator does not exist: json = json`; `j IN
  ('1')`: 42883 `operator does not exist: json = unknown`.
- `ORDER BY` a json value: 42883 `could not identify an ordering operator
  for type json`, HINT `Use an explicit ordering operator or modify the
  query.`
- `GROUP BY`, `SELECT DISTINCT`, `UNION`, `INTERSECT`, `EXCEPT` and
  `count(DISTINCT …)`: 42883 `could not identify an equality operator for
  type json`.
- `UNION` or `COALESCE` of json with jsonb: 42846 `UNION could not
  convert type jsonb to json` (`COALESCE could not convert …`).
- `PRIMARY KEY`, `UNIQUE` and `CREATE INDEX` on json: 42704 `data type
  json has no default operator class for access method "btree"`, HINT
  `You must specify an operator class for the index or define a default
  operator class for the data type.` The layer has no json column (below),
  so it refuses the column itself first.

The layer builds `json` in stage 1c as a value and a result type, with
its casts, these refusals and OID 114, and in stage 4 and 5
`json_build_*` and `json_agg`. A json value inside a plan is a bytes view
of its text (`BC_T_BYTES`); the binder's type class keeps it apart from
text and jsonb. **A json column is refused** (`CREATE TABLE t (j json)`
and `ADD COLUMN … json`, 0A000; owner's decision 11): neither Mattermost
nor Django has one, so no stored record holds the json kind. Operators on
json, which walk its text on every call (`'{"a": [1,  2]}'::json -> 'a'`
is `[1,  2]`, the source text), are refused until a later design.

## jsonb storage format

### Binary layout of a jsonb value

A stored jsonb value is the bytes of one variable-region column
(`types_design.md`, *Row format*): a format byte, then one node. Integers
are little-endian and read by copying, as every row field is; nothing
depends on alignment.

```
value   := u8 format (0xF8), node          ; the node runs to the value's end
node    := u8 tag, body                    ; its extent is given by its parent
tag 0x01 null     body: empty
tag 0x02 false    body: empty
tag 0x03 true     body: empty
tag 0x04 number   body: u8  sign            ; 0 zero, 1 positive, 2 negative
                        u16 dscale          ; places shown after the point, 0..16383
                        i32 weight          ; power of ten of the first digit, -16383..131071; 0 for zero
                        u8[] digits         ; ASCII '0'..'9', first and last not '0'; none for zero
tag 0x05 string   body: u8[]               ; UTF-8, de-escaped, no NUL byte
tag 0x06 array    body: u32 n, u32 end[n], elements
                                            ; element i is [end[i-1], end[i]) of elements, end[-1] = 0
tag 0x07 object   body: u32 n, u32 key_end[n], u32 val_end[n], keys, values
                                            ; key i is [key_end[i-1], key_end[i]) of keys,
                                            ; value i is [val_end[i-1], val_end[i]) of values
```

- **Numbers** are PG's own `numeric` model in base 10: sign, weight,
  significant digits and display scale. A non-zero number is
  `0.d1d2…dk × 10^(weight+1)`, printed with `dscale` places after the
  point. `123.45` is digits `12345`, weight 2, dscale 2; `0.0015` is `15`,
  weight -3, dscale 4; `100` is `1`, weight 2, dscale 0; `1.0` is `1`,
  weight 0, dscale 1. The last digit's place, `weight - k + 1`, is never
  below `-dscale`. The bounds are `numeric`'s: at most 131072 digits
  before the point and 16383 after it, so `1e131071` is a 10-byte value
  whose text is 131072 bytes. `numeric_out`'s text is rebuilt from the
  parts on output.
- **Strings** are stored de-escaped, so `->>` of a string is a view of
  its bytes with no copy; output escapes them again.
- **Keys** are strings without a tag, strictly increasing in storage
  order (de-escaped length, then bytes). That makes them unique and lets
  `-> 'k'` binary-search `key_end` without reading a value.
- **Arrays and objects** hold tagged children whose extents come from the
  end tables, so a scalar carries no length and `-> i` is one table read.
  An empty container is `n` = 0 with no table and an empty area.
- **The format byte versions the layout.** 0xF8 is this layout; a later
  one takes the next byte, up to 0xFF, and the reader dispatches on it,
  so a new layout is read beside old rows without a migration. 0xF8 can
  never start UTF-8 and is not PG's wire version byte (0x01), so
  canonical text, a wire jsonb (`0x01` and its text) or any text that
  reaches the stored-form path is `BC_E_CORRUPT`, never a value. (A format
  byte of 0x01 would have read the text `\x01\x01` as a stored `null`.)
- **A sub-value is a node.** Inside a plan a jsonb value is a node, not a
  value with its format byte (*Physical type BC_T_JSONB and the bc_jsonb
  handle*). The format byte is checked once when a column is read and
  written once when a row is set, so `->` and `#>` return a slice of their
  source with no copy.

### Why binary and not canonical text

The reviewed alternative stored jsonb as its canonical text, `jsonb_out`'s
output, and scanned it on every operator call. The owner chose binary
(*Owner's decisions on jsonb*, Q1). Measured on a 1,489-byte canonical
object of 40 keys (g++ -O2, one core):

| Operation | Canonical text | Binary |
|---|---|---|
| `->> 'key_001'` (first key) | 16 ns | 40 ns |
| `->> 'key_040'` (last key) | 793 ns | 25 ns |
| output | 15 ns (copy) | about 2,000 to 2,300 ns (walk and escape) |

What binary buys: a key lookup is O(log n) and does not grow with the
value; `=`, the sort key and `@>` walk the tree without a parse; a string
is a slice; a number is stored at the size of its significant digits, so
a write never expands it (`1e131071` is 10 bytes, where text stores
131072). It is also what the type's name promises and what PG 17 stores.

What it costs: output is a walk where text output was a copy, about 2 µs
per 1.5 KB, or about three rows of the layer's own scan (700 to 750 ns a
row, `planner_design.md` :361-365). ORMs and Mattermost's post fetches
read whole `Props`, so this is paid often. PG 17 pays the same way: over
200k rows of about 1.6 KB, `jb->>'key_040'` took 875 ms against 745 ms
for `length` of a text column, and `jb::text` took 2212 ms. Binary also
adds an encoder, a bounds-checked decoder with its fuzz target, and a
format to version.

**Output cost is bounded before it is paid.** Output is two passes over
the node: the first computes the text's length, the second writes it.
The length is checked against 1 GiB - 1 and charged to the statement's
budget before a byte is written, so a value whose text expands (a number
like `1e131071`, a string of control bytes printed as `\u00xx`) is 54000
at a predictable point, and the walk is linear in the text it writes.
Both passes run one emitter, templated on its sink: a counting sink for
the first, a writing sink for the second, so escapes, UTF-8 and number
text are coded once and the passes cannot disagree. The writing sink
checks its bound on every write and fails `BC_E_CORRUPT` rather than
writing past the length the first pass gave.

### Limits on a jsonb value

- **Depth:** 10000 nested containers (54001 on input). Every walk (parse,
  encode, output, sort key, containment, modification) is iterative with
  an explicit stack charged to the budget, so no input reaches the C++
  stack. A stored value nested deeper than 10000 is `BC_E_CORRUPT`, so
  the stack is bounded even when the budget is none (`max_bytes` 0,
  `bc_plan.h:117`). Every path that builds a container checks depth as it
  builds: a modification or builder's result is as deep as its path, plus
  the inserted value's depth, plus one, and past 10000 it is 54001 at
  construction. So `jsonb_build_array(j)`, `jsonb_set`, `||` or
  `jsonb_agg` over a stored depth-10000 value is a user error, never a
  value later read as `BC_E_CORRUPT`.
- **One value:** at most 268435455 bytes (2^28 - 1) encoded, PG 17's
  bound for one container and one string, and the engine's hard value
  ceiling (`codec/row_codec.cppm:48`). Every offset therefore fits a u32,
  because every encoder path enforces the bound itself, whatever the
  budget (`max_bytes` 0 checks nothing, `take()`, `plan_api.cpp:1330`):
  the parser's size pass, concat, delete, set, insert, strip_nulls, the
  builder's close and each `bc_agg_jsonb_add_*` compute the result's size
  in `size_t` before writing any table entry and check it against
  268435455. Past it is 54000 with PG's messages (*jsonb input grammar
  and its errors*, Size).
  A stored value is further bounded by its row: the engine's configured
  `max_value_bytes` (4 MiB by default, `include/bytecask.hpp:154`) at
  commit, and a write plan by the 1 GiB batch limit. Both are 54000 today
  and stay so.
- **A computed value** (a `jsonb_agg`, a `||`, an output text) is bounded
  by the statement's memory budget (`statement_memory_bytes`, 64 MiB by
  default, `api/database.cppm:1271`): 54000 at the call that passes it.
  In a transaction, jsonb writes also meet the transaction's own bound
  (`TransactionOptions::max_bytes`, 64 MiB, `:541`) long before the 1 GiB
  batch limit.
- **Any text a call returns** is at most 1 GiB - 1 bytes (PG's
  `MaxAllocSize`), refused with 54000 before it is allocated, so a view's
  32-bit length never wraps whatever the budget.

### Parsing a jsonb value

Input text becomes a binary value in three passes over charged memory.
Passes 1 and 3 are linear in the input; pass 2 is O(p log p) for an
object of p pairs.

1. **Parse** into a node array, in document order: per value its kind,
   its source span and, for a container, its child count. Strings keep
   their source span, their de-escaped length and a flag for whether they
   hold a `\`. Numbers are read by the number scanner below into their
   parts. The parser is iterative, with its stack of open containers on
   the array itself.
2. **Order keys.** For each object, sort an index of its pairs by key in
   storage order (de-escaped length, then de-escaped bytes; a key with a
   `\` is de-escaped once into a charged buffer), and keep the last of
   equal keys. The sort is `std::sort` on (key, source position), which
   allocates nothing, so the last of equal keys is the highest position;
   `std::stable_sort` would allocate an uncharged buffer.
3. **Size, then write.** Compute each node's encoded size bottom-up (the
   array read backwards, children before parents), charge the total, and
   write the value once, top-down, with each table filled from the sizes
   already known. Sizes are summed in u64 and each node's is checked
   against 268435455 as it is computed, so the first node past it stops
   the parse with 54000 and a stored u32 size never wraps. A node's span
   and child count are u32, which holds because input is under 2^30.

No subtree is copied as its level closes, so the cost does not grow with
depth. Peak memory is the node array (a node of 28 bytes, at most (L + 1)
/ 2 nodes for L input bytes, so about 14 bytes per input byte), the key
index (4 bytes per pair), the de-escaped keys that hold a `\` and the
encoded value, which is never larger
than the input plus one format byte, 3 bytes per container, 4 per
element, 8 per pair and 8 per number.
Each is charged as it grows, so a value too large for the budget is
54000 at a predictable point.

**The number scanner.** `numeric_in` (`sql/text.cppm:1126-1235`) builds
its digits in an uncharged `std::string`, throws `TextError` (which the
plan guard would report as `BC_E_CORRUPT`, 58030, `plan_api.cpp:1009-1010`)
and returns text. It is split in two: `numeric_parse`, which never throws
and yields `{negative, digits, whole, scale}` with the digits in a buffer
the caller gives, and `numeric_text`, which writes the text from those
parts. NUMERIC calls both, as today. jsonb gives `numeric_parse` a charged
buffer the size of the token, checks the range from `whole` and `scale`
(22003), and stores sign, weight, the digits without leading or trailing
zeros, and the scale. Nothing is expanded: `1e131071` costs its 8 input
bytes and 10 stored bytes, and only its output writes 131072, charged
first. One parser serves NUMERIC and jsonb.

**json's mode.** For `json` the parser runs pass 1's grammar alone: the
same tokens, DETAILs and CONTEXT, no node array, no key ordering, no
number parts and no encoding. A number is checked by the JSON grammar
only, and `\u0000` and unpaired surrogates are not refused (*The json
type*), so `'1e1000000'::json` and `'"\ud83d"'::json` are accepted. Its
memory is a charged stack of open containers, bounded by the same depth
limit.

### Decoding a stored jsonb value without overflow

A damaged value is an error, `BC_E_CORRUPT` (58030), never undefined
behaviour. The engine's CRC already refuses a record damaged on disk, so
what reaches jsonb code is the encoder's own output, or a bug. Every read
follows these rules, and a break of any is `BC_E_CORRUPT`:

- **Positions are `size_t`, and every number read from the value is
  checked against its extent before use.** A check subtracts from the
  extent rather than adding to a position, so it cannot wrap.
- **The value** starts with a known format byte and holds at least one
  byte after it.
- **A container** of extent [b, e) has `e - b >= 5`. Its count is checked
  before any multiply: `n <= (e - b - 5) / 5` for an array (a table entry
  and at least a tag per element) and `n <= (e - b - 5) / 9` for an object
  (two table entries and at least a tag per value). The table's end, `b +
  5 + 4n` or `b + 5 + 8n`, is computed only after that check and cannot
  overflow.
- **An empty container** (`n` = 0) has extent exactly 5.
- **Areas.** An array's elements area is `[b + 5 + 4n, e)`. An object's
  keys area is `[k, k + key_end[n-1])` with `k = b + 5 + 8n` and
  `key_end[n-1] <= e - k`; its values area runs from there to `e`. A
  value lookup reads and checks `key_end[n-1]` before it finds the values
  area.
- **End tables:** each element's and value's slice is non-empty
  (`end[i] > end[i-1]`), each key's slice is non-decreasing (a key may be
  empty), every end lies inside its area, and the last element or value
  end equals its area's size, so no trailing bytes. A lookup checks the
  two ends of the slice it reads, both inside the area and ordered
  (`prev <= cur` for a key, `prev < cur` for an element or value), so a
  length `cur - prev` never wraps; a full walk checks all of them.
- **An array index** `i` (an `int64_t`) is resolved without overflow:
  for `i >= 0` it must be `< n`; for `i < 0`, `i >= -int64_t(n)` (safe,
  as `n <= 2^28`) and then the index is `n + i`. It is never written
  `n - (-i)`, which overflows at `INT64_MIN`.
- **A scalar's extent:** `null`, `false` and `true` are exactly one byte;
  a number is at least eight. An unknown tag is damage.
- **A number:** sign 0, 1 or 2; `dscale <= 16383`. Zero has weight 0 and
  no digits. A non-zero number has `-16383 <= weight <= 131071`, from 1
  to 147455 ASCII digits whose first and last are not `'0'`, and
  `weight - k + 1 >= -dscale`, computed in `int64_t` after the range
  checks. A number's output length is then computed in `size_t` without
  overflow: `(negative ? 1 : 0) + (weight >= 0 ? weight + 1 : 1) +
  (dscale > 0 ? 1 + dscale : 0)`. A weight below 0 prints one integer
  digit, `0`: `0.0015` (weight -3, dscale 4) is 6 bytes, and `1e-16383`
  is 16385.
- **A string or key** holds no 0x00 byte. Every walk that copies its
  bytes (output, the sort key, `->>` and the other text results) checks
  this alongside the copy, so a damaged value cannot make two sort keys
  share a prefix.
- **Depth:** every walk keeps an explicit stack and stops at a depth past
  10000. No walk recurses.

### Validation of a stored jsonb value

- **Full check on write and at load.** `bc_row_set_jsonb` and the
  catalog's load of a default check the whole value: every rule above,
  keys strictly increasing in storage order, and every string and key
  valid UTF-8 with no NUL byte. One iterative walk with its explicit stack
  and nothing else allocated. A value the layer's parser built passes by
  construction; the check catches a bug before it is stored.
- **Bounds checks on every read.** Output, the sort key, containment and
  the accessors check what they visit by the rules of *Decoding a stored
  jsonb value without overflow*, and nothing more: `-> 'k'` on a value
  whose keys are out of order could answer NULL where a full check would
  raise `BC_E_CORRUPT`. It never reads outside the value.
- **Text that leaves a jsonb is UTF-8.** Every call that returns a jsonb
  string or key as text checks it, O(its length): `->>`, `#>>`,
  `jsonb_object_keys`, `jsonb_each_text`, `jsonb_array_elements_text`
  and stage 6's other set-returning functions. Output checks each string
  as it escapes it,
  so a damaged value cannot hand invalid UTF-8 to a text column or a
  client.

The fuzz target of *Tests of jsonb by stage* holds every read to this:
any byte string, through every call, gives a result or `BC_E_CORRUPT`,
under ASan and UBSan.

### jsonb as a sort key and group key

The value's bytes are not its order (keys are in storage order; `1.0` and
`1` differ). Comparison uses a second, order-preserving encoding, the
**jsonb sort key**, built by one walk of the node, written only into sort
keys, group keys, hash keys and subquery sets, and never decoded:

```
top-level empty array : 0x01
null                  : 0x02
string                : 0x03, its bytes, 0x00
number                : 0x04, then 0x01 negative | 0x02 zero | 0x03 positive,
                        then for non-zero: weight as i32 big-endian, sign bit flipped,
                        significant digits as ASCII, 0x00;
                        every byte after the sign class inverted when negative
false / true          : 0x05 0x00 / 0x05 0x01
array                 : 0x06, n as u32 big-endian, each element's encoding
object                : 0x07, n as u32 big-endian, then per pair in storage order:
                        key's bytes, 0x00, value's encoding
```

- **Order.** `memcmp` order is PG 17's order and byte equality is jsonb
  equality: the count before the elements gives length first, the tag
  gives the type order, digits without leading or trailing zeros make
  `1`, `1.0` and `1.00` one encoding (the stored digits already are), and
  the empty top-level array's own tag puts it below `null`. "Top-level"
  is the node the key is built from, so `j -> 'a'` holding `[]` takes
  0x01, as PG 17's `->` result is a value of its own.
- **Prefix-free.** No encoding is a proper prefix of another: a string or
  key holds no NUL (`\u0000` is refused, and a stored NUL is
  `BC_E_CORRUPT`), so its one 0x00 ends it; digits are ASCII and end in
  0x00, or 0xFF inverted. So the key is appended to a sort key unescaped,
  concatenates with other parts, and a `DESC` part inverts it, as
  `types_design.md`, *Key encoding*, does for every part.
- **Weight.** The layout bounds it to [-16383, 131071], checked on read,
  so it fits an i32 and its flipped big-endian form orders correctly.
- **Size.** A node's sort key is at most its stored length plus one: a
  number's key is no longer than its node, a string's or boolean's is one
  byte longer, and inside a container every child's 4-byte table entry
  (8 for a pair, whose key gains one byte) more than covers that byte.
  `sort_value_max_bytes` (`algebra/plan_api.cpp:2582`), whose switch has
  no jsonb case today (its `Num` case throws `logic_error`), gains `case
  PhysType::Jsonb: return len + 1;`, `len` the node's length without the
  format byte, and charges it before the key is built. The builder checks
  its write position against that bound, so a bug fails rather than
  overruns.

**The value a group prints.** A group's printed value is its first row's
value, never one rebuilt from the sort key, which has lost the scale. A
sorter's or hash table's payload already carries the row's value, so
`DISTINCT` through a sorter and `UNION`, `INTERSECT` and `EXCEPT` print
the first of each run of equal keys. An aggregate's group keeps only its
key (`bc_agg_next`, `plan_api_design.md` :947, :991), so stage 2 adds an
aggregate slot that keeps the first node folded into it (*bc_jsonb_\*
calls by stage*).

**As a stored key** (primary key, `UNIQUE`, index column) jsonb is
refused with 0A000 `DdlRefused` in these stages, as NUMERIC is
(`refuse_key_type`, `catalog/catalog.cppm:1897-1910`); *Next phase of
jsonb* describes it.

## Plan API for jsonb

Each stage that adds calls bumps the revision (20 for stage 0, then one
per stage) and lists them in `plan_api_design.md`'s status line.

### Physical type BC_T_JSONB and the bc_jsonb handle

- `enum bc_type` gains `BC_T_JSONB = 6`, and `PhysType::Jsonb = 6`
  (`codec/key_codec.cppm:41`, with the `plan_api.cpp:127` static_assert).
  It is held in the row's variable region and is never a key part.
- **Its own handle.** `bc_plan.h` gains `typedef struct { uint32_t i;
  uint64_t g; } bc_jsonb;`, a view of one binary node (*Binary layout of
  a jsonb value*, without the format byte) in the same view table, its
  entry tagged as jsonb. Only `bc_*jsonb*` calls take or return
  it, so a plan that passes one to `bc_emit_bytes`, `bc_concat`,
  `bc_like`, a `TEXT` column or a bytes sort key does not compile. C
  still lets a plan forge either handle from the other's fields
  (`bc_view v = {j.i, j.g}`), so the tag is checked both ways:
  `ViewEntry` (`plan_api.cpp:159-167`) gains its own jsonb tag field,
  apart from `RowKind`; every jsonb resolve refuses an untagged entry and
  every bytes resolve (the generic `resolve()`, `plan_api.cpp:1498`, used
  by `bc_emit_bytes`, `bc_concat`, `bc_row_set_bytes` and the rest)
  refuses a tagged one, each with `BC_E_HANDLE`. This is what keeps text
  and binary apart: a plan cannot hand a text view to a jsonb call or a
  node to a text call.
- **Reading and setting.** `bc_col_jsonb` reads a jsonb column, checks
  its format byte and returns the node after it as a sub-view that shares
  the row's owner (`cx->owner`,
  `plan_api.cpp:1514-1518`) and dies when the row's loop steps (`CONTRACT.md`
  :65). `bc_col_bytes` on a jsonb column and `bc_row_set_bytes` into one
  are `BC_E_HANDLE`: `declared()` (`plan_api.cpp:3009`) names `Jsonb`
  explicitly, not through the Bytes/Num exception. `bc_row_set_jsonb`
  sets a column from a `bc_jsonb`, after the full check, writing the
  format byte and the node; `bc_row_set_param` sets one from a
  parameter's text (parsed and encoded).
- **Text in and out.** `bc_jsonb_in(cx, bc_view text) -> bc_jsonb` parses
  and encodes text (a cast, a parameter, a folded constant);
  `bc_jsonb_text(cx, j) -> bc_view` writes the canonical text into a new
  charged bytes view, for `::text`, `||` with text and assignment to a
  string column. `bc_emit_jsonb` and `bc_emit_col` of a jsonb column write
  the text. These are the only two crossings between the forms.
- **Every predicate on `PhysType` is an exhaustive switch.** Today
  several are written as comparisons that a new type would fall through
  silently: `is_fixed` is `t != Bytes && t != Num`
  (`codec/key_codec.cppm:44`, so Jsonb would count as fixed width),
  `layout_valid`'s range check is `code > PhysType::Num`
  (`codec/row_codec.cppm:721`, so a Jsonb entry would read as damage),
  and `key_suffix` and `carry_suffix` test only `== Num`
  (`plan/codegen.cppm:187, 194`). Stage 1 rewrites each as a switch with
  no `default:`, and a unit test checks each predicate for every value of
  the enum.

### One form for a jsonb value at each boundary

Inside the layer a jsonb value is binary: in a row and behind a
`bc_jsonb` handle. At every edge it is canonical text, `jsonb_out`'s
output, so no `Datum`, wire message or Python value ever holds the binary
form:

- **A parameter** from the user is text (in text or binary wire format,
  *Wire OIDs and formats for jsonb and json*) and is parsed once per execution by
  `bc_param_jsonb(cx, i) -> bc_jsonb`, which keeps the encoded node,
  charged, for the rest of the execution.
- **A constant** (`'{"a":1}'::jsonb`, a literal resolved as jsonb) is
  parsed at prepare, so its 22P02 comes before the statement runs, and
  folded as its canonical text into the hidden parameter it becomes
  (`binder.cppm:5695`), which `bc_param_jsonb` encodes once per
  execution, as a user's parameter.
- **A subquery's value** is what the plan emits, canonical text: a
  `BC_SUB_VALUE` of jsonb folds it into a jsonb parameter, and a
  `BC_SUB_SET` of jsonb folds each value's sort key, probed with
  `bc_sub_has_jsonb`, so `j IN (SELECT …)` finds `1.0` for `1`.
- **A catalog default** is stored as canonical text and parsed at load
  into the node the rows use; text that does not parse is a refused
  catalog. Text keeps the catalog record and Python's `default` free of
  the binary form, and the load's parse is the full check.
- **A result** (`sql::Rows`, a `Datum` of `Bytes` with the column's
  `SqlKind::Jsonb`) is canonical text. Python's `to_python`
  (`python/bytecask_sql_module.cpp:155-181`) maps `Jsonb` and `Json` to
  `str`, for rows and for a column's `default` (`:212`); today an unlisted
  kind falls through to `bytes`.

### bc_jsonb_* calls by stage

Every call takes `bc_jsonb` handles, allocates through the arena
(charged), fails through the sticky error, and returns SQL NULL as
`false` with nothing written. The list is the proposal; names follow
`bc_plan.h`'s.

- **Stage 1:** `bc_col_jsonb`, `bc_row_set_jsonb`, `bc_param_jsonb`,
  `bc_jsonb_in`, `bc_jsonb_text`, `bc_emit_jsonb`.
- **Stage 3:** `bc_jsonb_field(cx, j, key, bc_jsonb *out)`,
  `bc_jsonb_element(cx, j, i64, out)`, their `_text` forms writing a
  `bc_view`, `bc_jsonb_path(cx, j, path, out)` and `_text` (a path is the
  form of *Path arguments*), `bc_jsonb_typeof`, `bc_jsonb_array_length`,
  `bc_jsonb_to_bool`, `_to_i64(cx, j, object_id, sql_kind)`, `_to_f64`,
  `_to_num`. A result that is a sub-value is a node sliced from its
  source, with the source's owner and no copy; `->>` of a string is a
  view of its stored bytes, checked UTF-8; `->>` of any other value
  writes its text.
- **Stage 2:** `bc_sort_key_jsonb(cx, key, j, desc)` and the group and
  hash key forms; `bc_sub_has_jsonb`; an aggregate slot that keeps the
  first node folded into it, `bc_agg_first_jsonb(cx, group, slot, j)`,
  read back with `bc_agg_jsonb(cx, group, slot) -> bc_jsonb`, charged its
  bytes once. `=`, `<` and the rest compare two sort keys with `memcmp`;
  a constant side's key is built once per execution. There is no separate
  comparison function, so order has one definition.
- **Stage 4:** `bc_jsonb_contains`, `bc_jsonb_exists`, `_exists_any`,
  `_exists_all`, `bc_jsonb_concat`, `bc_jsonb_delete_key`, `_delete_index`,
  `_delete_keys`, `_delete_path`, `bc_jsonb_set(cx, target, path, value,
  create)`, `bc_jsonb_set_lax`, `bc_jsonb_insert`, `bc_jsonb_strip_nulls`,
  `bc_jsonb_pretty`, and one builder for the variadic constructors and
  `to_jsonb`: `bc_jsonb_build_open(cx, kind)`, `bc_jsonb_build_add_*` per
  physical type (`i64` with its SQL kind and scale, `f64`, `bool`, bytes
  as a string, num, jsonb, null), `bc_jsonb_build_close -> bc_jsonb`. A
  modification writes a new node: the unchanged children are copied as
  byte ranges and only the tables on the changed path are rebuilt. Every
  modification and the builder check depth and the 2^28 - 1 size as they
  build (*Limits on a jsonb value*).
  - **`i64` with a scale.** A DECIMAL(p, s) is an `i64` scaled by s, a
    column property (`types_design.md` :31; `bc_number_text` takes it
    too), so the `i64` add takes the scale and builds digits with dscale
    s: `to_jsonb(1.5::numeric(10,2))` is `1.50`, and
    `jsonb_build_array(1.5::numeric(10,2), 2::numeric(5,0))` is `[1.50,
    2]`.
  - **num.** A finite NUMERIC text goes through `numeric_parse` into sign,
    weight, digits and dscale, keeping its scale (`to_jsonb(1.50::numeric)`
    is `1.50`, `to_jsonb('-0.0'::numeric)` is `0.0`). `NaN`, `Infinity`
    and `-Infinity`, which a number node cannot hold, become strings, as
    in PG 17: `to_jsonb('NaN'::numeric)` is `"NaN"`.
- **Stage 5:** aggregate slots of kind `jsonb_agg`, `json_agg`,
  `jsonb_object_agg`, `json_object_agg` (and `_strict`, `_unique`), each a
  growing builder charged per element, with `bc_agg_jsonb_add_*` per
  physical type, taking `i64` and num as the builder does, and checking
  depth and size per element.

### Memory charged for jsonb

- Parsing charges its node array, key index and encoded value as each
  grows (*Parsing a jsonb value*), each before it is written; nothing goes
  into an uncharged temporary (`bc_concat`'s pattern,
  `plan_api.cpp:5241-5252`, is not copied).
- A value `bc_row_set_param` parses stays charged while it sits in the
  row being built, until the row enters the write set, whose charge then
  takes it over; `CONTRACT.md` :64's "a row still being built" is
  uncharged only for what the row's own buffer holds.
- Output charges its text, whose length is computed before it is
  written (*Why binary and not canonical text*); a text past the budget
  is 54000 at `bc_jsonb_text` or the emit.
- `make_view` and `remake_view` store a length through `narrow<>`
  (today `static_cast<std::uint32_t>`, `plan_api.cpp:1439, 1455`), and
  every text a call builds is refused past 1 GiB - 1 with 54000 before it
  is allocated.
- An aggregate's builder is charged per element, as an aggregate slot is
  (`CONTRACT.md` :64): `jsonb_agg` over a million rows past the budget is
  54000, nothing committed.
- An allocation failure is `BC_E_MEMORY` (53200), never an exception into
  the plan.

### jsonb runtime errors and their SQLSTATEs

Today a runtime error's SQLSTATE comes from its code, and for
`BC_E_ARITH` from its exact message (`exec_sqlstate`,
`api/database.cppm:1775-1785` matches `division by zero` 22012,
`negative substring length not allowed` 22011 and `lower bound must be
less than or equal to upper bound` 22023). jsonb adds over twenty
messages across 22P02, 22P05, 22023, 22004, 22030, 2202E, 22003, 54000
and 54001; matching each by text would scatter the mapping further.
Approved (owner's decision 5), built as stage 0 before any jsonb:

- **A SQLSTATE in the error.** `bc_error_detail` carries the SQLSTATE,
  packed in 30 bits (five characters of six bits), for a new code
  `BC_E_DATA` and for `BC_E_ARITH`. `BC_E_ARITH` keeps its code, so the
  contract's "overflow, division by zero and out-of-range conversion as
  `BC_E_ARITH`" stays true; its three message-matched errors set their
  SQLSTATE, and a detail of 0 still means 22003. `exec_sqlstate` unpacks
  the detail and matches no message.
- **DETAIL, HINT and CONTEXT.** `bc_error_note` is one DETAIL line
  (`bc_plan.h:177`, `api/database.cppm:148`, `:1715`). Stage 0 adds
  `bc_error_hint` and `bc_error_context`, the matching `ExecError` fields,
  and the wire's separate `D`, `H` and `W` fields of ErrorResponse.
- Range errors of the cast targets keep `BC_C_RANGE` and
  `range_sqlstate`, so a cast to `integer` says what an integer assignment
  says.

## Binder rules for jsonb

### Type names, columns and the type classes

- `sql_type()` (`sql/binder.cppm:240-310`) maps `jsonb` and `json` (also
  `pg_catalog.`-qualified) to `SqlKind::Jsonb` and `SqlKind::Json`;
  `pg_type_name` and `sql_type_name` name them.
- **Two type classes**, `Jsonb` and `Json`, though PG 17 puts both in
  category `U`. `type_refusal` returns nothing when both sides share a
  class (`binder.cppm:479-486`), so one shared class would let `jsonb =
  json` and a `UNION` of the two through, mixing their texts in one
  column. With two classes, `type_refusal` gives 42804 for a jsonb
  assigned to a number column, 42883 for a jsonb compared with a number
  or with json, and 42846 for a `UNION` or `COALESCE` of json and jsonb,
  as PG 17.
- **json's refusals** (*The json type*) are the binder's, with PG 17's
  SQLSTATEs, wherever an equality or order is needed: comparison, `IN`,
  `ORDER BY`, `GROUP BY`, `DISTINCT`, set operations, `count(DISTINCT)`.
- **Uncompared types.** Stage 1 turns `refuse_numeric`'s 22 call sites
  (`binder.cppm:659-661`) into one `refuse_uncompared(kind)` naming
  NUMERIC and jsonb, so no site checks one and forgets the other. Stage 2
  lifts the jsonb comparisons; NUMERIC keeps its own.
- **Keys.** `refuse_key_type` (`catalog/catalog.cppm:1897-1910`, today
  Char and Numeric only) gains jsonb, with 0A000 (*Next phase of
  jsonb*). A json column is refused before any key is considered.
  `key_suffix` throws for both kinds, as for Num.
- **Other records that hold a type.** A domain's base type
  (`catalog.cppm:2155-2163`) and a procedure argument (`:2291-2296`) also
  store a `SqlKind`. `CREATE DOMAIN d AS jsonb`, `CREATE PROCEDURE p(j
  jsonb)` and a PL/pgSQL variable of jsonb or json are 0A000 in these
  stages, so only table records hold jsonb, and no record holds json.
- **Constants** are parsed at prepare: `'{"a":1}'::jsonb` with bad text
  is 22P02 before the statement runs, as `'abc'::int` is today.

### Operator resolution for jsonb, with unknown literals and parameters

The binder resolves each operator as PG 17's `oper_select_candidate` does
for its candidates, recorded here and checked by a case per row:

| Expression | Resolves to | PG 17 evidence |
|---|---|---|
| `j -> 'a'`, `j ->> 'a'` | (jsonb, text): text is the preferred type of the unknown's category | `'{"1":5}'::jsonb -> '1'` is `5`; `-> 1` is NULL |
| `j -> 1`, `j -> 1::smallint` | (jsonb, integer) | `-> 1::smallint` is `2` on `[1,2]` |
| `j -> 1::bigint`, `j -> 1.5` | 42883 `operator does not exist: jsonb -> bigint` (`jsonb -> numeric`) | verified |
| `j -> $1`, `j ->> $1` | `$1` text | `{text,text}` |
| `$1 -> 'a'`, `'{}' -> 'a'` | 42725 `operator is not unique: unknown -> unknown` | verified |
| `j = '…'`, `j @> '…'`, `j || '…'`, `'…' || j` | the literal is jsonb, read by `jsonb_in` (22P02 if not JSON) | `j || 'x'` and `'x' || j` are 22P02; `'[1]' || '{}'::jsonb` is `[1, {}]` |
| `j = $1`, `j @> $1`, `j || $1` | `$1` jsonb | `{jsonb,jsonb,jsonb}` |
| `j ? 'a'`, `j ? $1` | text | `{text}` |
| `j - 'a'`, `j - $1` | text (of text, int, text[]) | `{text}` |
| `j - '{a}'` | (jsonb, text): deletes the key `{a}` | `'{"a":1,"{a}":2}'::jsonb - '{a}'` is `{"a": 1}`; `'{"a":1,"b":2}'::jsonb - '{a}'` is unchanged |
| `j - '{a}'::text[]` | (jsonb, text[]): deletes the key `a` | `{"b": 2}` |
| `j - 1` | integer | `[1,2,3] - 1` is `[1, 3]` |
| `j #> '{a,b}'`, `#>>`, `#-`, `?|`, `?&` with an array literal or `$n` | text[] | `{text[],text[],text[],text[],text[]}` |
| `j = 'x'::text`, `j = 1`, `j + 1`, `j || 1` | 42883 (`operator does not exist: jsonb || integer`) | verified |
| `j || 'x'::text`, `'x'::text || j` | text concatenation, type text | `{"a": 1}x`, `x{}` |
| `INSERT INTO t (id, j) VALUES ($1, $2)` | `{integer, jsonb}` | verified |

**Precedence** is libpg_query's. `->`, `->>`, `#>`, `#>>`, `||`, `?`,
`@>` and the rest are "other" operators, left-associative, above
comparison, so `j->>'a' = 'x'` and `j->'a'->>1` need no binder rule. jsonb
`-` is binary minus and binds tighter than all of them: `j -> 'a' - 'b'`
parses as `j -> ('a' - 'b')`, 42725 `operator is not unique: unknown -
unknown`, and `j - 'a' -> 'b'` as `(j - 'a') -> 'b'`. `j->>'a'::int` is
`j->>('a'::int)`: 22P02 at prepare. A case records each.

The parser admits these operators in value expressions
(`sql/parser.cppm:290-336`, today 42601 "only +, -, *, / and % are
supported here") and, for those of boolean result (`@>`, `<@`, `?`, `?|`,
`?&`), as predicates in `WHERE` (`compare_op`, `:489-496`). `@?` and `@@`
are 0A000 (jsonpath).

### Path arguments

`#>`, `#>>`, `#-`, `?|`, `?&`, `- text[]`, `jsonb_set` and
`jsonb_insert` take a `text[]`; arrays are not a layer type
(`types_design.md` :48). Approved (owner's decision 2): a **path**, a
`text[]` accepted only in these argument positions, from an array literal,
`ARRAY['a', $1]` of text constants and parameters, or a parameter (OID
1009 over the wire, the same text form). It is never a column, a result
or a stored value. Its form for the plan is a u32 count, then each
element's length (or a NULL mark) and bytes.

An array literal is read as PG 17's `array_in` reads a text array:

- Unquoted elements are trimmed (`'{ a b }'` is the key `a b`); a quoted
  `"NULL"` is a string, an unquoted `NULL` a null element.
- Dimension bounds are accepted: `'[1:2]={a,b}'` is the path `a, b`.
- `'a'` is 22P02 `malformed array literal: "a"`, DETAIL `Array value must
  start with "{" or dimension information.`; `'{a'` is the same message
  with DETAIL `Unexpected end of input.`; a ragged `'{{a,b},{c}}'` is
  DETAIL `Multidimensional arrays must have sub-arrays with matching
  dimensions.`

PG 17's rules per operator:

- **A 2-D array** is read flat by `#>`, `#>>`, `?|` and `?&`
  (`'{"a":{"b":1}}'::jsonb #> '{{a},{b}}'` is `1`), and is 2202E `wrong
  number of array subscripts` from `#-`, `jsonb_set`, `jsonb_insert` and
  `- text[]`.
- **A NULL element** is ignored by `?|`, `?&` and `- text[]`, gives SQL
  NULL from `#>` and `#>>`, and is 22004 `path element at position 2 is
  null` from `#-`, `jsonb_set` and `jsonb_insert`.

### Expressions in WHERE and ON

`Data->>'scope' = $1` needs an expression operand. That is general work,
not jsonb's, designed as stage P (`frontend_design.md`, *Expressions in
WHERE, ON and HAVING (stage P)*; `planner_design.md`, *Expression filters
(stage P)*). Its first stage, P1a, is built: either side of a comparison,
`IS NULL`, `LIKE`, `IN` and `BETWEEN` may be any value expression the
select list takes, evaluated per row as a filter, never an index key. Stage
3's `->>` in a `WHERE` needs nothing more than its select-list kinds.

### Wire OIDs and formats for jsonb and json

- `type_info` (`server/pg_text.cppm:45-70`) maps `Jsonb` to `{3802, -1,
  -1}` and `Json` to `{114, -1, -1}`; `server_design.md`'s OID table gains
  both. `\gdesc` and psycopg see jsonb and json.
- **Text format.** A jsonb column returns its text; json its stored text.
  A parameter the layer types jsonb is read by `jsonb_in`, whatever OID
  the client sent (`server_design.md` :652-656). PG 17 differs: a text
  parameter of OID 25 or 1043 into a jsonb column is 42804 `column "j" is
  of type jsonb but expression is of type text` (`character varying`),
  OID 114 is taken as an assignment cast, and `j = $1` with OID 114 is
  42883 `operator does not exist: jsonb = json`. The layer's leniency is
  listed in *Differences*.
- **Binary format** is refused for every type today (0A000,
  `server/pg_session.cppm:1993-1996`, `:2017-2020`). Stage 1b accepts it
  for parameters only (owner's decision 3). lib/pq with
  `binary_parameters=yes`, Mattermost's DSN, sends every `[]byte`
  argument in binary whatever its target type and asks for text results,
  so the stage covers the types such an argument meets: jsonb, json,
  text, varchar and bytea, whose binary forms are their text or bytes
  (jsonb's behind a version byte). Binary results stay 0A000. The wire's
  binary jsonb is PG's, version 1 and the text; the stored layout never
  reaches the wire, and its format byte (0xF8) is not the wire's.
- **A jsonb parameter in binary** is `0x01` then its text. The client's
  OID picks the form, since the bytes mean different things: OID 114 is
  json's form, the text with no version byte, which PG 17 accepts into a
  jsonb column; any other OID is jsonb's. A first byte other than 1 is
  XX000 `unsupported jsonb version number N`, `\x01` alone 22P02 with
  DETAIL `The input string ended unexpectedly.`, and an empty value 08P01
  `insufficient data left in message`, as PG 17.

## jsonb aggregates

Stage 5 builds `jsonb_agg`, `json_agg`, `jsonb_object_agg`,
`json_object_agg` and the `_strict` and `_unique` object forms, with two
general clauses the layer refuses today (`sql/parser.cppm:373`): `FILTER
(WHERE …)` on any aggregate and `ORDER BY` inside an aggregate's call.

- `jsonb_agg(x)` converts each value by `to_jsonb`'s rules, makes SQL NULL
  a JSON null, and returns SQL NULL over no rows (so Mattermost writes
  `COALESCE(json_agg(x) FILTER (WHERE x IS NOT NULL), '[]')`).
- `json_agg` returns json, its elements separated by `, `
  (`["three", null, "one"]`); `json_agg` of a whole row is refused, since
  rows are not values here.
- `jsonb_object_agg(k, v)`: last duplicate key wins; a NULL key is 22023
  `field name must not be null`; `_strict` skips NULL values; `_unique`
  is 22030 `duplicate JSON object key value`, with no key in the message.
  `jsonb_object_agg_unique_strict` over `('a',1), ('a',NULL)` is still
  22030: the duplicate check comes before the NULL value is skipped.
- `json_object_agg(k, v)` writes `{ "k" : v, … }`, PG 17's json spacing,
  and keeps duplicate keys (`{ "a" : 1, "a" : 2 }`); a NULL key is 22004
  `null value not allowed for object key`; `json_object_agg_unique` is
  22030 `duplicate JSON object key value: "a"`, with the key.
- `json_build_object('a',1,'b',null)` is `{"a" : 1, "b" : null}`, with no
  space inside the braces, unlike `json_object_agg`; `json_build_array`
  is `[1, "x", null]` (stage 4).
- Without `ORDER BY` inside the call the element order is the order rows
  reach the aggregate, unspecified in PG 17 too.
- Memory: the builder is the aggregate's slot, charged per element and
  bounded by the budget (54000). A `GROUP BY` with many groups holds one
  builder per group, each charged.
- `DISTINCT` inside these calls stays 0A000; PG 17 builds it
  (`json_agg(distinct x)` is `[1]`), so it is a listed difference.

## jsonb set-returning functions

`jsonb_array_elements`, `jsonb_array_elements_text`, `jsonb_each`,
`jsonb_each_text`, `jsonb_object_keys` and `jsonb_to_recordset` produce
rows from a value, usually of the outer row (`FROM t,
jsonb_array_elements(t.j) e`, `CROSS JOIN LATERAL`, `LEFT JOIN LATERAL …
ON true`, `WITH ORDINALITY`, and in the select list).

The derived relations of #839 and #840 cannot host them: a derived
relation is read only as the plan's first, outer loop and never on a
NULL-extended side (`planner_design.md` :1033-1110), while a function of
the outer row is an inner loop per outer row, and `LEFT JOIN LATERAL`
NULL-extends it. Stage 6 is therefore a new physical leaf, an inner loop
over an iterator on one jsonb value (`bc_jsonb_iter_open(cx, j, kind)`,
`_next`), whose storage is the loop's and released when it steps, as
every loop's is (`CONTRACT.md` :65). It needs its own design addendum,
approved before code. Until then every function in `FROM`, `LATERAL` and
`WITH ORDINALITY` stays refused, with 0A000 naming the function rather
than today's 42601 "SELECT reads tables" once jsonb exists.

PG 17's rules the stage will match: `[]` and SQL NULL give no rows; an
object or a scalar is 22023 `cannot extract elements from an object` /
`from a scalar`; `jsonb_each` on a non-object is 22023 `cannot call
jsonb_each on a non-object` (`jsonb_each_text` likewise);
`jsonb_object_keys` on an array or a scalar is 22023 `cannot call
jsonb_object_keys on an array` (`on a scalar`); keys come in storage
order; `jsonb_to_recordset` needs a column list (42601) and converts by
the target type's input rules.

## Indexes on jsonb

- **B-tree on jsonb** (`CREATE INDEX ON t (j)`, `UNIQUE (j)`, a primary
  key): 0A000 in these stages; PG 17 accepts them. The next phase
  (*jsonb as a key, unique or index column*).
- **GIN** (`USING GIN (j)`, `(j jsonb_path_ops)`): 0A000, as the parser
  refuses every access method but btree (`sql/parser.cppm:917-923`). The
  next phase (*GIN indexes on jsonb*).
- **Expression indexes** (`((j->>'a'))`): the layer has none
  (`sql/parser.cppm:928`); refused as today. When they arrive, `->>` is
  immutable and qualifies.

## Catalog changes for jsonb

- `SqlKind::Jsonb = 19` (stage 1) and `SqlKind::Json = 20` (stage 1c),
  appended (`codec/row_codec.cppm:64-101`); `phys_type`, `fixed_width`
  (0), `sql_type_valid` and every exhaustive switch gain each case.
- **An older build** reads an unknown kind as damage: "catalog: column …
  has an invalid type" (`catalog/catalog.cppm:2346`), the path TIMETZ and
  NAME took (owner's decision 8). Table records stay in format 8
  (`serialize_table`, `:1958`).
- **`DEFAULT '{}'::jsonb`** and `ADD COLUMN … jsonb NOT NULL DEFAULT '[]'`
  are constant defaults the layer already supports: the default is stored
  in the column entry as its canonical text and parsed into its binary
  node on catalog load (text that does not parse, or is not canonical, is
  a refused catalog, as any bad default is). A row written before the
  `ADD COLUMN` reads the default.
- A view, a `CREATE TABLE AS` and `INSERT … SELECT` carry jsonb columns
  as any variable column. A `CREATE TABLE AS` whose select list has a
  json result is 0A000, as a json column is.
- `SqlKind::Json` is never written to a record (a json column, domain or
  procedure argument is refused), so an older build never meets it.

## Differences from PostgreSQL 17 for jsonb

These remain after every stage, each a known difference in
`CONTRACT.md` or the README:

- **Collation.** Strings and keys compare bytewise, which is PG 17's
  order only in a C or C.UTF-8 database. Under ICU `en`, PG 17 has
  `'"a"' < '"B"'`; the layer has the opposite.
- **Depth.** Fixed at 10000 (54001 past it, without the
  `max_stack_depth` HINT). PG 17 accepts up to its stack, about 14.5k
  nested objects and 16.4k nested arrays at 2MB.
- **Stored size.** A row is bounded by the engine's `max_value_bytes` (4
  MiB by default); PG 17 stores up to 2^28 - 1 bytes per container
  through TOAST. Both store numbers by their significant digits.
- **Output past the budget.** A value whose text passes the statement's
  budget is 54000, a stored value included, since its text is built on
  output (a 4 MiB value of numbers like `1e131071` prints gigabytes); PG
  17 writes it, up to 1 GB.
- **Stored layout.** The binary layout is the layer's own, not PG 17's
  JEntry format; only the wire's binary form (version 1 and the text) is
  PG 17's.
- **Group representative.** The first row's value (for `INTERSECT`, the
  right arm's first); PG 17's depends on the plan. Both print an input
  value.
- **Parameter OIDs in text format.** A text-format parameter the layer
  types jsonb is read as jsonb whatever OID the client gave; PG 17 refuses
  OIDs 25 and 1043 with 42804.
- **`DISTINCT` inside the jsonb and json aggregates** is 0A000.
- **Not built:** jsonpath, SQL/JSON, subscripting, operators on `json`,
  a `json` column, `json_build_array` and `json_build_object` (until stage
  1c), `jsonb[]`, record functions, jsonb as a key, unique or index column,
  expression and GIN indexes, `GREATEST` and `LEAST`, domains and
  procedure arguments of jsonb, `to_jsonb` and the build functions of a
  `CHAR(n)` value, named arguments and `VARIADIC` of the jsonb functions,
  binary results, binary parameters of other types.

## Guarantees and CONTRACT rows jsonb adds

`bytecaskdb-sql-layer/CONTRACT.md`. Each is named in its stage's PR. Rows
name only words of `bc_plan.h` and `api/database.cppm`.

- **Widened** (stage 0, :62's error rows and the wire row :214): a runtime
  error carries PG 17's SQLSTATE, DETAIL, HINT and CONTEXT, over the wire
  as separate fields. `BC_E_ARITH` keeps its code for the same failures.
- **Added** (stage 1): a jsonb row modelled on NUMERIC's (:116). A jsonb
  column holds any JSON value PG 17's `jsonb_in` accepts, read with its
  grammar, normalisation and errors (22P02 with DETAIL and CONTEXT, 22P05
  for `\u0000`, 22003 past numeric's range, 54001 past depth 10000, 22021
  for bytes that are not UTF-8); returns `jsonb_out`'s text byte for
  byte, as type 3802, across a reopen; casts as PG 17 (42846, 42804,
  22001); a damaged stored value is 58030, never undefined behaviour.
  Refused (0A000) until stage 2: comparisons, `ORDER BY`, `GROUP BY`,
  `DISTINCT`, keys.
- **Added** (stage 1c): a json row: a json value keeps the input's text
  verbatim after PG 17's validation, as type 114; PG 17's 42883 and 42846
  refusals of its comparisons; a json column is 0A000.
- **Unchanged** (:164): an earlier build already refuses a database this
  build writes; a jsonb column only changes its message (decision 8).
- **Widened** (stage 1b, :214): values travel in text format, and
  parameters of jsonb, json, text, varchar and bytea also in binary.
- **Widened** (stage 2): jsonb compares, orders, groups, is `DISTINCT`, as
  PG 17's `jsonb_ops` in a C database; a group prints one of its values.
- **Added** (stage 3, 4): a row per operator family (accessors,
  containment and existence, modification, constructors), each with its
  NULL rule and SQLSTATEs.
- **Added** (stage 5): the jsonb aggregates, `FILTER` and `ORDER BY` in an
  aggregate's call.
- **Widened** (stage 1 onward, :64): the memory row's list gains a jsonb
  value parsed or built, charged until it enters the write set, and an
  aggregate's builder.
- **Unchanged:** the engine's `CONTRACT.md`. Nothing here touches the
  engine.

## Tests of jsonb by stage

Each stage adds:

- **Unit tests** (`tests/jsonb_test.cpp`): the parser against every row
  of *jsonb input grammar and its errors*, DETAIL, token extent and
  CONTEXT included (the 80-byte token and the `é` window among them);
  output of every case of *jsonb output text*; `numeric_parse` and
  `numeric_text` against `numeric_in` for every number in `numeric.yaml`;
  the encoder against hand-written layouts (each tag, empty containers,
  the number examples of *Binary layout of a jsonb value*); every decoding
  rule of *Decoding a stored jsonb value without overflow* broken in turn
  (a count past its bound, `n` = 2^32 - 1, a decreasing or overrunning
  end, trailing bytes, an empty element, a weight of `INT32_MIN` and
  `INT32_MAX`, a dscale of 16384, a digit `'0'` at either end, depth
  10001, the format byte 0x01 with `\x01\x01`, an empty container of
  extent 6, `key_end[n-1]` past the extent, a key end below its
  predecessor on a lookup, a 0x00 inside a string or key) giving
  `BC_E_CORRUPT`; the output length's first pass against the bytes the
  second writes for weights -1, -3, -16383, 0 and 131071, each with
  dscale 0 and 16383 where the layout allows; `numeric_parse` and the
  encoder on every row of *Number range*, `-0.0e-16382` and
  `9.99e131071` among them; array indexes `INT64_MIN`, `INT64_MAX`,
  `-n`, `-n - 1` and `n`; a bytes call on a forged `bc_jsonb` and a jsonb
  call on a forged `bc_view`, both `BC_E_HANDLE`;
  the 39-value order and its equalities through the sort key, and the
  sort key's length against its bound for every functional case; every
  `PhysType` predicate for every value of the enum.
- **Plan API tests:** each call, its NULL result, its SQLSTATE, DETAIL,
  HINT and CONTEXT, `BC_E_CORRUPT` on a damaged value, `BC_E_HANDLE` for
  a bytes call on a jsonb column, a slice that dies with its row's step,
  and its charge: a budget sweep from 0 bytes upward gives 54000 or the
  full result, never a partial one, and holds the same bytes on every run.
  Stages 4 and 5 add, with the budget at 0: a `||`, `jsonb_set` and
  `jsonb_agg` whose result crosses 2^28 - 1 is 54000, and
  `jsonb_build_array` and `jsonb_set` over a stored depth-10000 value are
  54001, not 58030.
- **Functional cases** recorded from PG 17:
  `tests/functional/cases/postgresql/jsonb.yaml` and `json.yaml`, with
  feature ids `pg:jsonb` and `pg:json` added to `features.yaml` (:2014-2080).
  Cases a stage does not build are `unimplemented: true` with a comment,
  as `numeric.yaml:370-380` does. Stage 1 lands the cases of stages 1 to
  5, so the report shows the distance; stage 6's land with its addendum.
  Among them: every resolution and precedence row, every path rule, the
  scalar, negative-index and empty-path accessors, the json and jsonb
  aggregate differences, the varchar assignment, and PG 17's function
  errors (22023 `cannot get array length of a scalar` and `of a
  non-array`, `cannot delete from scalar`, `cannot delete from object
  using integer index`, `argument 1: key must not be null`, `argument list
  must have even number of elements` with its HINT, `jsonb_set_lax`'s
  `null_value_treatment must be "delete_key", "return_target",
  "use_json_null", or "raise_exception"`, its default `use_json_null`, and
  its `raise_exception` mode's 22004 `JSON value must not be null` with
  DETAIL and HINT; `to_jsonb('NaN'::float8)` is `"NaN"` and `Infinity` is
  `"Infinity"`, both strings, as are `to_jsonb('NaN'::numeric)`,
  `'Infinity'` and `'-Infinity'`; `to_jsonb(1.50::numeric)` keeps `1.50`,
  `to_jsonb('-0.0'::numeric)` is `0.0`, `to_jsonb(1.5::numeric(10,2))` is
  `1.50` and `jsonb_build_array(1.5::numeric(10,2), 2::numeric(5,0))` is
  `[1.50, 2]`). Also the accessors on a scalar `->` returned
  (`('{"a":1}'::jsonb -> 'a') -> 0`), indexes at the `integer` bounds,
  and path elements that are not integers.
- **Proof machine:** `test_sql_machine` gains a jsonb column; its model
  keeps the canonical text, the edge form, and both targets, embedded and
  wire, must return it.
- **Wire:** RowDescription OIDs 3802 and 114, `\gdesc`, parameter types
  from Describe (`{integer,jsonb}`, `{text,text[]}`), and in stage 1b the
  binary forms, OID 114's form, and the XX000, 22P02 and 08P01 cases.
- **Fuzz:** a libFuzzer target over the decoder (any bytes as a stored
  value through the full check, output, sort key, lookup, path and
  containment: a result or `BC_E_CORRUPT` under ASan and UBSan, and any
  value the full check accepts gives output that parses back to the same
  bytes) and one over the parser (any text: a value or a
  22P02/22P05/22003/54001, the encoded value passing the full check, and
  its output parsing back to the same encoded bytes).
- **Allocation fault sweep:** every allocation of the parser, the
  encoder, the output writer, the walks and each builder fails in turn; each gives
  `BC_E_MEMORY` with nothing leaked and nothing committed. No stage adds
  an engine durability site, so `tests/CLAUDE.md`'s fault sweeps of syncs
  and renames do not apply.
- **Catalog:** a jsonb column and its default round-trip through a
  reopen; a damaged default refused at load; a domain or procedure
  argument of jsonb refused.
- **Python:** a jsonb and a json value, and a column's default, read as
  `str`.
- **Bench:** none owed; no stage claims a speed-up. Stage 3's PR records
  `sql_layer_bench`'s point lookup before and after, to show the binder
  changes cost nothing on queries without jsonb.

## Stages of jsonb

Each stage is one PR that ships alone, with its tests, its README update
(Built / Not built, the conformance table), its `types_design.md` and
`plan_api_design.md` edits, and its named contract changes. Sizes are
estimates of changed lines, code and tests.

- **Stage 0: SQLSTATE in the error** (about 500 lines). `BC_E_DATA`, the
  packed SQLSTATE in `bc_error_detail`, `bc_error_hint`,
  `bc_error_context`, the `ExecError` fields and the wire's `H` and `W`;
  `BC_E_ARITH`'s three message matches moved to it. Revision 20.
- **Stage 1: the jsonb type** (about 3,600 lines, up from 2,800 for
  text storage: the encoder, the bounds-checked decoder and its fuzz
  target, and the output writer). The parser, number scanner
  (`numeric_in` split), encoder, full check, decoder and output writer in
  `sql/jsonb.cppm` (a new module; the PL/pgSQL reader `sql/json.cppm` is
  left alone: it trusts its producer, has no depth bound and keeps keys in
  a `std::map`); `SqlKind` 19; binder type name, the `Jsonb` class,
  casts with text, literals, `$n` typed jsonb, `refuse_uncompared`,
  `refuse_key_type`, the domain and procedure refusals; `PhysType::Jsonb`
  and the predicate switches; `BC_T_JSONB`, `bc_jsonb` and the stage 1
  calls; runtime set paths; wire OID 3802; catalog default; Python `str`.
  Revision 21.

  **As built.** Everything above, with these choices the code made:
  - The number scanner is a module of its own, `sql/numeric.cppm`
    (`numeric_parse`, `numeric_in_range`, `numeric_text`), which
    `numeric_in` and the jsonb parser both call: the catalog loads defaults
    through the jsonb module, so the scanner sits below the catalog, where
    `sql/text.cppm` cannot.
  - Pass 3 writes no node twice and keeps no stack: each node's size field
    becomes its position once its parent has laid out its table, and the
    nodes are written in document order, a dropped duplicate's subtree
    skipped. Pass 2 de-escapes each key that holds a `\` once, into a
    charged pool, before any object is sorted.
  - One walk serves the full check, the output length and the output text
    (a counting sink and a writing sink), so every read of a whole value
    checks every rule, key order and UTF-8 included, and the two output
    passes cannot disagree.
  - A constant is folded to its canonical text by `jsonb_in`
    (`sql/text.cppm`), whose `TextError` now carries a DETAIL and a
    CONTEXT, and a statement error (`ast::SqlError`) a CONTEXT, sent as
    W.
  - The binder's `JsonbCast` expression (since #904 stage C1, the one
    `Cast` kind of every cast computed per row) is the run-time cast: a string to
    jsonb (`bc_jsonb_in`) and a jsonb to `text` or `varchar` of no length
    (`bc_jsonb_text`). Its other casts are refused as *Casts to and from
    jsonb and json* sorts them: to `boolean` and the numbers is stage 3
    (0A000), to `varchar(n)`, `char` and `name` 0A000, to anything else
    42846.
  - Not built in stage 1, 0A000: `||` of a jsonb column with text (of a
    constant it is text concatenation). A jsonb column's value assigned to
    a string column is its text, PostgreSQL's I/O assignment cast, in
    `SET` and `INSERT … SELECT` alike, and `UPDATE … SET` takes any
    expression, casts included (#904 stage S1).
  - Constants are refused as columns are: a comparison, `||` or `-` of
    jsonb constants is 0A000 after an unknown literal beside the jsonb is
    read by `jsonb_in` (its 22P02 first), and a pair PostgreSQL has no
    operator for is 42883 with its operand types (`unknown` for a
    literal).
  - The parser refuses jsonb's operators (`->`, `->>`, `#>`, `#>>`, `@>`,
    `<@`, `?`, `?|`, `?&`, `#-`, `@?`, `@@`) and the JSON functions
    (`jsonb_…`, `json_…`, `to_json[b]`, `row_to_json`, `array_to_json`)
    with 0A000 wherever they are written, before it reads the operands.
    Forms the parser takes for no type keep their 42601 with jsonb: `IN`,
    `BETWEEN`, `IS DISTINCT FROM` and `LIKE` in a select list, and
    `COLLATE` (PostgreSQL's is 42804, *collations are not supported by
    type jsonb*).
  - An assignment refused 42804 carries PostgreSQL's HINT (*You will need
    to rewrite or cast the expression.*) at every site: a constant, a
    parameter, a column, `INSERT … SELECT` and `DEFAULT`.
  - A `DEFAULT` takes a string cast to jsonb (`'{}'::jsonb`), the one cast
    the parser takes in a `DEFAULT`.
  - The allocation-failure sweep over the parser and the output writer
    asserts its reach: each charge names its buffer (`jsonb::Site`: the
    parser's nodes, key index and key pool, a walk's stack), and the test
    marks the buffer, or the encoded value or text, whose allocation each
    injected failure hit; every one must be hit.
  - A set operation's sorter carries a jsonb through its text, read back
    by `bc_jsonb_in`, the same value.
  - The fuzz targets are `fuzz_sql_jsonb_decode` and `fuzz_sql_jsonb_parse`
    (`tests/fuzz/`, built as the root's fuzz targets are); their
    properties (`tests/fuzz/jsonb_properties.h`) also run over seeded
    mutations in `sql_layer_tests`.
- **Stage 1b: binary-format parameters** (about 300 lines; owner's
  decision 3).
- **Stage 1c: the json type** (about 700 lines, down from 900: no column
  type, owner's decision 11). `SqlKind` 20, the `Json` class and its
  refusals, casts with jsonb and text, OID 114, the json column refusal.
  Needed before stage 4, whose `json_build_*` return json.
- **Stage 3: accessors and casts out** (about 1,600 lines, down from
  1,800: a lookup is a table read or a binary search over the decoder's
  checked reads, not a text scanner). Parser and binder operators, path
  arguments, `bc_jsonb_field` and the rest; `->>` in a `WHERE` needs stage
  P. Revision 22.
  **As built.** Everything above, revision 22, with these choices the code
  made:
  - The lookups are `sql/jsonb.cppm`'s `field`, `element` and `at_path`,
    each reading only the header and the end tables it needs, checked as
    the walk checks them (a count past its extent, an end out of order or
    past its area, a key end below its predecessor on the search); the
    found node is a slice. `text_form` gives `->>`'s three outcomes, a
    string's bytes checked UTF-8. A slice from a row shares the row's
    owner, so it dies with its step (`BC_E_HANDLE` after).
  - A path is `parse_path`, PostgreSQL 17's `array_in` (`ReadArrayDimensions`,
    `ReadArrayStr`, `ReadArrayToken`) for a `text[]`, its messages and
    DETAIL word for word, every dimension read flat; its form is the u32
    count, then each element's u32 length (`0xFFFFFFFF` for NULL) and
    bytes. A literal path is read by the binder and folded as that form
    into a hidden parameter; a `$n` path is typed text and described as
    `text[]` (`ParamInfo::path`, OID 1009), read at bind by `check_params`
    for its error (as PostgreSQL's Bind reads it, whether a row reaches it
    or not) and again by `bc_jsonb_path_in` in the plan; `ARRAY[…]` of
    values builds the path with `bc_jsonb_path_add`, one element at a time.
    `ARRAY[ARRAY[…]]` and a `::text[]` cast are not built (42601).
  - The calls are the design's, but each that can give NULL takes an `out`
    pointer and returns `false` for NULL. `bc_jsonb_to_i64(cx, j, int_type,
    object_id)` takes `bc_int_type`, which gains `BC_BIGINT`, rather than a
    SQL kind; `bc_jsonb_to_f64(cx, j, real)` reads the number's text with
    `std::from_chars`, which keeps a denormal and refuses what rounds to
    zero or infinity, as `float8in` and `float4in` do.
  - Constants fold in the binder through the same functions, so
    `'{"a":1}'::jsonb -> 'a'` is a constant, its error at prepare, as
    PostgreSQL's planner folds it. A cast to `numeric` of no precision is
    taken only from a jsonb (`jsonb_numeric`); to `numeric(p, s)` is 0A000.
  - The binder's logical form is one expression kind, `Jsonb`, with its
    function (`Field`, `Element`, `Path`, their `Text` forms, `Typeof`,
    `ArrayLength`, `PathIn`, `PathArray`); the casts out widen
    `JsonbCast` (now `Cast`, #904 stage C1). Both are strict but `PathArray`, whose NULL element is the
    path's.
  - `'a' - 'b'`, two string literals under an arithmetic operator, is
    PostgreSQL's 42725 (`operator is not unique: unknown - unknown`), so
    `j -> 'a' - 'b'` is too.
  - Over columns, the accessors go where the layer takes an expression
    (the select list, `WHERE` and `ON` as stage P1a's filters, `ORDER BY`,
    an aggregate's argument); `GROUP BY`, `DISTINCT` and `UPDATE … SET`
    of one are the layer's limits for any expression.
  - No benchmark: no speed-up is claimed. The binder's new branches run
    only for the accessors' operator and function names.
- **Stage 2: comparison and order** (about 1,300 lines). The sort key
  from a walk of the node, `bc_sub_has_jsonb`, the first-value aggregate
  slot and its own plan API test, binder lifts. Revision 23.
  **As built.** Everything above, plan API revision 26 (23 to 25 went to
  stages 3 and 4 and #904's casts), with these choices the code made:
  - The sort key is `sql/jsonb.cppm`'s `sort_key`, one iterative walk over
    `open_container` and `child_at` (the stage-4 readers) with a charged
    stack, writing into a buffer `sort_key_bound` (the node's length plus
    one) sized: a write past it is `Corrupt`, never a write outside it. It
    checks what it copies (a NUL in a string or key, a number's rules, the
    end tables, depth 10000), not key order or UTF-8, as a lookup does.
  - `bc_sort_key_jsonb(cx, key, j, dir, nulls)` adds the key as a sort
    key's part, as the other `bc_sort_key_*` calls do, its bound charged
    first and its bytes inverted under `DESC`; `bc_sort_key_col` of a jsonb
    column does the same from the row. A jsonb part never decodes: no
    `bc_sort_key_part_jsonb`. `key_codec`'s `sortable(PhysType)` is
    `keyable`'s types and jsonb: what a sort, group or set key may hold,
    while a table's or index's key still refuses it.
  - Comparison is one call, `bc_jsonb_cmp(cx, a, b)`, -1, 0 or 1 from
    `memcmp` of the two sort keys, built and released per call; codegen
    prints `bc_jsonb_cmp(...) op 0` wherever it prints a comparison (a
    filter, a value, `HAVING`, a join's `ON`). Order still has one
    definition, the sort key. Folded constants compare by the same keys
    (`jsonb_order` in the binder).
  - No first-value aggregate slot. A jsonb expression key of `GROUP BY` or
    `DISTINCT` is held as its first row's text in the slot an expression key
    already has (`bc_agg_min_bytes` on the group's first row) and read back
    by `bc_jsonb_in`: the same value, its scale kept. A jsonb column key is
    read from the group's held first row, as every non-integer column key
    is. `COUNT(DISTINCT j)` of a column keys it by its sort key, not its
    stored bytes (`1` and `1.0` differ there).
  - Set operations key each row's jsonb by its sort key and carry it in the
    payload as its text, as stage 1 did for `UNION ALL`; `UNION` prints the
    first row of each value, `INTERSECT` the right arm's (the layer's
    existing choice for every type), `EXCEPT` the left's.
  - Subqueries of jsonb: a `BC_SUB_SET` holds each row's sort key
    (`bc_sub_has_jsonb`), `BC_SUB_BOUNDS` keeps the extremes by their sort
    keys and hands the plan their text, and a scalar subquery its text; each
    read back by `bc_param_jsonb`. So `= ANY`, `IN (SELECT …)`, `> ALL`
    and the rest of `ANY` and `ALL`, and a scalar subquery of jsonb run.
  - A correlated subquery takes an outer jsonb column as its argument by
    `bc_sub_arg_jsonb`, which sets the parameter to the value's text, read
    back by `bc_param_jsonb` as a jsonb parameter the caller gave
    (`EXISTS (SELECT 1 FROM r WHERE r.j = m.j)`).
  - The binder's `uncompared` is NUMERIC's alone again; an assignment still
    takes a jsonb only from a jsonb or into a string type.
  - Not built, as for every type: `GREATEST`, `LEAST`, `COLLATE` (42601,
    PostgreSQL's is 42804), `IN`, `BETWEEN` and `IS DISTINCT FROM` in a
    select list (42601), `DISTINCT ON`.
  - Tests: `jsonb_test`'s sort key cases (PostgreSQL 17's order of 63
    values in 53 classes, prefix-freedom, the bound, damage, the stack's
    charge), `plan_api_jsonb_ops_test`'s `bc_jsonb_cmp` and sort cases
    (and the budget sweep), embedded API cases, a proof test drawing
    values and parameters against PostgreSQL 17 on both targets, the fuzz
    properties widened to the sort key, and functional cases recorded from
    PostgreSQL 17 (stage 2's marks of `jsonb.yaml` lifted, seven cases
    added). No benchmark: no speed-up is claimed.
- **Stage 4: containment, existence, modification, constructors** (about
  3,200 lines, up from 3,000: a modification re-encodes the tables on its
  path, and the builder writes binary). Containment walks both trees with
  no parse. Could split into 4a (operators) and 4b (functions) if the
  review prefers. Revision 24.
  **As built.** Everything above but `json_build_array` and
  `json_build_object`, which return `json`, a type stage 1c has not built
  (0A000 still, a follow-up of stage 1c). Plan API revision 25 (24 went to
  #904's casts). The choices the code made:
  - One builder, `sql/jsonb.cppm`'s `Builder`, takes PostgreSQL's
    `pushJsonbValue` tokens in document order (containers opened and
    closed, a key before each member, leaves that are whole nodes) and
    writes the result once: an object's members sorted into storage order,
    the last of equal keys kept, every size summed bottom-up and checked
    against 268435455 (54000) and every depth against 10000 (54001) before
    the result is allocated. A leaf is a byte range of its source, copied
    once, so only the containers on a modification's way are rebuilt. A
    value nested anew (an inserted value, a build argument, an object
    wrapped by `||`) has its depth measured by a full walk; the rest keep
    their source's depth.
  - `contains` is `JsonbDeepContains` run iteratively: a charged stack of
    open pairs of containers, each frame the right side's member or element
    it is matching and, for an array, the left side's candidate container
    it is trying, so the backtracking search keeps nothing on the C++
    stack. Numbers compare by sign, weight and digits, not scale.
  - #-, `jsonb_set`, `jsonb_set_lax` and `jsonb_insert` are `setPath` walked
    down once: the containers on the path are found first (each step's
    errors as PostgreSQL raises them, a NULL element 22004 and an array
    index that is no integer 22P02 only where the walk reaches them), and
    the result is built only when the last step changes something; a call
    that changes nothing returns its input, as do `-` of a missing key and
    `jsonb_strip_nulls` of a scalar. `jsonb_set_lax` reads its treatment's
    NULL first (22023), as PostgreSQL does, and its text only for a NULL
    value.
  - A path of two or more dimensions keeps that fact in its count's top
    bit, so `#>`, `?|` and `?&` read it flat and the functions that take
    it as a list raise 2202E at run time, as PostgreSQL does.
  - `jsonb_pretty` is the output walk with four-space indentation, the same
    counting and writing passes.
  - `to_jsonb` and the builders: `bc_jsonb_of_i64`, `_f64`, `_bool`,
    `_text` and `_num` make a scalar node from a value
    (`sql/text.cppm`'s `jsonb_scalar_text`: a number's output text read by
    the JSON grammar, a string for text that is no JSON number), and
    `bc_jsonb_build_array` and `_build_object` take the converted arguments
    as C arrays with their NULL marks (and, for an object's keys, whether
    one is a jsonb), at most 100, PostgreSQL's FUNC_MAX_ARGS, instead of
    the proposed open/add/close calls. A float key is its `float8out` text.
    `CHAR(n)` arguments are 0A000: no value holds its padded text.
  - The binder resolves every stage-4 operator and function in one place
    (`jsonb_signature`), for constants folded at prepare and for values,
    so their errors cannot drift apart. A `$n` an earlier use typed keeps
    its type (`jsonb_set($1, $2, $3), $1 ?| $2`); `x::text[]` of a string
    literal or `$n` is a path; `x::numeric` of a literal is a NUMERIC of
    no precision as an argument of `to_jsonb` and the builders.
  - `jsonb || text` and `text || jsonb` of columns is text concatenation
    of the jsonb's text, which stage 1 left at 0A000.
  - Tests: `jsonb_ops_test` (each operator and function against PostgreSQL
    17's results, the depth and size bounds, damage, budget and
    allocation sweeps asserting the buffers they fail),
    `plan_api_jsonb_ops_test`, embedded API cases, a proof test drawing
    values, keys, indexes and paths against PostgreSQL 17 on both targets,
    a wire test of Describe's types, the fuzz properties widened to the
    stage-4 walks, and functional cases recorded from PostgreSQL 17. The
    54000 bound is reached through a build and through `||` and
    `jsonb_set` of a 135 MB array, not `jsonb_agg` (stage 5). No benchmark:
    no speed-up is claimed.
- **Stage 5: aggregates, FILTER and ORDER BY in the call** (about 1,900
  lines, up from 1,800: each builder encodes as it grows). Revision 26.
- **Stage 6: set-returning functions** (design addendum first; about
  2,500 lines).

Order: 0, 1, then 3 (after stage P, designed and built on its own), 2,
1c, 4, 5, 6. Stages 2 and 3 are independent; 1b and 1c can go any time
after 1.

## Next phase of jsonb

Two features stay refused with 0A000 through stage 6, by the owner's
decision of 2026-11-21. Each needs its own design, approved before code,
and a follow-up issue tracks it.

### GIN indexes on jsonb

`CREATE INDEX … USING GIN (j)` and `(j jsonb_path_ops)` serve `@>`, `?`,
`?|` and `?&` in PG 17. Mattermost creates two (migrations 000228 and
000229).

- **What it would take.** The parser accepts `USING gin` and the two
  operator classes (today it refuses every access method but btree,
  `sql/parser.cppm:917-923`); the catalog records an index kind beside
  btree; an inverted index whose entries are a value's keys and scalars
  (`jsonb_ops`) or hashes of its paths to scalars (`jsonb_path_ops`),
  maintained in the same write batch as the row on every insert, update
  and delete; a build over the existing rows; the planner choosing it for
  those four operators, with a recheck of the row, since an entry match is
  only a candidate; and the transaction, recovery and fault-sweep tests
  every index has.
- **Why deferred.** A GIN index never changes a result in PG 17, only a
  plan, so refusing it costs speed, not correctness, and no stage here
  depends on it. Mattermost's two GIN indexes are created `CONCURRENTLY`,
  which the layer refuses for every index, so accepting GIN alone would
  not run those migrations. Recording an index with no entries meanwhile
  would persist catalog state for a builder not yet designed.

### jsonb as a key, unique or index column

A jsonb primary key, `UNIQUE (j)` or `CREATE INDEX ON t (j)`. PG 17
accepts them; `'1.0'` after `'1'` is 23505 `Key (j)=(1.0) already
exists.`

- **What it would take.** The key part is the jsonb sort key of stage 2,
  which already orders and compares as PG 17's `jsonb_ops`. It cannot be
  decoded (the scale is gone), so the row must hold the value, as
  `types_design.md` already plans for a collated text part, and a
  covering read of the column must go to the row: a planner change beyond
  jsonb. `refuse_key_type` and `key_suffix` lift for jsonb; a key whose
  sort key passes the engine's key size bound is refused at write; the
  23505 message prints the row's value, not the key.
- **Why deferred.** No caller found needs it: Mattermost and Django key
  their tables on other columns. It is the first key part that cannot be
  decoded, which changes how covering reads work for every type that
  later takes the same path, and that deserves its own review.

## Owner's decisions on jsonb

Answered by the owner on 2026-11-21. The design above follows them.

- **Q1. Storage format: binary.** A tree with a format byte, offset
  tables, sorted keys, de-escaped strings and numbers as sign, weight,
  digits and scale (*Binary layout of a jsonb value*). The recommendation
  had been canonical text; the owner chose binary because the type is
  called jsonb and PG 17 stores it binary. The measured cost, a walk on
  output, and how it is bounded are in *Why binary and not canonical
  text*.
- **Q2. text[] path arguments: as recommended.** A path accepted only as
  the `text[]` argument of jsonb operators and functions (array literal,
  `ARRAY[…]` of text, parameter OID 1009), not a general `text[]` type:
  Mattermost's `jsonb_set` and Django's `#>` need it, and psycopg2 sends a
  Python list as `ARRAY[...]`.
- **Q3. Binary wire format: as recommended.** Accepted for parameters of
  jsonb, json, text, varchar and bytea in stage 1b, because Mattermost's
  DSN sets `binary_parameters=yes`. Binary results stay refused.
- **Q4. GIN indexes: next phase.** Refused (0A000) in these stages;
  *Next phase of jsonb*.
- **Q5. Runtime errors carry their SQLSTATE: as recommended.** The
  SQLSTATE in `bc_error_detail` for `BC_E_DATA` and `BC_E_ARITH`, with
  DETAIL, HINT and CONTEXT as separate notes, built as stage 0 and
  replacing `exec_sqlstate`'s message matches.
- **Q6. jsonb as a key, unique or index column: next phase.** Refused
  (0A000) in these stages; *Next phase of jsonb*.
- **Q7. Validation: as recommended, adapted to binary.** The full check
  on write (`bc_row_set_jsonb`) and at catalog load; every read
  bounds-checked and overflow-safe, stopping with `BC_E_CORRUPT` at the
  first break it meets. The only producer of stored bytes is the layer's
  own encoder and the engine's CRC catches media damage first, so a full
  check per read would cost O(n) on every `-> 'k'` for nothing.
- **Q8. What an older build says: as recommended.** "invalid type", as
  TIMETZ and NAME. The catalog writes one record format for every table
  (`catalog.cppm:1958`); a per-table format for one type is not worth it.
- **Q9. Depth bound: as recommended.** 10000, which PG 17 accepts for
  arrays and objects at every call depth tested; PG 17's own limit varies
  with the stack and the call path.
- **Q10. Set-returning functions: as recommended.** Stage 6, with its own
  design addendum approved first; refused meanwhile. Mattermost's runtime
  queries use them.
- **Q11. json: as recommended.** A result type only (`json_build_*`,
  `json_agg`, casts); a json column is 0A000. Mattermost and Django have
  no json column. Rails' `t.json` is the likely caller, and adding the
  column later is small once the kind exists.

## Risks of jsonb

- **Ordering edge cases.** The top-level empty array, length-first
  objects in storage order and numeric equality are easy to get subtly
  wrong. Mitigation: the 39-value order and its equalities, and a
  generated corpus whose pairwise comparisons are recorded from PG 17,
  checked through the sort key, the one definition of order.
- **Amplification.** Numbers like `1e131071` and `\u00xx` escapes
  multiply bytes on output, and deep nesting multiplies stack on walks.
  Mitigation: a number is stored by its digits, so a write never expands
  it; output length computed, capped at 1 GiB - 1 and charged before it
  is written; every walk iterative, with its depth checked on read.
- **Decoder safety.** A binary format read from storage is where
  overflows and out-of-bounds reads hide. Mitigation: the rules of
  *Decoding a stored jsonb value without overflow*, a unit test per rule,
  the decoder fuzz target under ASan and UBSan, and the typed `bc_jsonb`
  handle with the 0xF8 format byte, so text never reaches the decoder as
  a value.
- **Output cost.** `SELECT j`, the most common use, walks the tree: about
  2 µs per 1.5 KB, about three rows of the layer's scan. Mitigation:
  measured in *Why binary and not canonical text*; PG 17 pays the same.
- **Write latency.** A write parses and encodes its value: linear in
  size, at most `max_value_bytes`. A 4 MiB value is a few milliseconds of
  parse, paid by that statement only.
- **Layout changes.** A stored layout is hard to change once rows exist.
  Mitigation: the format byte, so a later layout is read beside this one
  without a migration.
- **Binder breadth.** Unknown-literal resolution across a dozen operators
  can pick a wrong type silently (`j - '{a}'` is the trap). Mitigation: a
  case per row of the resolution table, and Describe's parameter types
  checked against PG 17's `pg_prepared_statements`.
- **Scope creep.** jsonb invites jsonpath, SQL/JSON and arrays. Each is
  refused here with 0A000 and waits for its own design.
- **Mattermost still will not run** after these stages without
  `CONCURRENTLY`, `LATERAL`, `ALTER COLUMN … TYPE` and more; the README
  must not suggest otherwise.

## Edits to other documents once jsonb is built

- `types_design.md`: :48 drops `JSON`; a *jsonb and json* section points
  here; *Key encoding* lists the jsonb sort key; *Row format* names the
  jsonb layout.
- `plan_api_design.md`: the status line per revision, `BC_T_JSONB`,
  `bc_jsonb`, `BC_E_DATA` and the SQLSTATE detail, the calls.
- `server_design.md`: OIDs 3802 and 114; binary-format parameters; the
  `H` and `W` error fields.
- `frontend_design.md` and `planner_design.md`: the operator kinds, the
  stage P addendum and, for stage 6, the new leaf.
- `README.md`: Built / Not built per stage.
