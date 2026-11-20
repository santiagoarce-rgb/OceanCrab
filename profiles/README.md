# Profiles

Profiles contain everything PSPRecomp needs for a particular title without contaminating the reusable framework with game-specific addresses or behavior.

A profile is selected at configure time:

```bash
cmake -S . -B out/<profile> -DPSPRECOMP_PROFILE=<profile>
```

| Profile | Title | Status |
|---|---|---|
| `spongebob` | SpongeBob's Truth or Square (`ULUS-10478`) | Active — the Ocean Crab port |

Each profile keeps its generated AOT corpus in `profiles/<id>/generated/`. That corpus is derived from the user's own game executable and is never committed.

See `docs/PROFILE_GUIDE.md` before adding another title.
