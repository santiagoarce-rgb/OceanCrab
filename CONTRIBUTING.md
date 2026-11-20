# Contributing to Ocean Crab

Thanks for helping! This project recompiles a commercial PSP game, so a few rules matter more than usual.

## Ground rules

1. **Never commit game data.** That includes the EBOOT, ISO/CSO images, extracted assets, frame dumps and the generated AOT corpus (`profiles/*/generated/`, except `generated_units.hpp`). They are git-ignored; keep it that way.
2. **Never copy code from incompatible licenses.** The project is MIT. Emulators such as PPSSPP (GPL) are fine as a *behavioral reference*, but do not paste or adapt their source. See [`docs/SOURCE_PROVENANCE.md`](docs/SOURCE_PROVENANCE.md).
3. **Keep the framework game-neutral.** Addresses and behavior specific to SpongeBob belong in `profiles/spongebob/`, not in `src/`, `include/` or `tools/`.

## Code generator changes

Changes to `tools/codegen_main.cpp`, `src/program_analysis.cpp` or `src/decoder.cpp` affect every generated unit and force a full corpus regeneration and rebuild (about an hour). **Open an issue or draft PR describing the change before implementing it**, with:

- the instruction pattern or analysis gap,
- the generated C++ before and after,
- how you verified it (tests, a game run, a comparison with a reference).

## Workflow

1. Fork and create a branch from `main`.
2. Build and run the tests:
   ```bash
   cmake -S . -B out/crab -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPSPRECOMP_PROFILE=spongebob
   cmake --build out/crab -j"$(nproc)"
   ctest --test-dir out/crab --output-on-failure
   ```
3. Keep commits **small and focused**: one logical change per commit.
4. Open a pull request. CI must pass.

## Commit messages

[Conventional Commits](https://www.conventionalcommits.org/) with a scope:

```
fix(io): return ENOENT for missing .bik files
feat(ctrl): implement sceCtrl input from host keyboard/gamepad
docs: add configuration reference
```

Common scopes: `codegen`, `runtime`, `io`, `ge`, `audio`, `ctrl`, `movie`, `menu`, `utility`, `debug`.

Explain the *why* in the body: which address or NID, what the game expected, what you observed.

## Reverse-engineering notes

When you identify a function or structure, document it next to the override (comment with the guest address and what it does) and add findings to [`NOTES.md`](NOTES.md). Named, understood code is what will make enhancements and co-op possible.

## Reporting bugs

Use the issue templates. Include the commit, your distro and GPU, the environment variables you used and the relevant log. A run with `PSPRECOMP_TRACE_ON_ERROR=1` is the most useful log for crashes.
