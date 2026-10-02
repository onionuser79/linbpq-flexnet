# Patches

Fixes to LinBPQ code paths found while running linbpq-flexnet, packaged
separately from the overlay so they can be applied to stock LinBPQ as
well. Each directory has a README explaining the symptom, the cause, how
to apply the patch and how to check it worked.

A patch here is not part of the overlay until a release says so in
[RELEASE_NOTES.md](../RELEASE_NOTES.md).

| Patch | Fixes | Applies to | Status |
|---|---|---|---|
| [0001-l2-appl-alias](0001-l2-appl-alias/) | An AX.25 connect to an aliased APPLICATION runs only the first 12 characters of the alias, as the connecting user. Telnet `ATTACH` aliases fail with `needs SYSOP Status` / `Invalid Command` | LinBPQ 6.0.25.41, linbpq-flexnet v2.3.0 | Compile-tested; awaiting live verification |

Upstream patches keep upstream's CRLF line endings. `.gitattributes`
excludes this directory from the repository's LF normalisation.
