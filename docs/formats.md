# WaveCairn interchange formats

## WCTP command tape

A WCTP file is an ordered list of commands applied to a session. All multibyte values are little-endian.

### Header

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic `WCTP` |
| 4 | 1 | format version (currently 3) |
| 5 | 1 | tape flags |
| 6 | 2 | reserved, written as zero |
| 8 | 4 | session seed |

Commands continue until end-of-file; there is no declared command count. Import limits bound both total bytes and accepted command records.

### Command record

Each record has a fixed prefix, a payload, and a trailing payload checksum:

| Field | Size | Meaning |
| --- | ---: | --- |
| opcode | 1 | command operation |
| flags | 1 | record flags, including barrier state |
| payload length | 2 | payload bytes |
| transaction | 4 | transaction identifier |
| payload | variable | command-specific data |
| payload checksum | 2 | Fletcher-16 checksum of payload |

Import limits are checked before allocation. Unknown opcodes remain structurally valid and can be diagnosed or ignored by a processing profile.

### Transactions and barriers

Commands with the same transaction number are logically related. A barrier asks the processor to validate accumulated state before moving to later lifecycle phases. A migration application can therefore record asset declaration, timeline construction, routing, and render planning as separate phases while preserving one portable stream.

### Processing profiles

The public processor supports complete, import, editing, routing, rendering, and package profiles. A profile does not change parsing. It selects which valid command categories are applied after the tape has passed structural checks.

## WCPK package

WCPK is a sectioned container for a command tape and associated metadata.

### Header

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic `WCPK` |
| 4 | 2 | package version |
| 6 | 2 | package flags |
| 8 | 8 | package identifier |
| 16 | 4 | section count |
| 20 | 4 | header checksum |

### Section record

| Field | Size | Meaning |
| --- | ---: | --- |
| tag | 4 | printable four-character tag |
| flags | 2 | section flags |
| reserved | 2 | reserved for alignment |
| revision | 4 | section revision |
| payload length | 4 | payload byte count |
| payload checksum | 4 | payload checksum |
| payload | variable | section data |

Recognized tags include `TAPE` for command tape bytes, `META` for UTF-8 metadata, `NOTE` for human notes, and `MANF` for a serialized manifest. Applications may retain unknown printable tags.

## Safety and limits

Both parsers reject truncated headers, invalid magic, unsupported versions, impossible lengths, invalid checksums, excessive command counts, excessive section counts, and payloads over configured limits. Input parsing does not require filesystem access and never follows media paths.

## Canonicalization

Canonicalization produces deterministic ordering for collections where order has no creative meaning. It rebuilds lookup indexes after sorting. Package sections are ordered by tag, revision, flags, and payload digest; timeline and routing collections use stable entity and position keys.
