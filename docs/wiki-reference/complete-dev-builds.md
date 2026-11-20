<!-- Portions Copyright 2026 The Hollycast Authors -->

# Publish rolling latest-dev releases from complete dev builds

Use the [latest Dev release](https://github.com/nexus382/Hollycast/releases/tag/latest-dev) to get the latest complete build. Its six downloads come from the same commit. The previous complete build stays available while a new one is being prepared. Each release includes checksums.

## Download and verify a Dev build

1. Open the latest Dev release and check the source commit and build date. Under **Assets**, choose the ZIP for your platform and download `SHA256SUMS.txt`.
2. Hash the downloaded ZIP. On Windows PowerShell, run `Get-FileHash -Algorithm SHA256 "path-to-download.zip"`. On Linux, use `sha256sum` with the ZIP path, or use `shasum -a 256` on macOS. Compare the full hash with the line for that ZIP in `SHA256SUMS.txt`.
3. When the hashes match, extract the ZIP and any nested archive.

[Source pull request #40](https://github.com/nexus382/Hollycast/pull/40)
