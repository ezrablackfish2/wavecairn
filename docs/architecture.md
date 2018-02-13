# Architecture

WaveCairn separates byte-level parsing from domain mutation. `parse_command_tape` and `parse_package` return owned documents after validating framing and checksums. `process_input` identifies the container, extracts a command tape when needed, and dispatches records through `Session::apply`.

The session subsystems deliberately expose read-only collection views plus small domain operations. Indexes are rebuilt after bulk canonicalization. This keeps import behavior deterministic while allowing inspection tools to walk the full model without depending on internal hash-table ordering.

Diagnostics are accumulated rather than printed. A caller can display them, serialize them, or reject a session according to its own policy. Validation is a separate pass because a structurally valid file may still have missing creative references, such as a clip whose media was not delivered.

The report module turns the same model into operational artifacts. The merge module builds a new command tape instead of mutating internal vectors directly, so merge behavior is auditable and uses the normal command validation path.
