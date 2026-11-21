# symbolize — automatic first-pass naming (phase 2)

Names functions and classes of the EBOOT using only information that is already
in the binary. Run it after generating the analysis reports:

```bash
tools/symbolize/run.sh            # uses out/crab; writes profiles/spongebob/analysis/symbols/
```

`run.sh` builds `dump_image`, which loads and relocates the EBOOT exactly like the
runtime does, and then runs `symbolize.py` on that image.

## Sources of names

| Kind | Confidence | How |
|---|---|---|
| `import` | high | Import stubs from the analyzer's import report (`sceKernel*`, …) |
| `lua-binding` | high | `luaL_reg {name, fn}` tables; the library name comes from the function that registers the table (`luaL_register(L, "Char", t)`) |
| `debug-string` | high/medium | The function references a message containing its own `Class::Method` name (high when it also calls the engine printf `0x089C9E34`) |
| `ctor` | medium | The function builds the address of exactly one class vtable and stores it in an object: constructor or destructor |
| `virtual-named` | medium | Vtable slot N of a subclass, when slot N of an ancestor has a known method name |
| `virtual` | medium/low | `Class::vfunc_NN`: the most basic class whose vtable holds the function at slot NN |

Classes come from GCC RTTI. `std::type_info` objects (`{vptr, name}` plus a base
for `__si_class_type_info`, or a base list for `__vmi_class_type_info`) give each
polymorphic class and its primary parent. Vtables are `{0, &typeinfo, vfunc…}`.

## Outputs

- `symbols.csv`: `address,name,kind,confidence,evidence`
- `classes.csv`: class, typeinfo, vtable, parent, number of virtuals
- `class_tree.txt`: indented hierarchy
- `lua_tables.csv`: Lua libraries exposed to the level scripts
- `summary.txt`: counts

To browse in Ghidra, load the EBOOT at `0x08804000` and run
`tools/ghidra/ImportSymbols.java` on `symbols.csv`.

The scan is linear (it follows `lui`/`addiu`/`ori` pairs, with no control
flow), so a few references are missed. Names marked `medium` or `low` are
leads to check in Ghidra, not facts.
