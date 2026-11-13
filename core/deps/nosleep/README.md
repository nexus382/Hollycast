# Phone screen wake fallback media

The inline WebM and MP4 video assets in `media.h` come from
[NoSleep.js 0.12.0](https://github.com/richtr/NoSleep.js), commit
`07fcee254724ab1b79076fbc22f3dd447649a2eb`, `src/media.js`.
They are distributed under Rich Tibbett’s MIT license (see `LICENSE`).

The original media bytes are unchanged. The CommonJS export is renamed to
`PhoneVmuWakeMedia` and wrapped in a C++ raw string for local serving.
Hollycast manages wake-lock acquisition and release in its phone page. These
silent videos provide a user-activated fallback on local HTTP, where the native
Screen Wake Lock API is unavailable. No external requests are made.
