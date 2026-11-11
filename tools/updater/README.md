<!-- Portions Copyright 2026 The Hollycast Authors -->
# Hollycast updates

The standalone UI checks the published `nexus382/Hollycast` build catalogue at startup. Help > Check for Updates also opens the dialog. The saved Master/Dev selection controls the next check. Startup checks can be disabled; offline checks do not interrupt the user.

The catalogue groups the platform CI releases by commit and waits for the complete platform set. The dialog shows the installed version/build date and selected build's commit/date. GitHub's paginated compare endpoint supplies all missed commit messages, newest first, as the combined CI changelog. A branch switch can intentionally select an older history; normal checks never downgrade an ahead-of-track build.

Install starts only after Update is pressed. Downloads must match the release asset's published size and SHA-256 digest. Missing checksums, incomplete history, ambiguous archives and unsupported architectures stop the update before Hollycast exits.

On Windows, keep `HollycastUpdater.exe` beside `Hollycast.exe` when distributing or extracting a build. CMake builds and installs both; CI includes both in Windows packages. A private copy downloads and stages the package, optionally hands off to the package's verified helper, waits for Hollycast to exit, replaces only the managed executables and relaunches in the original working directory. Backups are retained beside the installation for recovery. The helper and manifest protocol must remain compatible with older installed versions.

Linux supports the published x86_64 AppImage. macOS supports the packaged signed application bundle, with the user profile outside the bundle. Their detached helper verifies and stages the replacement before requesting shutdown. Saves, settings and other profile data are never overlaid from the archive.

GitHub Android builds pass `-PgithubUpdates=true` to Gradle to include the APK-install permission overlay. Store builds omit it and direct users to their store for installation. Android owns installation confirmation and signing validation; older version codes cannot be installed over newer ones. The CI debug keystore is already shared by Master and Dev GitHub APK builds. A Play-signed installation cannot be replaced by an APK signed with a different certificate.

UWP, iOS and Switch require their system or homebrew installation flow. They can check the catalogue, but the automatic installer reports the platform limitation.

No updater should ever be validated against a user's only working installation. Exercise successful updates, cancellation, bad checksums, missing permissions, nested packages, parent-process timeouts, file locks and rollback in a disposable installation before release.

## Google Play

Store builds use Play In-App Updates 2.1.0 for availability and the consented immediate update flow. Checks run on startup when enabled and on Check now. The Update button rechecks availability before starting a fresh flow; cancellation leaves the emulator usable, and an interrupted consented flow resumes when Android returns to the app. If Play disallows in-app installation, the button opens the Play listing. Store builds never call the GitHub package installer. Manage Play testing track opens the account enrollment page; the app does not impersonate enrollment or select an ineligible release.

Play does not return release names, dates or changelogs. After a Play version is published, add its actual metadata to `resources/update/play-releases.json` on master. Each entry has `versionCode` (integer), `versionName`, `date` (ISO 8601), `channel` (`master` or `dev`), and `notes`. Notes are combined from the installed code up to the exact version Play offers; testing notes are excluded from a stable target. The feed is optional: missing/offline metadata shows the Play version code and explains that notes/date are unavailable, without blocking installation. Never populate this feed with unpublished builds.

A live Play update requires an account eligible for a higher version on a Play track (or internal app sharing), with matching application ID and signing. Local sideloading alone cannot prove that flow. Standard Gradle builds omit APK installation permission; only GitHub CI uses `-PgithubUpdates=true`.

## Regression checks

`tests/updater` can be configured as a standalone CMake project. It exercises official catalogue validation, architecture selection, paginated changelogs, downgrade rules, checksums and exact-version Play notes. On Windows, pass `-DUPDATER_HELPER=<absolute path to HollycastUpdater.exe>` to also run real replacement, nested archive, cancellation, rollback and parent-timeout cases against disposable dummy executables. The fixtures retain their folders and results for diagnosis; they never use the working emulator or user profile.

Android's `testDebugUnitTest` runs the Play controller tests with mocked platform/Play responses. Run it with and without `-PgithubUpdates=true` to verify both distributions. These host tests do not replace an eligible-account Play installation test. CI runs the catalogue checks on Linux, helper transactions on Windows x64, and both Android source configurations.

The headless ImGui frame tests exercise the real updater dialog with a delayed backend response. They cover automatic startup notification, deferral until idle, and manual checking with startup checks disabled, including the distinction between a never-opened popup and user dismissal.
