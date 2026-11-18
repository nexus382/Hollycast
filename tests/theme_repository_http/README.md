# Theme repository HTTP initialization regression

Requires CMake, a C++17 compiler, the repository's dependency submodules, and
libzip development files (including its CMake package).

```sh
cmake -S tests/theme_repository_http -B build/theme-http-tests
cmake --build build/theme-http-tests --config Release
ctest --test-dir build/theme-http-tests -C Release --output-on-failure
```

These tests compile the real theme repository with a fake HTTP client that
returns 500 if used before initialization, matching the Windows client's
failure behavior. They check a cold start, a fetch after another service shuts
down HTTP, and an injected transport that must not touch the native client.
No network requests are made. Native WinInet behavior and the browser UI still
need platform testing.
