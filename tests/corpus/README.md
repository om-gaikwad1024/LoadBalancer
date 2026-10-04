# Parser corpus

`reject/<status>_<description>.txt` holds malformed and smuggling-style requests (plan IV.3, VII).
`HttpCorpus.EveryRejectCaseFailsWithItsStatus` feeds each one to the request parser, whole and
in many split patterns, followed by a connection close. Each must fail with the status in its
file name, and none may crash.

Files use C-style escapes so the exact bytes survive editors and git: `\r` `\n` `\t` `\\` `\xHH`.
Literal line breaks in a file are ignored; they only make the file readable.

The corpus test uses fixed parser limits: request line 1024, header section 2048 bytes,
20 header fields, chunk line 64. `tools\fuzz.ps1` decodes these files into the fuzzer's seed corpus.
