# cJSON

Upstream: https://github.com/DaveGamble/cJSON

Vendored without modifications from tag `v1.7.19`, commit
`c859b25da02955fef659d658b8f324b5cde87be3`.
The original MIT license is included in LICENSE and the source headers.

TGFS checks UTF-8, embedded NUL, number grammar and duplicate object keys
around cJSON. The offline prototype uses an 8 MiB JSON document limit,
64-level decoded tree limit and at most 65,536 records/entries/parts.
These are prototype resource limits, not a final TGFS wire-format decision.
