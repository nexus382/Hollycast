<!-- Portions Copyright 2026 The Hollycast Authors -->

# ReShade FX compiler

The standalone language compiler is vendored from
[crosire/reshade](https://github.com/crosire/reshade/tree/7bf9de8b33bcc76c3177007e65d73c72dd0f34c0/source),
commit `7bf9de8b33bcc76c3177007e65d73c72dd0f34c0`, under the BSD 3-Clause license
in [LICENSE.md](LICENSE.md).

Only the parser, preprocessor, expression/type system, and GLSL code generator
are included. Hollycast owns texture loading, pass scheduling, resource limits,
parameter storage, and renderer integration. The compiler does not install
hooks or inject code into another application.

The upstream files retain their original copyright notices. The local CMake
target builds the compiler for every standalone renderer. The symbol table uses
the platform's `alloca` header so the same source also builds on macOS and Linux.

The GLSL generator records escaped uniform and sampler names in reflection so
the host binds the exact identifiers emitted for namespaced effects.

Generated local names also reserve Hollycast's host interface identifiers so
effect variables and struct fields cannot shadow host bindings or expand macros.
