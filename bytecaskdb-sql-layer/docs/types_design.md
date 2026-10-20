# SQL types, value semantics and the shared codec

Status: design, decided (#490); the codec it specifies is implemented in
`bytecaskdb-sql-layer/codec/` with the property tests in
`bytecaskdb-sql-layer/tests/codec_test.cpp`. This document decides how SQL
values map onto the four physical types of the plan API
(`docs/plan_api_design.md`), what nulls, comparisons and arithmetic mean,
which functions the compiler may call, and the two on-disk formats the
sql-layer and the MariaDB plugin will share: the key encoding and the row
format. Nothing in the engine changes.

The plan API deals in `i64`, `f64`, `bool` and `bytes`. Everything SQL knows
beyond those four is a rule the compiler applies: a scale, an epoch, a range
check, a collation. Those rules live here.

## Stage-1 types

The binder accepts these types and rejects every other. A rejected type is a
bind error with the position of the type name.

| SQL type | Physical | Stored as |
|---|---|---|
| `SMALLINT`, `INTEGER`, `BIGINT` | `i64` | The value. Every width is widened: one integer type in storage, in keys and in arithmetic. The declared width is a catalog property, checked on assignment. |
| `DOUBLE PRECISION`, `REAL` | `f64` | IEEE 754 binary64. A `REAL` is stored as its `float` rounding, widened, as PostgreSQL stores a float4: 0.1 reads back as 0.100000001490116…, and a finite value past `FLT_MAX` that does not round to infinity as `FLT_MAX`. A `REAL` key or index part is keyed the same way, from a row or a parameter, so a lookup, a bound or an `UPDATE`/`DELETE` by key finds the row by the value it was written with, as a `$1` typed real does in PostgreSQL. A value is rounded in three places: the row writer, keyed by the column's declared type; the key encoder, keyed by the part; and the binder, which parses a `REAL` DEFAULT straight to `float`, rounded once as PostgreSQL's `float4in` rounds, so the catalog holds the value a row stores. A DEFAULT literal that overflows, or underflows to zero, is out of range; one that rounds to a denormal is kept. A `DOUBLE PRECISION` DEFAULT is parsed as a double the same way. |
| `BOOLEAN` | `bool` | 0 or 1. |
| `BYTEA` | `bytes` | The bytes. |
| `TEXT`, `VARCHAR(n)` | `bytes` | UTF-8. Compared as bytes (binary collation). `n` counts code points and is a catalog property, checked on assignment. |
| `DECIMAL(p, s)`, `NUMERIC(p, s)`, p ≤ 18 | `i64` | The value times 10^s, exactly. `p` and `s` are catalog properties of the column. `s` defaults to 0; `p` is required. |
| `TIMESTAMP` | `i64` | Microseconds since 1970-01-01 00:00:00, no time zone; from 4714-11-24 BC, PostgreSQL's first. |
| `DATE` | `i64` | Days since 1970-01-01, in PostgreSQL's range: 4714-11-24 BC to 5874897-12-31. |
| `TIME` | `i64` | Microseconds since midnight, in [0, 86 400 000 000]: 24:00:00 included, as in PostgreSQL. |

Rejected in stage 1, each for a reason that is not "later": `NUMERIC` with
no precision or with p > 18 (does not fit `i64`); `CHAR(n)` (its
blank-padded comparison is a collation rule, and stage 1 has none);
`TIMESTAMP WITH TIME ZONE` (a request carries no time zone yet, #491);
`INTERVAL`, `UUID`, `JSON`, arrays, enums, composite types, `BIT`, `MONEY`,
`SERIAL` (sequences are a runtime service, deferred in the plan API).

Widening is why the plan language has one integer type: a column declared
`INTEGER` is an `i64` in the row and in every `bc_*` call, and `2147483647 + 1`
is not an overflow but an assignment that fails its range check.

**Declared widths and lengths are checked on assignment, not on read.**
The row builder checks every value a statement writes (`bc_row_set_*`,
`bc_row_set_param`) against its column's declared type and refuses one that
does not fit with `BC_E_CONSTRAINT`, `BC_C_RANGE` and the table's id, the
message naming the column: outside [−2^15, 2^15) for `SMALLINT` or
[−2^31, 2^31) for `INTEGER`, a `REAL` that overflows or underflows `float`,
`TEXT` or `VARCHAR` that is not well-formed UTF-8 or holds a NUL, more
than `n` code points for `VARCHAR(n)` (counted in the same pass), more than `p − s` integer digits
for `DECIMAL(p, s)`, a `DATE`, `TIMESTAMP` or `TIME` outside PostgreSQL's
range for it (the table above). SQL reports it as SQLSTATE 22003, 22001 for
a string too long, or 22008 for a date or time. `REAL` overflow is decided
as PostgreSQL decides it: by whether the value rounds to an infinite
`float`. A `DEFAULT` is checked the same way
at `CREATE TABLE` and `ADD COLUMN`. Reads trust the row.

**A compared parameter is typed by its column, as PostgreSQL types a
`$n`.** A parameter a `WHERE` compares with a column takes that column's
type without its modifier: `VARCHAR(n)` as `TEXT`, `DECIMAL(p, s)`
unbounded. It is checked against that type before the plan runs, so a
value outside it is `BC_C_RANGE` whether or not the plan would read it (a
key whose other part is NULL): 
`3000000000` for an `INTEGER` key or bound, `1e39` or `1e-50` for a `REAL`,
text that is not UTF-8. It is never wrapped, rounded to a neighbour, or
taken to find nothing. A modifier is checked only on a write: a `VARCHAR(2)`
key compared with `'abc'` finds nothing, as in PostgreSQL. A parameter only
assigned (`INSERT`, `SET`) is checked by the row builder, in column order,
as PostgreSQL's assignment checks the values a driver sends typed. The plan API
checks a key value the same way where it is built (`bc_key_*`, against the
column its key part is), for plans not compiled from SQL. `NOT NULL` is checked when the row ends, after these, so a row
holding both reports the range, as PostgreSQL does. A `VARCHAR(n)` value
whose excess is only spaces is refused, where PostgreSQL truncates it.

**Text is valid UTF-8.** The row builder refuses a `TEXT` or `VARCHAR` value
that is not well-formed UTF-8, or holds a NUL, when it is written
(`BC_C_RANGE`, 22021), as it refuses any value outside its type; the
compiler never emits a validity check. A key value that is not UTF-8 is
refused the same way. Comparison and ordering are bytewise,
which is code-point order for well-formed UTF-8. Collation sort keys come
later, in the slot the MariaDB plugin uses them: the key encoder writes a
text key part as the collation's weights, as `bytes`, and the row keeps the
original text (*Key encoding*, below). Nothing else changes when they arrive.

**Decimal.** `DECIMAL(p, s)` with p ≤ 18 fits `i64` with room to spare: the
largest 18-digit value is below 2^60. The scale is the column's, fixed in the
catalog, so a column holds every value as the same multiple of 10^−s and
`memcmp` order on the key encoding is numeric order. Assigning a value with
more fractional digits than `s` rounds half away from zero, as PostgreSQL's
cast to `numeric(p, s)` does.

**Time.** Microseconds in `i64` reach ±292 000 years; days in `i64` reach
further. `TIMESTAMP` and `DATE` values are instants and days with no time
zone; conversions between them are integer arithmetic on the day length
(`DATE` from `TIMESTAMP` is the floor division, which the compiler emits
with the adjustment for negative values that `bc_div` truncation needs).

## Null

SQL's three-valued logic, as PostgreSQL has it:

- A comparison with a null operand is *unknown*. `WHERE` keeps a row only
  when its predicate is *true*; `unknown` is dropped like `false`. `NOT
  unknown` is `unknown`; `AND` and `OR` are Kleene's. `IS NULL`, `IS NOT
  NULL`, `IS DISTINCT FROM` are the only operators that yield a boolean
  from a null.
- Arithmetic and functions with a null operand yield null, except
  `COALESCE`, `NULLIF`, `IS [NOT] NULL` and the aggregates.
- **In keys nulls sort first.** The marker byte `0x00` precedes every
  present value's `0x01`. Under `DESC` the whole part is inverted and nulls
  sort last. So an `ORDER BY` on an index gives `NULLS FIRST` for `ASC` and
  `NULLS LAST` for `DESC`, as MariaDB and SQLite do; PostgreSQL's default is
  the reverse. A query that asks for the other placement is not served from
  the index in stage 1.
- **A null parameter in a key lookup matches nothing**: `WHERE id = $1`
  with `$1` null returns no rows and `UPDATE`/`DELETE` affect none. The
  compiler emits the null test before building the key, as the prototype's
  plans do.
- **A range never includes nulls.** `NULL < x` is unknown, so a scan of a
  nullable key column with no lower bound starts at the successor of the
  null part, past the null entries that sort first; a lower bound excludes
  them already through its present marker. (Under `DESC` the nulls sort
  last and the upper end is the one bounded the same way.)
- **Aggregates** skip nulls: `SUM`, `MIN`, `MAX`, `AVG` and `COUNT(x)`
  ignore null inputs; over no rows `COUNT` is 0 and the others are null.
  `COUNT(*)` counts rows.
- **`DISTINCT` and `GROUP BY` treat nulls as equal**: all null values form
  one group, which the key encoding gives for free (one marker byte, one
  group key).
- **Unique indexes treat nulls as distinct**, PostgreSQL's default: two rows
  with a null in a unique column do not collide. The compiler skips the
  uniqueness check when any indexed column of the row is null. Secondary
  index keys always carry the primary key after the index columns, so null
  entries coexist.
- A `NOT NULL` column assigned null is `bc_raise(BC_C_NOT_NULL, column_id)`;
  the compiler emits the test on the parameter, and `bc_row_end` refuses the
  row (`BC_E_LAYOUT`) if the compiler did not.

## Comparison and arithmetic

**Operands of one type compare as that type.** Integers, decimals of one
scale, timestamps, dates, times and booleans compare as `i64`; doubles as
`f64`; text and binary as bytes.

**Mixed operands convert toward the wider type**, once, before the operator:

| Left | Right | Both become |
|---|---|---|
| integer | decimal(p, s) | decimal(·, s): the integer is scaled by 10^s (`bc_mul`, checked) |
| decimal(p1, s1) | decimal(p2, s2) | decimal(·, max(s1, s2)): the smaller scale is raised (`bc_mul`, checked) |
| integer or decimal | double | double (`bc_i64_to_f64`, then a division by 10^s for a decimal; exact only below 2^53) |
| boolean | integer | integer, 0 or 1 |
| date | timestamp | timestamp: days × 86 400 000 000 |

Anything else is a type error at bind time: there is no implicit conversion
between text and a number, or between a time and a number.

**Integer arithmetic is checked.** The subset has no raw integer operators;
the compiler emits the helpers, which set `BC_E_ARITH` and return zero on:

| Helper | Raises on |
|---|---|
| `bc_add`, `bc_sub`, `bc_mul` | the mathematical result outside `i64` |
| `bc_div` | a zero divisor; `INT64_MIN / −1`. Truncates toward zero, as C and PostgreSQL do. |
| `bc_mod` | a zero divisor. The result takes the dividend's sign; `INT64_MIN % −1` is 0. |
| `bc_neg` | `INT64_MIN` |
| `bc_f64_to_i64` | NaN, or a value outside `i64` after truncation toward zero |

`BC_E_ARITH` ends the plan and is reported to the client as a numeric
overflow or a division by zero; the compiler does not retry it.

**Decimal arithmetic** is integer arithmetic on the scaled values with a
result scale fixed by the operator: `+` and `−` keep the common scale; `×`
has scale s1 + s2; `÷` has the common scale, computed as (a × 10^s) ÷ b and
rounded half away from zero, which the compiler emits with `bc_mul`,
`bc_div` and `bc_mod`. Any intermediate outside `i64` is `BC_E_ARITH`:
a product of two 18-digit values is one, and stage 1 says so rather than
widening. Assignment to a column rounds to the column's scale and
range-checks the integer digits (*Stage-1 types*).

**Double arithmetic is plain IEEE 754.** `+ − × ÷` are the C operators, the
plan language allows them, and every input has a defined result: `1.0 / 0.0`
is infinity and `0.0 / 0.0` is NaN. PostgreSQL raises on a double division
by zero and stage 1 does not; it is the one place where the IEEE result is
kept, because the subset has no way to raise a numeric error from a double
expression and a branch per division is not worth the case. Comparison
follows PostgreSQL: NaN equals NaN and is greater than every other value,
including infinity, so sorting, `MAX` and the key encoding agree. `−0.0`
equals `0.0`.

**Text and binary compare bytewise** (`bc_bytes_cmp`), the memcmp order,
which the key encoding reproduces. Equality is `bc_bytes_eq`; `LIKE 'abc%'`
on a key column becomes a prefix scan (`bc_bytes_prefix` on a row column).

**Casts.** `CAST` exists between: integer and double (`bc_i64_to_f64` is
always defined; `bc_f64_to_i64` truncates and raises as above); integer and
decimal (scaling, checked; decimal to integer truncates toward zero);
decimal and double; boolean and integer (0 and 1; any non-zero integer is
true); timestamp and date (floor and multiplication); text and binary
(identity, with the UTF-8 check on the way to text, done by the runtime).
Casts to and from text (`'42'::INTEGER`, `now()::TEXT`) are not in stage 1
except for literals, which the binder folds at compile time.

## The function library

The compiler may emit these in stage 1. Each is either an operator the
language has, or a helper declared in `bc_plan.h`, or a value the runtime
binds as a hidden parameter. Helpers not yet declared are added to the
header, with tests, by the compiler PR that first emits them (#492); this
document fixes the set, not the signatures.

| Function | How |
|---|---|
| `COALESCE`, `NULLIF`, `CASE`, `IS [NOT] NULL`, `IS DISTINCT FROM` | `if`/`else` in the plan |
| `ABS`, `GREATEST`, `LEAST` on numbers | comparisons and `bc_neg` |
| `COUNT`, `SUM`, `MIN`, `MAX`, `AVG` | locals when the scan's order groups the keys, `bc_agg_*` otherwise; `SUM` of integers is checked |
| `OCTET_LENGTH` on bytes and text | a helper |
| `LENGTH` (code points), `SUBSTRING` (by code point), `||`, `CONCAT` on text | helpers that walk UTF-8 in the API, not in the plan: the subset has no counted loop |
| `SUBSTRING` by byte, `||` on `BYTEA` | the same helpers on bytes |
| `CURRENT_TIMESTAMP`, `NOW()`, `CURRENT_DATE` | a hidden parameter the runtime binds at `bc_ctx_begin`: a plan cannot see the clock, so every call in one statement sees one instant |
| `EXTRACT(field FROM ts)` for `YEAR`…`SECOND`, `DATE_TRUNC` to a day or coarser | integer arithmetic on the microsecond value and a civil-date helper |
| `CAST` | *Casts*, above |

Excluded in stage 1: `UPPER`, `LOWER`, `TRIM`, `POSITION`, `LIKE` beyond a
prefix, regular expressions (all need character semantics or a collation);
`ROUND`, `TRUNC`, `FLOOR`, `CEIL` on doubles (arithmetic on doubles stays
plain); `RANDOM`, sequences and `gen_random_uuid` (a plan is pure); text
formatting and parsing functions; window functions; `ARRAY_AGG` and
`STRING_AGG`. Each is an addition to the library, none changes a type.

## Key encoding

Every key is `namespace(1) id(4, big-endian) parts…`. Parts are encoded so
that `memcmp` order on the whole key is the typed order of its parts, and so
that the key parses back into its parts without a length prefix. The
namespaces are `0x01` catalog, `0x02` table rows, `0x03` index entries.

| Part | Encoding |
|---|---|
| nullable marker | One byte before a nullable part: `0x00` null, `0x01` present. A null part is the marker alone. A non-nullable part has no marker. |
| `i64` | 8 bytes big-endian with the sign bit flipped. |
| `f64` | The IEEE 754 bits, big-endian; a positive value (sign clear) has its sign bit flipped, a negative value every bit flipped. Before encoding `−0.0` becomes `0.0` and every NaN becomes the one quiet NaN `0x7FF8000000000000`, so equal values make equal bytes and NaN sorts above `+∞`, as *Comparison* orders it. Decoding returns those canonical values. |
| `bool` | One byte, `0x00` false, `0x01` true. |
| `bytes` | Every `0x00` byte becomes `0x00 0xFF`; the part ends with `0x00 0x00`. Self-delimiting: a decoder reads to the first `0x00` not followed by `0xFF`. A string sorts before every string it is a prefix of, and the two-byte terminator keeps that true whatever the next part's first byte is: `0x00 0x00` is below `0x00 0xFF`, which is the smallest thing a longer string can continue with. |
| `DESC` part | The part's bytes as above, marker included, with every bit inverted. Decoding inverts byte by byte before interpreting, so the part stays self-delimiting. Nulls sort last. |

Integer SQL types of every width, decimals, timestamps, dates and times are
`i64` parts; the order they need is the integer order of their stored value,
which the sign flip gives. Text is a `bytes` part in stage 1; when collations
arrive, a text part under a non-binary collation is the collation's sort key
(its weights) written as `bytes`, the row holding the original text. This is
the slot the plugin's `key_format` 2 uses for `CHARSET_INFO::strnxfrm`
weights, and the decoder for such a part returns the weights, not the text:
a covering read of a collated text column goes to the row.

**Why not the plugin's variable-length formats.** The plugin has been
through three: a 2-byte length prefix before the bytes, which sorted by
length; a value-ordered form that pads every part to its declared width and
moves the length behind the bytes, which needs a padding rule per collation
and stores the width whatever the value's length; and collation weights with
a trailing length under `NO PAD`. The escaped, terminated form above sorts
by value, stores no width, needs no padding rule, and under a binary
comparison two values are equal if and only if their bytes are, so nothing
has to be trimmed. The sign flip, the IEEE transform and the `DESC`
inversion are the plugin's, unchanged.

**A secondary index key** is `0x03 index_id(4)` followed by the index
columns as parts (nullable where the column is, `DESC` where the index says
so) followed by the primary key's parts, encoded exactly as they are in the
row key. `0x02 table_id(4)` plus that suffix is the row key, which is how
`bc_scan_row` finds an entry's row.
Because every part is self-delimiting, the suffix is found by skipping the
index columns with the index's catalog layout, and `bc_key_part_*` decodes
any part by position, index columns first. The prefix is `0x03 index_id`,
not the plugin's `0x03 table_id index_id`: index ids are global, so the
table id would add four bytes and no information
(`docs/catalog_design.md`, *Secondary-key prefix*).

**`bc_key_successor`** of a prefix is the smallest key above every key that
starts with it: the last byte below `0xFF` incremented and everything after
it dropped. A prefix that is all `0xFF` has no successor and yields the empty
view, which `bc_scan_open` reads as "to the end". A partial key is a prefix
of every key that extends it, so `[key, successor(key))` is the range of a
`WHERE col = $1` on the first index column, and `[successor(key(a)), …)` is
`col > a`; there is no exclusive-low or inclusive-high flag.

**Limits.** A key over 65 535 bytes, the engine's hard ceiling, is
`BC_E_LIMIT` at `bc_key_end`; a key over the engine's configured limit
(`max_key_bytes`, 4096 by default) is `BC_E_LIMIT` at commit. A text key
part escapes each `0x00`, so its encoded length is at most twice its bytes
plus two.

**Decoding is total.** `KeyReader` reads a part of a given layout or reports
the key malformed: a truncated part, a marker that is neither `0x00` nor
`0x01`, a `0x00` in a bytes part followed by anything but `0x00` or `0xFF`.
It never reads past the key, and once a key is malformed every later read
fails too.

## Row format

```
u16   layout_generation   ; which offset table decodes this row
u16   bitmap_bytes        ; size of the null bitmap; a multiple of 8
u16   var_table           ; offset of var_end: where the fixed region ends
u16   nvar                ; entries in var_end
u32   row_bytes           ; the whole row; equal to the record's length
u8[]  null_bitmap         ; bit set = present, indexed by permanent column id
      fixed columns       ; 8 bytes each, at permanent offsets, little-endian
u32[] var_end             ; nvar end offsets, relative to the start of var data
u8[]  var data
```

The header is 12 bytes. Every fixed column is one 8-byte word: `i64` and
`f64` as their bits, `bool` as 0 or 1 (a decoder reads any non-zero word as
true). Variable column `k` occupies `[var_end[k−1], var_end[k])` of the var
data, with `var_end[−1]` taken as 0. All integers are little-endian. The row
sits at an arbitrary offset in a record or an arena, so decoders copy bytes
and nothing depends on alignment.

**Columns have permanent ids and positions.** A column's id is its bit in
the bitmap and is never reused. Its position, a byte offset for a fixed
column or a slot for a variable one, is assigned when the column is added
and never changes within a layout generation. New columns append to their
region; a dropped column leaves its position reserved. A type change is a
new column.

**The bitmap says present.** A column whose bit is set has a value at its
position; a column whose bit is clear is null, whether the writer set it
null or did not know the column. That second case is what makes short rows
work: a row written before a column was added has the column's bit clear
and reads as null with no further test. `bc_col_null(row, col)` is the bit,
and the position argument revision 2 of the plan API added to it is not
needed; `docs/plan_api_design.md` is updated accordingly. One decoder,
`row_column`, reads every column of a stored row, and says it is absent, a
value, or malformed. Absent is a clear bit, or no bit: NULL. Malformed is a
header that does not parse, or a set bit for a column the row does not
hold: a fixed column past the fixed region, a variable slot past `nvar`, or
a `var_end` that runs past the row. Malformed is `BC_E_CORRUPT` on every
path that reads a column (`bc_col_*`, `bc_emit_col`, `bc_row_from`, a
key built from a row, SQL results, `Database::table_rows`), never read as 0,
empty or NULL (#520). Plans name columns by id since plan API revision 5,
and the runtime maps an id to its position; the decoder still refuses an
offset inside the header or off a word boundary. Nothing is read past
the end.

**The bitmap is sized at create, with a ceiling.** `bitmap_bytes` is the
smallest multiple of 64 bits that holds every column id of the table with at
least 16 ids to spare, fixed when the layout generation is created; a
bitmap that would push the fixed region past the 16-bit `var_table` is
refused at create. Adding
a column uses the next id; when the next id has no bit, `ADD COLUMN` is
refused until an `ALTER TABLE … REBUILD` starts a new generation with a
larger bitmap. The size is in the row so that every structural check is
local to the row: the fixed region starts at `12 + bitmap_bytes`, the
var_end table at `var_table`, the var data at `var_table + 4 × nvar`, and
each is checked against `row_bytes`. The sizes in the header exist for the
same reason: appending a fixed column lengthens the fixed region, so a
reader holding an older layout's constants cannot find `var_end` from them;
the row says where it is.

**Header or trailer.** The alternative weighed was a trailer: `var_table`
and `nvar` at the row's end, read backwards from `row_bytes`. The two carry
the same bytes. A writer gains nothing from the trailer: `var_table` and
`nvar` come from the layout, known before the first byte is written, and the
var_end offsets are not known until the variable data is laid out in either
design. A reader loses a little: the trailer is found through `row_bytes`,
which is in the header anyway, so the structure is described in two places,
and a row cut short loses its description with its tail, where a header
survives truncation and names what is missing. Header fields are kept, as
the plan API's revision 2 has them.

**The row's length is 32 bits.** The u16 `row_bytes` the prototype carried
capped a row at 64 KiB, a quarter of what MariaDB allows in a row and below
what a `TEXT` column is for. `row_bytes` is u32 and the row's ceiling is the
engine's value limit: 256 MiB less a byte as a hard ceiling, enforced by
`bc_row_end` as `BC_E_LIMIT`, and the configured `max_value_bytes`, enforced
by the engine at commit. `var_table` stays u16: a fixed region is 8 bytes a
column and the bitmap bounds the column count, so it never nears 64 KiB.

**The format is versioned through the catalog.** A row carries no format
version of its own: every row belongs to a table, and every table's record
in the catalog carries the record format, which names the row format its
positions are for. This row format is the one of record formats 2 and 3
(3 is the catalog record of #489). The prototype's format 1
(an 8-byte header, a 16-bit length, a bitmap meaning null) is refused when
the catalog loads, with an error that says the database is from an earlier
build, so no row of it is ever decoded under this format.

**Generations.** `layout_generation` names the offset table that decodes the
row. A plan's constants are its own generation's; a row of another
generation is `BC_E_LAYOUT` to `bc_row_from` and takes the catalog's slow
path for reads, which is not built yet (#503). Rebuilds are rare and
explicit, and updates rewrite rows into the current generation.

**Result rows** use the same format with a layout derived from the plan's
`result_types`: column `i` has id `i`, is nullable, and is a fixed column
or a variable one by its type. The runtime decodes them with that layout.

## What the plugin adopts

The plugin keeps its own key and row codecs until the shared ones are proven
by the hand-written plans and the fault tests (#496). It then adopts them as
it adopted its last two key formats: a new per-table `key_format` value
means "the shared key encoding" and a new row-format version means "the
shared row format"; new tables use them and existing tables keep theirs
until `ALTER TABLE … FORCE`. No flag day, and a database written through
MariaDB becomes readable by the sql-layer table by table. What that costs
the plugin is in the plugin: a conversion of `key_copy()` and `record[0]`
layouts into the codec's neutral input, a mapping from MariaDB's types onto
the physical types above, and the collation weights taken from
`CHARSET_INFO` and written as `bytes` parts. The sql-layer gains nothing
from the plugin and includes nothing of it; the plugin links the sql-layer's
archive through a header bridge (#488). Nothing in the plugin changes in the
PR that lands this document.

Two things the codec does not decide, the catalog does
(`docs/catalog_design.md`): index keys are prefixed by the index
id alone, and an index records `DESC` per part and whether it is unique. A
unique index has the same entries as any other; its check is a scan of the
index-part prefix. The codec takes a `KeyPart{type, nullable, desc}` per
part and is indifferent to the prefix.

## Validation

Property tests in `tests/codec_test.cpp`, each over thousands of random
values with the edges mixed in:

- every part type round-trips through `KeyReader`, with and without a
  marker, `ASC` and `DESC`;
- `memcmp` order on encoded keys equals the typed order, for each type and
  for composite keys with ties on the leading parts: negative integers,
  negative and special doubles (`±0.0`, `±∞`, NaN), prefixes of byte
  strings, nulls first under `ASC` and last under `DESC`;
- `bc_key_successor` bounds every key under its prefix;
- a key cut at any byte, or with a bad marker or escape, is reported, not
  over-read;
- rows of every physical type round-trip, a row written before a column was
  added reads the column as null, a row written after reads under the older
  layout, the bitmap ceiling refuses the column that has no bit, and a row
  over the limit is refused.

The plan API's tests (`tests/plan_api_test.cpp`) cover the calls on top:
`bc_key_part_*` on an index scan, `bc_raise` and the duplicate-key path,
scans with empty bounds and in reverse, the `f64` and `bool` calls, and
every fault path (`BC_E_HANDLE` on a wrong part, a stale view, a raised
error that is not retried).
