# Troubleshooting by diagnostic id

Every failure Chameleon reports carries a stable id such as `CML5300`. The id is printed with the message and a
"What to do" sentence by `cml_broker`, by `cml_ui`, in the editor extensions and by `cml_broker check`. Look the id up
below: each entry gives the cause, a typical message and what to do. Messages name the file, module or symbol involved;
the entries show them with `<...>` where the real text varies.

The first digit is the stage: `CML1xxx` parse and PDB (including the build-requirements audit), `CML2xxx` compile,
`CML3xxx` link and bind, `CML4xxx` load and hook (agent side), `CML5xxx` runtime (reload pipeline, control pipe, UI,
revert, restart).

## First steps for any problem

1. Check the module against the build requirements: `cml_broker check <module.exe|module.dll>`. Every `FAIL` and `WARN`
   line names its id; most "it does not reload" reports end here (see `docs/BUILD_REQUIREMENTS.md`).
2. Run the broker with `--verbose --log broker.log` and read the lines before the diagnostic. Every decision of the bind
   planner is logged at `trace`.
3. Look for the diagnostic in the `diag` events or the reload response: the `id`, `message` and `action` fields are the
   text shown below.
4. Hot-restart (`restart`) is the way out when a change cannot be patched in place (`docs/KNOWN_LIMITATIONS.md`).
5. Slow, not failing (WP-50): the broker log says where the time went, in lines that start with a `WP50-` tag.
   * `WP50-INDEX module=... pdb_ms=... audit_ms=... scan_ms=... scan_read_ms=... scan_missing=... baseline_ms=...`: one
     per PDB index (enabling a module, and again in the new process of a hot-restart). `scan_ms` is the include scan
     (ADR-0013): with thousands of translation units it is most of the time, `scan_missing` counts the include-path
     candidates that do not exist. `pdb_ms` is the PDB, `baseline_ms` the hashes of the tracked sources.
   * `WP50-RESTART old_pid=... total_ms=... prepare_ms=... terminate_ms=... relaunch_ms=... register_ms=... reenable_ms=...
     index_ms=... reapply_ms=... index_module=...`: one per hot-restart (also the `timing` and `stages_ms` members of the
     `restart` result). `index_ms` is the wait for the new process's indexes; their own phases follow (`index_*_ms`).
   * `WP50-HOOKS side=agent ...` and `side=broker ...`: one each per `InstallHooks`. The agent's line splits the request
     into the wait, the application hooks, planning (`plan_us`, `probe_us`), `VirtualProtect` (`protect_us`), stopping the
     threads (`stop_us` = `enumerate_us` + `suspend_us` + `context_us`), the writes (`write_us`, slowest hook
     `write_max_us`) and resuming; the broker's line gives the round trip it saw. `batch=1` is the first of the process.
     `query_us` / `queries`: of `probe_us`, the time in and the number of VirtualQuery calls. The installer answers
     VirtualQuery once per memory region and batch (WP-50); a first batch of new hooks into a large module still pays for the
     first query of the code region.
   * `WP50-PLAN module=... object_ms=... image_ms=... bind_ms=... export_parses=... prove_ms=... narrow_ms=...
     pdb_*_ms=...`: the plan stage of one recompiled object, split up (`image_ms` is near 0 once the module file was read in
     the background).
   * `WP50-ONEOFF what=module-image|linker-detection|reserve-range ms=...`: work done once per module at its first reload.
     The module file is normally read in the background once the index is ready (`image=background`); `image=reload` means
     the first reload had to read it itself (set-option `prefetch_module_image` false, or the reload came first).
   * `WP50-SCAN-CACHE hit|miss module=... reason=...`: whether an index took the file lists and include graph of an earlier
     index of the same module file (a hot-restart's new process, a module loaded again). `miss reason=size|mtime|pdb` means the
     module file was rebuilt since; `absent` that this broker had not indexed it before. Set-option `cache_include_scan` false
     scans every time.
   * The two set-options of WP-50 (control command `set-option {"name":..., "value": true|false}`, both default `true`):
     `prefetch_module_image` (read the module file for the function comparison in the background once its index is
     ready; applies to modules enabled afterwards) and `cache_include_scan` (reuse the file lists and include graph of an unchanged
     module file this broker indexed before; applies to indexes started afterwards). Turn one off only to rule it out when
     looking for a problem.

## Ids raised by the integrations

The CMake helper (`CML1700` to `CML1749`), the VS Code extension (`CML5990` to `CML5993`) and the Visual Studio extension
(`CML5994` to `CML5997`) raise their own ids outside the C++ code; their entries are at the end of this document. The
licensing hook (`CML5998`, `CML5999`) is never raised by the shipped build.

## The entries

## CML1100 to CML1199

### CML1101

- **Cause:** /DEBUG:FASTLINK (or a PDB without function symbols).  
- **Message:** `The PDB of <...> holds no function symbols for its compilands (a /DEBUG:FASTLINK or stripped PDB); Chameleon needs /DEBUG:FULL.`  
- **What to do:** Relink the module with /DEBUG:FULL.  
- **Raised by:** `broker/cml_core/include/cml/core/module_audit.h`

### CML1102

- **Cause:** Compilands without a recorded command line.  
- **Message:** `The PDB of <...> has no compilands that Chameleon can recompile (no S_COMPILE3 records of C/C++ objects).`  
- **What to do:** Build the module with MSVC /Zi, /ZI or /Z7 (clang-cl: /Z7), and link with /DEBUG:FULL.  
  Other variants of the advice: Build with MSVC /Zi or /Z7 (clang-cl: -gcodeview-command-line) or provide compile_commands.json. / Build them with MSVC /Zi or /Z7 (clang-cl: -gcodeview-command-line) or provide compile_commands.json.  
- **Raised by:** `broker/cml_core/include/cml/core/module_audit.h`

### CML1103

- **Cause:** /GL.  
- **Message:** `Compiland <...> in <...> was built with /GL (LTCG); its objects are IL, not COFF<...> (<...> compilands: <...>).<...>.`  
- **What to do:** Disable /GL for modules you want to hot-reload.  
- **Raised by:** `broker/cml_core/include/cml/core/module_audit.h`

### CML1104

- **Cause:** /guard:cf.  
- **Message:** `Module <...> is built with /guard:cf; patched code would not be a valid CFG call target.`  
- **What to do:** Build with /guard:cf- for development configurations.  
- **Raised by:** `broker/cml_core/include/cml/core/module_audit.h`

### CML1105

- **Cause:** /Gy absent or unknown.  
- **Message:** `Compiland <...> of <...> was not built with /Gy; functions cannot be patched individually<...> (<...> compilands: <...>).<...>.`  
- **What to do:** Add /Gy to the compile flags.  
- **Raised by:** `broker/cml_core/include/cml/core/module_audit.h`

### CML1106

- **Cause:** x86, ARM64, ... (and the PE reader's CML1303).  
- **Message:** `The PDB of <...> is for machine <...>; only x64 is supported in v1.`  
- **What to do:** Use an x64 build.  
- **Raised by:** `broker/cml_core/include/cml/core/module_audit.h`


## CML1200 to CML1299

### CML1200

- **Cause:** See the message printed with the id.  
- **Message:** `cannot open '%s' for reading`  
- **What to do:** Check that the file exists and is readable.  
  Other variants of the advice: Check that the path names a regular file. / Check that the file is not locked or truncated.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1201

- **Cause:** See the message printed with the id.  
- **Message:** `COFF object is %zu bytes, too small for the 20-byte COFF header`  
- **What to do:** Check that the file is a complete .obj produced by the compiler.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1202

- **Cause:** See the message printed with the id.  
- **Message:** `section table (%u sections at offset %zu) extends past the end of the %zu-byte object`  
- **What to do:** The object is truncated or corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1203

- **Cause:** See the message printed with the id.  
- **Message:** `anonymous COFF header has version %u; only /bigobj version 2 is supported`  
- **What to do:** The file is not a /bigobj object (version 0 is a short import object); pass the compiler's .obj, not an import library member.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1204

- **Cause:** See the message printed with the id.  
- **Message:** `anonymous COFF header has an unrecognised class id (not ANON_OBJECT_HEADER_BIGOBJ)`  
- **What to do:** Only regular COFF and /bigobj objects are supported; LTCG/bitcode objects are not.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1205

- **Cause:** See the message printed with the id.  
- **Message:** `section %u '%.*s' raw data (offset %u, size %u) extends past the end of the %zu-byte object`  
- **What to do:** The object is truncated or corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1206

- **Cause:** See the message printed with the id.  
- **Message:** `section %u '%.*s' declares %llu relocation records at offset %u which extend past the end of the %zu-byte object`  
- **What to do:** The object is truncated or corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1207

- **Cause:** See the message printed with the id.  
- **Message:** `object declares %u symbols but its symbol table offset is 0`  
- **What to do:** The object is corrupt; rebuild it.  
  Other variants of the advice: The object is truncated or corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1208

- **Cause:** See the message printed with the id.  
- **Message:** `string table at offset %llu is only %llu bytes; its size field needs 4`  
- **What to do:** The object is truncated or corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1209

- **Cause:** See the message printed with the id.  
- **Message:** `symbol %u has string-table offset %u which is outside the %zu-byte string table or unterminated`  
- **What to do:** The object is corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1210

- **Cause:** See the message printed with the id.  
- **Message:** `symbol %u declares %u aux records but only %u records remain in the symbol table`  
- **What to do:** The object is truncated or corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1211

- **Cause:** See the message printed with the id.  
- **Message:** `section %u has a long-name reference '%.8s' that does not resolve to a string-table entry`  
- **What to do:** The object is corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1212

- **Cause:** See the message printed with the id.  
- **Message:** `section %u '%.*s' is flagged LNK_NRELOC_OVFL but its overflow relocation record is outside the object`  
- **What to do:** The object is truncated or corrupt; rebuild it.  
  Other variants of the advice: The object is corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1213

- **Cause:** See the message printed with the id.  
- **Message:** `symbol %u refers to section %d but the object has only %u sections`  
- **What to do:** The object is corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1214

- **Cause:** See the message printed with the id.  
- **Message:** `relocation %zu of section %u '%.*s' targets symbol index %u but the object has %u symbol records`  
- **What to do:** The object is corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1215

- **Cause:** See the message printed with the id.  
- **Message:** `relocation %zu of section %u '%.*s' targets symbol index %u which is an auxiliary record, not a symbol`  
- **What to do:** The object is corrupt; rebuild it.  
- **Raised by:** `core/cml_coff/src/coff.cpp`

### CML1216

- **Cause:** See the message printed with the id.  
- **Message:** `regular COFF object declares %u sections; the limit is 65279 (larger objects need the /bigobj format)`  
- **What to do:** The object is corrupt; rebuild it (compile with /bigobj if it genuinely needs this many sections).  
- **Raised by:** `core/cml_coff/src/coff.cpp`


## CML1300 to CML1399

### CML1300

- **Cause:** See the message printed with the id.  
- **Message:** `cannot open '%s' for reading`  
- **What to do:** Check that the file exists and is readable.  
  Other variants of the advice: Check that the path names a regular file. / Check that the file is not locked or truncated.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1301

- **Cause:** See the message printed with the id.  
- **Message:** `not a PE image: missing 'MZ' DOS header in %zu bytes`  
- **What to do:** Pass a .exe or .dll produced by the linker.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1302

- **Cause:** See the message printed with the id.  
- **Message:** `e_lfanew 0x%X puts the NT headers outside the %zu-byte image`  
- **What to do:** The image is truncated or corrupt.  
  Other variants of the advice: The module is not a normally mapped PE image.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1303

- **Cause:** See the message printed with the id.  
- **Message:** `image is PE32 (32-bit); only PE32+ (64-bit) images are supported`  
- **What to do:** Chameleon targets 64-bit processes; build the target for x64.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1304

- **Cause:** See the message printed with the id.  
- **Message:** `unknown optional header magic 0x%X`  
- **What to do:** The image is corrupt or is not a PE32+ executable.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1305

- **Cause:** See the message printed with the id.  
- **Message:** `optional header (size %u at offset 0x%zX) is smaller than the 112-byte PE32+ header or extends past the %zu-byte image`  
- **What to do:** The image is truncated or corrupt.  
  Other variants of the advice: The module is not a normally mapped PE image.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1306

- **Cause:** See the message printed with the id.  
- **Message:** `section table (%u sections at offset 0x%zX) extends past the end of the %zu-byte image`  
- **What to do:** The image is truncated or corrupt.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1307

- **Cause:** See the message printed with the id.  
- **Message:** `RVA 0x%X is outside the %zu-byte mapped image`  
- **What to do:** The image is corrupt.  
  Other variants of the advice: The image is truncated.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1308

- **Cause:** See the message printed with the id.  
- **Message:** `%llu bytes at RVA 0x%X extend past the end of the containing section or image`  
- **What to do:** The image is corrupt or truncated.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1309

- **Cause:** See the message printed with the id.  
- **Message:** `string at RVA 0x%X is not NUL-terminated within its section`  
- **What to do:** The image is corrupt or truncated.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1310

- **Cause:** See the message printed with the id.  
- **Message:** `export name #%u uses ordinal index %u but the export address table has only %u entries`  
- **What to do:** The image is corrupt.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1311

- **Cause:** See the message printed with the id.  
- **Message:** `import directory has more than 65536 descriptors`  
- **What to do:** The image is corrupt.  
  Other variants of the advice: Relink without /BIND (or unbind the image) so the import name table is present.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1312

- **Cause:** See the message printed with the id.  
- **Message:** `CodeView debug entry has no readable data (no mapped RVA and no valid file pointer)`  
- **What to do:** The image is corrupt or its debug data was stripped.  
  Other variants of the advice: The image is corrupt.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1313

- **Cause:** Cml_pe: debug directory present but no RSDS CodeView record.  
- **Message:** `no RSDS CodeView record found; the only CodeView record has signature 0x%08X (only RSDS / PDB 7.0 is supported)`  
- **What to do:** Link with /DEBUG so the linker emits an RSDS record (MSVC link and lld-link both do).  
- **Raised by:** `core/cml_pdb/src/detail/pe_codeview.cpp`

### CML1314

- **Cause:** See the message printed with the id.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1315

- **Cause:** See the message printed with the id.  
- **Message:** `load config directory has fewer than 4 readable bytes`  
- **What to do:** The image is corrupt or truncated.  
  Other variants of the advice: The image is corrupt.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1316

- **Cause:** See the message printed with the id.  
- **Message:** `TLS directory is %u bytes; IMAGE_TLS_DIRECTORY64 needs 40`  
- **What to do:** The image is corrupt.  
- **Raised by:** `core/cml_pe/src/pe.cpp`

### CML1318

- **Cause:** See the message printed with the id.  
- **Message:** `module base address is null`  
- **What to do:** Pass the handle of a loaded module.  
- **Raised by:** `core/cml_pe/src/pe.cpp`


## CML1400 to CML1499

### CML1400

- **Cause:** File missing, unreadable or cannot be mapped.  
- **Message:** `Cannot open '<...>': <...> (os error <...>).`  
- **What to do:** Check that the file exists, is readable and is not locked exclusively by another process.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1401

- **Cause:** Too small / wrong magic.  
- **Message:** `'<...>' is empty.`  
- **What to do:** Point Chameleon at the .pdb the linker produced.  
  Other variants of the advice: Point Chameleon at the .pdb file the linker produced.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1402

- **Cause:** Old MSF 2.00 or invalid super block.  
- **Message:** `is an old MSF 2.00 PDB (Visual C++ 6 or earlier).`  
- **What to do:** Only MSF 7.00 PDBs (Visual Studio 2002 and later, clang-cl) are supported.  
  Other variants of the advice: The file is damaged; rebuild the module so the linker regenerates the PDB.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1403

- **Cause:** File shorter than the super block claims.  
- **Message:** `is truncated: the super block describes <...> bytes but the file has <...>.`  
- **What to do:** Copy the PDB again or rebuild the module; the file was cut short.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1404

- **Cause:** See the message printed with the id.  
- **Message:** `The PDB stream directory of <...> is corrupt (<...>).`  
- **What to do:** Rebuild the module so the linker regenerates the PDB; if it was copied, copy it again.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1405

- **Cause:** See the message printed with the id.  
- **Message:** `The DBI stream of <...> is invalid (<...>).`  
- **What to do:** Rebuild the module so the linker regenerates the PDB. /DEBUG:FASTLINK PDBs are not supported.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1406

- **Cause:** See the message printed with the id.  
- **Message:** `The module table of <...> ends inside a module record.`  
- **What to do:** Rebuild the module so the linker regenerates the PDB.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1407

- **Cause:** See the message printed with the id.  
- **Message:** `The PDB info stream of '<...>' is missing or too short.`  
- **What to do:** Rebuild the module so the linker regenerates the PDB.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1408

- **Cause:** PE has no RSDS debug record.  
- **Message:** `The PE image has no CodeView (RSDS) debug record.`  
- **What to do:** Link with /DEBUG so the image records its PDB; PDB 2.0 (NB10) records are not supported.  
  Other variants of the advice: Link with /DEBUG so the image records its PDB.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1409

- **Cause:** PE headers malformed / out of bounds.  
- **Message:** `The PE image is malformed: <...>.`  
- **What to do:** Check that the file is a complete, unmodified .exe or .dll.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1410

- **Cause:** See the message printed with the id.  
- **Message:** `No usable PDB for '<...>' (recorded path '<...>'; tried <...>).`  
- **What to do:** Put the PDB next to the image, or add its directory to the PDB search paths.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1411

- **Cause:** GUID / age differ from the PE.  
- **Message:** `'<...>' is <...> but '<...>' was linked against <...> (GUID/age mismatch).`  
- **What to do:** Use the PDB produced by the same link as the image; rebuild if the image and PDB are out of sync.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1413

- **Cause:** See the message printed with the id.  
- **Message:** `The symbols of module '<...>' in '<...>' are unreadable.`  
- **What to do:** Rebuild the module so the linker regenerates the PDB.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1414

- **Cause:** See the message printed with the id.  
- **Message:** `The section contribution table of <...> has unknown version <...>.`  
- **What to do:** Rebuild the module with a supported linker (MSVC link.exe or lld-link).  
  Other variants of the advice: Rebuild the module so the linker regenerates the PDB.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1415

- **Cause:** Publics / globals record stream.  
- **Message:** `The global symbol record stream of '<...>' is corrupt (a record leaves the stream).`  
- **What to do:** Rebuild the module so the linker regenerates the PDB.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1416

- **Cause:** See the message printed with the id.  
- **Message:** `The source-info table of <...> lists more modules than the module table.`  
- **What to do:** Rebuild the module so the linker regenerates the PDB.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1417

- **Cause:** See the message printed with the id.  
- **Message:** `proc_info needs a proc symbol of a compiland of this index.`  
- **What to do:** Pass a symbol returned by find_proc_by_rva.  
  Other variants of the advice: Use an index below PdbIndex::compiland_count().  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1418

- **Cause:** a 32-bit offset / count would overflow.  
- **Message:** `The module names of <...> exceed 4 GiB.`  
- **What to do:** This PDB is too large for the index; split the module.  
  Other variants of the advice: This PDB is too large for the index.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1419

- **Cause:** IPI stream (S_BUILDINFO fallback) malformed.  
- **Message:** `The <...>TPI<...>IPI<...> stream of '<...>' is invalid (<...>); <...>function types are unavailable.<...>build information is unavailable.`  
- **What to do:** Rebuild the module so the linker regenerates the PDB.  
  Other variants of the advice: Rebuild the module so the compiler and linker regenerate the debug info.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1430

- **Cause:** The TPI or IPI stream (the type records) of a PDB is missing or its header is invalid. The bind planner reads types only to tell same-named static functions and lambda-local statics apart, so this shows up as the reason inside a `CML3214` or `CML3207` message, never on its own.  
- **Message:** `The <...>TPI<...>IPI<...> stream of '<...>' is unusable (<...>); type information is unavailable.`  
- **What to do:** Rebuild the module so the linker regenerates the PDB.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1431

- **Cause:** A type record needed for a function's signature is malformed, missing, of a kind the reader does not know, or nested too deeply. The signature is then unknown and nothing is paired by it (the reason appears inside `CML3214` / `CML3207`).  
- **Message:** `type <...> is not in the type records` (other variants name the record and what is wrong with it)  
- **What to do:** Recompile the translation unit; if the message names a record kind, report it with the compiler version.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1432

- **Cause:** The patch object carries no usable type records: it has no `.debug$T` section (compiled without debug information), the section is not CodeView C13, or it only refers to types kept elsewhere.  
- **Message:** `The object's .debug$T section is not a CodeView C13 type section.`  
- **What to do:** Compile the translation unit with /Z7 (or /Zi) so the object carries type information.  
  Other variants of the advice: Read the types from that PDB, or compile the translation unit with /Z7.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1433

- **Cause:** A `.debug$S` section of the patch object is malformed, or a function has no procedure record in it, so the function's type is not known.  
- **Message:** `An object's .debug$S section is malformed (<...>).`  
- **What to do:** Recompile the translation unit.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`

### CML1434

- **Cause:** The patch object was compiled with /Zi and names a compiler PDB for its types, but the file at that path belongs to another compilation (its GUID differs).  
- **Message:** `the type records of <...> cannot be read: its types are in the compiler PDB '<...>', but the file there is <...> and the object was compiled against <...>`  
- **What to do:** Reload again (the compiler PDB was rewritten in between), or build the project with /Z7.  
- **Raised by:** `core/cml_pdb/include/cml/pdb/diag_ids.h`


## CML1500 to CML1599

### CML1500

- **Cause:** See the message printed with the id.  
- **Message:** `cannot read response file '<...>'`  
- **What to do:** Check that the response file recorded in the build still exists.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML1501

- **Cause:** See the message printed with the id.  
- **Message:** `the command line expands to more than <...> tokens`  
- **What to do:** Remove the runaway @file reference from the recorded command line.  
  Other variants of the advice: Remove the deeply nested @file reference from the recorded command line. / Remove the runaway @file references from the recorded command line.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML1502

- **Cause:** Not cl, clang-cl or a cc1 invocation.  
- **Message:** `compiler '<...>' is neither cl, clang-cl nor a clang -cc1 command`  
- **What to do:** Chameleon can recompile code built by MSVC cl or clang-cl; rebuild with one of them.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML1503

- **Cause:** The command names no source file.  
- **Message:** `the recorded command line names no source file`  
- **What to do:** Provide the source path (compile_commands.json entry or a PDB with a source record).  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML1504

- **Cause:** /clr, /ZW compilands.  
- **Message:** `Compiland <...> is built with /clr or /ZW; managed and WinRT code cannot be hot-reloaded.`  
- **What to do:** Hot-reload only native compilands.  
- **Raised by:** `broker/cml_core/include/cml/core/module_audit.h`

### CML1505

- **Cause:** Compile_commands.json malformed.  
- **Message:** `compile_commands.json is malformed at byte <...>`  
- **What to do:** Regenerate compile_commands.json (CMake: -DCMAKE_EXPORT_COMPILE_COMMANDS=ON).  
  Other variants of the advice: Split the project or generate a smaller compilation database.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML1506

- **Cause:** See the message printed with the id.  
- **Message:** `cannot read '<...>'`  
- **What to do:** Check the compile_commands.json path (CMake: -DCMAKE_EXPORT_COMPILE_COMMANDS=ON).  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML1507

- **Cause:** See the message printed with the id.  
- **Message:** `compile_commands.json entry for '<...>' has no command`  
- **What to do:** Regenerate compile_commands.json.  
  Other variants of the advice: Add the file to the build so that compile_commands.json lists it, then regenerate it.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML1510

- **Cause:** See the message printed with the id.  
- **Message:** `'<...>' was not found in the given directories or on PATH`  
- **What to do:** Run from a developer command prompt (vcvars64.bat) or add the toolchain's bin directory to PATH.  
  Other variants of the advice: Pass the full path of the tool. / Check the path of the tool.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML1511

- **Cause:** See the message printed with the id.  
- **Message:** `'<...>' has no version resource`  
- **What to do:** Check that the path names the compiler or linker binary.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML1520

- **Cause:** Build-flag detection: object file not usable.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`


## CML1600 to CML1699

### CML1601

- **Cause:** Functions without 6 bytes of padding before them.  
- **Message:** `Could not check whether the functions of <...> have hot-patch padding (no function could be sampled); /FUNCTIONPADMIN and /hotpatch are recommended.`  
- **What to do:** Compile with /hotpatch and link with /FUNCTIONPADMIN.  
- **Raised by:** `broker/cml_core/include/cml/core/module_audit.h`

### CML1602

- **Cause:** /OPT:REF or /OPT:ICF in effect.  
- **Message:** `Could not determine whether <...> was linked with /OPT:NOREF /OPT:NOICF (the PDB records no linker command line); they are recommended.`  
- **What to do:** Link with /OPT:NOREF /OPT:NOICF.  
- **Raised by:** `broker/cml_core/include/cml/core/module_audit.h`

### CML1603

- **Cause:** Optimised compilands with inlining on.  
- **Message:** `of <...> compilands of <...> are optimised with inlining (<...>); copies of a changed function inlined elsewhere keep the old code (CML3104).`  
- **What to do:** Build iterated modules with /Ob0 or /Od.  
- **Raised by:** `broker/cml_core/include/cml/core/module_audit.h`


## CML2100 to CML2199

### CML2102

- **Cause:** /Yc TU (clang: -emit-pch / -building-pch-with-obj) changed.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`


## CML2200 to CML2299

### CML2200

- **Cause:** The compiler ran and reported errors.  
- **Message:** `compiling '<...>' failed with exit code <...>`  
- **What to do:** Fix the errors shown in the compile output and save again.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML2201

- **Cause:** Warning: recorded compiler missing, using another.  
- **Message:** `the recorded compiler '<...>' does not exist; using '<...>'`  
- **What to do:** Rebuild the project with the installed toolchain to remove this warning.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML2202

- **Cause:** CreateProcess failed / no compiler anywhere.  
- **Message:** `the recorded compiler '<...>' does not exist`  
- **What to do:** Reinstall the toolchain the project was built with.  
  Other variants of the advice: Run the broker from a developer command prompt or reinstall the toolchain. / Close other programs or retry.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML2203

- **Cause:** See the message printed with the id.  
- **Message:** `compiling '<...>' was cancelled`  
- **What to do:** Save the file again to restart the compile.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML2204

- **Cause:** See the message printed with the id.  
- **Message:** `compiling '<...>' did not finish within <...> ms`  
- **What to do:** Raise the compile timeout or check for a stuck compiler process.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML2205

- **Cause:** Exit code 0 but the object is missing.  
- **Message:** `the compiler succeeded for '<...>' but '<...>' does not exist`  
- **What to do:** Check the compile output; the command line may redirect the object elsewhere.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML2206

- **Cause:** Output directory / response file failed.  
- **Message:** `cannot create the output directory for '<...>'`  
- **What to do:** Check the session temp directory is writable.  
  Other variants of the advice: Check that the temp directory is writable. / Check that the disk is not full.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML2210

- **Cause:** Warning: /GL (or -flto) removed for the patch compile.  
- **Message:** `/GL objects are IL, not COFF; the patch object is compiled without whole-program optimisation (/GL-)`  
- **What to do:** clr.  
  Other variants of the advice: clang:-flto(=.*)? / clang:-gcodeview-command-line  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`

### CML2211

- **Cause:** Warning: private compiler PDB copy could not be made.  
- **Message:** `cannot copy the compiler PDB '<...>' for the PCH-using TU`  
- **What to do:** Build with /Z7 so no compiler PDB is shared with the patch compile.  
  Other variants of the advice: If the compile fails with C2859, build the project with /Z7 or set PchDebugMode::DropPch.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/diag_ids.h`


## CML3100 to CML3199

### CML3101

- **Cause:** Image-relative reference to old-module data that is not a proved address displacement (WP-36 narrowed it). The ordinary case, `[reg + index*scale + sym@IMGREL]` in a function that keeps `__ImageBase` in a register (a switch with a jump table, two or more global arrays indexed), is retargeted and does not raise this. What remains: the reference is in a data section (a table of image-relative offsets to old data), it is an immediate operand, its instruction is RIP-relative or has a `67` prefix or an `fs`/`gs` override, its memory operand has no base register (`[index*scale+disp32]`, `[disp32]`), it is a `lea` with a 32-bit or 16-bit destination (the address would be truncated), or the instructions between the start of the function and the site cannot be decoded (an instruction the decoder does not know, or a relocation that does not fit one instruction's operand: data inside the function). The message names the offset and the instruction bytes.
- **Message:** `<...> references <...> in <...> image-relatively (ADDR32NB) at +<...> of section '<...>' in <...>, and the site is not proved to be an address displacement: <...>; this cannot be retargeted`  
- **What to do:** Move the reference behind a function or pointer, or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3102

- **Cause:** Thread-local reference shape that cannot be retargeted (WP-24 narrowed it).  
- **Message:** `'<...>' is a thread_local in the old image but <...> defines it as ordinary data; storage of a different kind cannot be bound`  
- **What to do:** Keep the thread_local keyword, or hot-restart.  
  Other variants of the advice: Move the thread_local to an unchanged source file, or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3104

- **Cause:** Per function, raised at reload time.  
- **Message:** `Compiland <...> of <...> is optimised with inlining and <...> small changed function(s) (<...>) may be inlined into their callers: a caller that is still executing, or whose own code did not change, keeps the old code.`  
- **What to do:** Build iterated modules with /Ob0 or /Od, or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/module_audit.h`


## CML3200 to CML3299

### CML3200

- **Cause:** Undefined symbol found nowhere.  
- **Message:** `unresolved external symbol <...> referenced from <...> in`  
- **What to do:** Define it in a module of the process (or link the library that provides it into the patched module), or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3201

- **Cause:** no compiland of the module matches the TU.  
- **Message:** `compiland index <...> is out of range in`  
- **What to do:** Pass the index of the compiland that was compiled from the patched source file.  
  Other variants of the advice: Check that the module was built from this source file with debug info (/Zi or /Z7), or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3202

- **Cause:** Relocation kind that cannot be retargeted.  
- **Message:** `unsupported relocation <...> against <...> from <...> in`  
- **What to do:** Compile with /MD-compatible 64-bit code models (no 32-bit absolute addresses), or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3203

- **Cause:** Not an x64 object / malformed for planning.  
- **Message:** `object <...> is not an x64 object`  
- **What to do:** Compile the patch for x64.  
  Other variants of the advice: This is a programming error in the caller.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3204

- **Cause:** Fix-up needed in a section without locator.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3205

- **Cause:** REL32 displacement does not fit 32 bits.  
- **Message:** `the <...> displacement to '<...>' does not fit 32 bits: site <...>, target <...> (section '<...>' + <...>)`  
- **What to do:** Reserve the patch within +-2 GiB of the module (the loader does this), or hot-restart.  
  Other variants of the advice: Hot-restart; this is not expected for a real module. / Access the data through a function or pointer in the module, or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3206

- **Cause:** Patch layout lacks a section or symbol.  
- **Message:** `the patch layout has no RVA for the slot symbol '<...>'`  
- **What to do:** Look the symbol up as a public in the patch PDB (it is defined by binding.obj).  
  Other variants of the advice: This is a bug in the bind planner; report it with the plan JSON. / Link the patch with /OPT:NOREF /OPT:NOICF /DEBUG:FULL (WP-09 does this).  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3207

- **Cause:** Static matches several old symbols. The reason follows a colon in the message. Same-named function-local statics of several lambdas (a macro such as Godot's `SNAME`) are told apart by their lambda as long as the function has the same lambdas as before; when a lambda with such a static was added or removed, the compiler's lambda numbers may have shifted. Several statics of one name in one function (declared in different blocks) are placed by the layout of their object section where that is known; where it is not (under cl every function-local static is a section of its own) they are matched by their place in the function (with the warning `CML3570`) only when the function has the same number of them as before, each has the same type and sits in the same block; otherwise this error names what differs, also when the guard variable of such a static has no record in the old image (clang-cl records none): the patch would then construct the old object a second time. The message also says so when the debug records needed for either could not be read (`CML1430` to `CML1434`). Since WP-40 also for a function-local static of a function `f` itself whose name is the name of a static of one of `f`'s lambdas, in the patch or in the running program: a compiler records the static of an inlined lambda in the function it was inlined into, under the same name, so a record of that name in `f` cannot be proven to be this static's (for instance after an inlined lambda was deleted and `f` got a static of its static's name).  
- **Message:** `static data '<...>' (<...>) matches <...> records named '<...>' in compiland <...> but the patch object defines <...> of them<...>`  
- **Message:** `static data '<...>' (<...>) in <...> cannot be paired with the <...> record(s) named '<...>' of its function in compiland <...>: <...>, and a record of that name in the function may be that lambda's (a compiler records the static of an inlined lambda in the function it was inlined into), so none is provably this static's`  
- **What to do:** Rename one of the statics, keep their number unchanged, or hot-restart.  
  Other variants of the advice: Rename the static (or the lambda's static), or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3208

- **Cause:** Relocation site outside its section.  
- **Message:** `relocation at offset <...> of section '<...>' in <...> lies outside the section data`  
- **What to do:** Rebuild the object with the compiler; the file is damaged.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3209

- **Cause:** Several compilands match the TU name.  
- **Message:** `translation unit '<...>' matches several compilands of <...>`  
- **What to do:** Pass the full object path or the compiland index.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3210

- **Cause:** TU-wide initialiser may re-run initialisers of bound globals.  
- **Message:** `initialiser '<...>' initialises every global of <...>, including ones bound to the old image; running it may re-initialise them`  
- **What to do:** Put new globals with dynamic initialisation into a separate source file, or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3211

- **Cause:** Function-local static guard not found in the old image (warning). Since WP-40 only for a guard whose block has no static in the patch at all; a guard of a static bound to the running process that cannot be found is the error `CML3600`, and the guard of a new static is new without a warning.  
- **Message:** `guard variable '<...>' of a function-local static was not found in the old image; the patch gets a fresh guard`  
- **What to do:** Keep the layout of the statics in this source file unchanged, or hot-restart if the static must not be initialised again.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3212

- **Cause:** Reference through a section symbol into bound data.  
- **Message:** `object section <...> ('<...>') holds data bound to the old image and is also referenced through its section symbol (first from <...>); those references stay on the patch's dead copy`  
- **What to do:** Give the data external linkage or avoid private (section-relative) references to it.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3214

- **Cause:** Same-scope static overloads cannot be told apart. Overloads are paired by their signature from the debug type records, so this is left for the cases named after a colon in the message: several functions of that name and scope have the same signature; no signature of the patch occurs in the old image; the call operators of lambdas whose function no longer has the same lambdas as before (a lambda is known only by the number the compiler gives it, and adding or removing one shifts the numbers); or the type records cannot be read (`CML1430` to `CML1434`: no debug information in the patch object, a compiler PDB that is missing, types that live in a precompiled header object, a malformed record).  
- **Message:** `ambiguous overloads of static '<...>' (<...>) in <...>; cannot tell which old function each new one replaces (<...> in the patch, <...> records named '<...>' in compiland <...>)<...>`  
- **What to do:** Rename one overload, or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3215

- **Cause:** Dynamic initialiser without a global of the TU.  
- **Message:** `dynamic initialiser '<...>' (<...>) in <...> could not be matched to a new global`  
- **What to do:** Report the symbol names; as a workaround move the global with dynamic initialisation to an unchanged source file, or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`


## CML3300 to CML3399

### CML3300

- **Cause:** a count, offset or size does not fit its COFF field.  
- **Message:** `adding anchors would push the symbol table past 2^32 records`  
- **What to do:** Split the module into smaller objects.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3301

- **Cause:** Regular object with more than 65279 sections.  
- **Message:** `object already has %zu sections; a %s object cannot hold more<...>bigobj<...>regular`  
- **What to do:** Use the /bigobj format or split the module.  
  Other variants of the advice: Write the object in the /bigobj format.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3302

- **Cause:** Symbol refers to a missing section / bad aux data.  
- **Message:** `symbol name is empty`  
- **What to do:** Give the symbol a non-empty name (an all-zero name field means string-table offset 0, which is invalid).  
  Other variants of the advice: Give the symbol a non-empty name. / Fix the symbol's aux records.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3303

- **Cause:** Relocation names a symbol index that does not exist.  
- **Message:** `section %zu '%s' has a relocation at offset %u naming symbol index %u but the table has %llu records`  
- **What to do:** Relocations must name existing symbol indices; never remove or reorder symbols.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3304

- **Cause:** Object cannot be reproduced losslessly (overlapping regions ...).  
- **Message:** `section %u '%.*s' has %u line numbers but line-number pointer %u (one of them is zero): no table to reproduce`  
- **What to do:** The object is corrupt; rebuild it.  
  Other variants of the advice: The object cannot be rewritten losslessly; rebuild it with the compiler.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3305

- **Cause:** Section name cannot be encoded.  
- **Message:** `section name is empty or contains a NUL byte`  
- **What to do:** Give the section a non-empty name.  
  Other variants of the advice: Add fewer long-named sections or shorten symbol names.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3306

- **Cause:** Object already has a __cml_anchor_<n> symbol.  
- **Message:** `object already contains the symbol '<...>'; it looks anchored already`  
- **What to do:** Anchor the compiler's original object, not an already anchored one.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3307

- **Cause:** Anchor tag with other characters than [A-Za-z0-9_].  
- **Message:** `anchor tag '<...>' contains a character other than letters, digits and '_'`  
- **What to do:** Use only letters, digits and '_' in the tag.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3308

- **Cause:** Two objects of one link got the same anchor tag.  
- **Message:** `anchor tag '<...>' is used by two objects of one link`  
- **What to do:** Give every object of a link its own tag (for example its compiland index).  
  Other variants of the advice: Give every object of a link its own tag (for example its compiland index); identical objects cannot be linked twice.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3309

- **Cause:** a section without locator was asked for one.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3310

- **Cause:** Empty / NUL-containing / linker-provided bind symbol name.  
- **Message:** `bind symbol name is empty or contains a NUL byte`  
- **What to do:** Pass the decorated linker name of the symbol to bind.  
  Other variants of the advice: Leave __ImageBase out of the bind list; the linker resolves it itself.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3311

- **Cause:** The same name (or a derived slot name) twice.  
- **Message:** `symbol '<...>' is listed twice for binding`  
- **What to do:** List each symbol once.  
  Other variants of the advice: Do not bind symbols whose names start with __cml_slot_.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3312

- **Cause:** Binding object would exceed a COFF limit.  
- **Message:** `binding object would exceed the COFF size limits (<...> functions, <...> data symbols)`  
- **What to do:** Bind fewer symbols per patch.  
- **Raised by:** `core/cml_coff/include/cml/coff/diag_ids.h`

### CML3350

- **Cause:** The linker ran and reported errors.  
- **Message:** `linking '<...>' failed (exit code <...>)<...>`  
- **What to do:** See the linker output in the log.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3351

- **Cause:** no usable link.exe / lld-link.  
- **Message:** `'<...>' is not a linker`  
- **What to do:** Use link or lld-link.  
  Other variants of the advice: Check the linker path in the configuration. / Run from a Visual Studio developer environment or set the linker path.<...>Install LLVM (lld-link.exe) or set the linker path.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3352

- **Cause:** CreateProcess failed.  
- **Message:** `cannot start the linker '<...>'`  
- **What to do:** Check that the linker exists and can be run.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3353

- **Cause:** See the message printed with the id.  
- **Message:** `linking '<...>' was cancelled`  
- **What to do:** Save the file again to restart the patch.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3354

- **Cause:** See the message printed with the id.  
- **Message:** `linking '<...>' did not finish within <...> ms`  
- **What to do:** Check the linker output and for a hung mspdbsrv.exe.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3355

- **Cause:** Exit code 0 but the DLL or PDB is missing.  
- **Message:** `the linker succeeded but '<...>' or '<...>' was not written`  
- **What to do:** Check the linker output in the log.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3356

- **Cause:** LNK2001 / LNK2019 / lld "undefined symbol".  
- **Message:** `unresolved symbol '<...>' while linking '<...>'<...> (+<...> more)<...>`  
- **What to do:** The bind list is missing this symbol, or the patch references something that is neither defined nor bound.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3357

- **Cause:** Warning: the linker printed a warning.  
- **Message:** `the linker warned`  
- **What to do:** Usually harmless; see the linker output in the log.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3358

- **Cause:** Output directory / response file failed.  
- **Message:** `cannot create the output directory '<...>'`  
- **What to do:** Check that the session temp directory is writable.  
  Other variants of the advice: Check that the session temp directory is writable and not full.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3360

- **Cause:** /OPT:REF, /OPT:ICF, /FORCE, /INCREMENTAL, ...  
- **Message:** `the linker option '<...>' is not allowed for a patch`  
- **What to do:** Remove it; section locators need /OPT:NOREF /OPT:NOICF and a complete, fixed-address link (ADR-0024).  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3361

- **Cause:** an option the driver owns (/OUT, /BASE, /PDB, /DLL, ...).  
- **Message:** `the linker option '<...>' is set by the patch linker driver`  
- **What to do:** Remove it; use the patch request's fields (output name, base) instead.  
  Other variants of the advice: Remove it; the driver always passes /NODEFAULTLIB.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3362

- **Cause:** no objects, empty output name, base not aligned, ...  
- **Message:** `cannot expand a response file in the extra linker arguments`  
- **What to do:** Check that the response file exists and is readable.  
  Other variants of the advice: Use link or lld-link. / Pass the patch objects.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3365

- **Cause:** Neither PE nor PDB tells which linker built the module.  
- **Message:** `the linker override '<...>' is not a linker`  
- **What to do:** Use 'link' or 'lld-link'.  
  Other variants of the advice: Choose the linker explicitly (link or lld-link).  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3366

- **Cause:** Warning: the PE and the PDB disagree.  
- **Message:** `the PDB says the module was linked by <...> but the PE says <...>; using the PDB`  
- **What to do:** If the PDB belongs to another build, rebuild the module; or choose the linker explicitly.  
  Other variants of the advice: Provide the module's PDB or choose the linker explicitly.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3367

- **Cause:** PE or PDB of the module unreadable.  
- **Message:** `the PDB of '<...>' could not be opened`  
- **What to do:** The linker is detected from the PE alone; check that the PDB is next to the module.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3370

- **Cause:** See the message printed with the id.  
- **Message:** `cannot open the patch PDB '<...>'`  
- **What to do:** Check that the linker wrote the PDB (/DEBUG:FULL /PDB:...).  
  Other variants of the advice: Check that the linker wrote a complete PDB.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3371

- **Cause:** See the message printed with the id.  
- **Message:** `cannot read the patch DLL '<...>'`  
- **What to do:** Check that the linker produced the DLL and that it is a valid x64 image.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3372

- **Cause:** a section locator is not a public of the patch PDB.  
- **Message:** `the locator '<...>' of section <...> of '<...>' is not a public of the patch PDB`  
- **What to do:** Link with /OPT:NOREF /OPT:NOICF and pass the anchored object (coff::add_anchors).  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3373

- **Cause:** a __cml_slot_<name> / data placeholder is not a public.  
- **Message:** `the <...>slot '<...>thunk '<...>' is not a public of the patch PDB '<...>'`  
- **What to do:** Link binding.obj with the patch and make sure the symbol is in the bind list.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3374

- **Cause:** a new function is not a public of the patch PDB.  
- **Message:** `the function '<...>' is not a public of the patch PDB '<...>'`  
- **What to do:** Check that the patch object defines it with external linkage.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`

### CML3375

- **Cause:** The DLL's ImageBase is not the requested /BASE.  
- **Message:** `the patch DLL '<...>' has ImageBase <...> instead of the requested`  
- **What to do:** Link with /FIXED /BASE at a free address; do not pass options that change the base.  
- **Raised by:** `core/cml_toolchain/include/cml/toolchain/link_diag_ids.h`


## CML3400 to CML3499

### CML3400

- **Cause:** Thread_local the old image does not have (error).  
- **Message:** `the initialisation guard '<...>' of a function-local thread_local in <...> was not found in the old image (the thread_local is new, or the layout of the thread-locals of this source file changed); patch-local thread-local storage is not supported`  
- **What to do:** Hot-restart, or keep the thread_locals of this source file unchanged.  
  Other variants of the advice: Hot-restart, or move the new thread_local to an unchanged source file (and rebuild).  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3401

- **Cause:** Dynamic initialiser/destructor of a thread_local stays the old one (warning).  
- **Message:** `has thread_local variables with dynamic initialisation or a destructor (<...>); the old image's initialiser and destructor stay in effect (existing threads keep their values, new threads run the OLD initialiser), a changed initialiser is not run`  
- **What to do:** Hot-restart if the changed initialiser must take effect.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3402

- **Cause:** Several S_GTHREAD32 records for one name (error).  
- **Message:** `thread_local '<...>' (<...>) matches several S_GTHREAD32 records of the old image; cannot tell which one it is`  
- **What to do:** Rename one of the thread_locals, or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3450

- **Cause:** (warning) the edit compiled to identical code: nothing was loaded.  
- **Message:** `the edit compiled to identical code (<...> of <...> planned hooks are identical to the running code, nothing new): no patch was loaded for`  
- **What to do:** Nothing to do. Change code or data that reaches the generated code, or set-option narrow_hooks false to hook everything.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML3451

- **Cause:** (warning) identical code, but the initial value of data in the running image changed: nothing was loaded.  
- **Message:** `the edit only changes the initial value of data that lives in the running image (<...>): the code is identical, so no patch was loaded for <...> and the running program keeps its current values (initialisers of existing globals do not run again)`  
- **What to do:** Restart the application to pick the new initial value up, or assign the value from code that runs again (a function the patch can hook).  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`


## CML3500 to CML3599

### CML3500

- **Cause:** Writable static data whose identity cannot be established (error).  
- **Message:** `static data <...> in <...> has no source-level name the planner can match to the old image (object section '<...>'), so it cannot be bound to the running process's copy`  
- **What to do:** Give the variable an ordinary name, or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3550

- **Cause:** Image-relative displacement to old data does not fit 32 bits (error). The code addresses the data as `[reg + index*scale + sym@IMGREL]` with the register holding the patch's `__ImageBase`; the displacement `target - patch base` is a signed 32-bit field, and the data (usually in another module) is more than 2 GiB away from where the patch can be loaded.  
- **Message:** `image-relative reference to <...> (<...>, <...>) from <...> in <...> cannot reach it: it is more than 2 GiB away from every place the patch can be loaded`  
- **Message:** `the image-relative displacement to '<...>' does not fit 32 bits: patch base <...>, target <...> (section '<...>' + <...>)`  
- **What to do:** Access the data through a function or pointer in the module, or hot-restart.  
  Other variants of the advice: Reserve the patch within +-2 GiB of the module (the loader does this), or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3570

- **Cause:** (warning) Several function-local statics of one name in one function, declared in different blocks (`if (i == 0) { static const T names = ...; } else { static const T names = ...; }`), were matched to the running program's copies by their order in the function. Nothing else tells them apart: they have one name, one scope and one type, and the layout of their object section is not known (under cl every function-local static is a section of its own; where a layout is known it places them and this warning is not raised). The reload is applied; the function has the same number of them as before, each with the same type and in the same block. If the edit swapped two of those blocks or replaced one, the statics now hold each other's values.  
- **Message:** `Function-local statics '<...>' of '<...>' (<...>) are matched by their order in the function; if you reordered or replaced blocks that declare them, their values may be exchanged.`  
- **What to do:** Rename one of them, or hot-restart after reordering.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3600

- **Cause:** A guard variable (`$TSS0`, or cl's `$S1` under `/Zc:threadSafeInit-`) of a function-local static cannot be bound consistently with the statics of its block. A guard says whether its static was constructed: bound to the old guard while its static is new, the patch would use an object that was never constructed; given a fresh guard while its static is bound to the running process, the old object would be constructed a second time over live state. Raised (1) when a block has both statics bound to the running process and new ones, and a guard without a record in the old image (compilers number the guards of a function, so which guard is whose cannot be told; a new static declared in front of an old one takes the old one's number); (2) when every static of the block is bound but its guard has no record (clang-cl records none) and the layout of the object section cannot place it: no layout is known (no static there was matched, or the matched ones disagree, for instance after statics were reordered), or it would put the guard outside the translation unit's contribution to the old image, into another image section, or onto (or inside) a variable or guard the old image records (WP-46: a variable's extent is its type's size, or up to the next record when the size is unknown). A layout that only statics paired by their function teach (clang-cl, WP-46) is used only when at least two of them agree, or when it agrees with the layout learned from statics matched by name. Until WP-40 case (2) was the warning `CML3211` and the reload went on. (3) A bitmask guard `$S<n>` (cl `/Zc:threadSafeInit-`, clang-cl `-fno-threadsafe-statics`: one per function) hands out its bits to the function's dynamically initialised statics in the order they are reached. A static removed or made constant-initialised, or blocks reordered, shifts the bits of the rest, so the old bitmask is kept only when the code of its function is unchanged (compared with the running code, as for hook narrowing); otherwise, or when the function cannot be compared, the reload is refused. Edits to other functions of the same file are not affected.  
- **Message:** `the guard variable <...> in <...> guards statics of one block of which some are bound to the running process (<...>) and some are new (<...>); which guard belongs to which static cannot be told, so either an old static would be constructed again or a new one never`  
- **Message:** `the guard variable <...> in <...> cannot be bound to the old guard of <...>, which is bound to the running process: <...>; with a fresh guard the old object would be constructed a second time`  
- **Message:** `the bitmask guard '<...>' (no thread-safe statics) cannot be bound to the running program's: its bits go to the function's dynamically initialised statics in the order they are reached, which only an unchanged function proves to be as before, and <...>; with the old bitmask a static could be constructed again or never`  
- **What to do:** Declare the new static in a block of its own (or in another function), keep the statics of this source file in their order, or hot-restart.  
  Other variants of the advice: Keep the statics of this source file in their order (do not add or remove statics in front of them), or hot-restart. / Leave this function unchanged while the program runs (edit other functions of the file), or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3601

- **Cause:** clang-cl with optimisation (`/O2`) split an internal-linkage aggregate (a `static` or anonymous-namespace array or struct) into separate variables named `<name>.0`, `<name>.1`, ... The debug information of such a piece is a fragment, which CodeView does not record: neither the old PDB nor the patch object says where the old piece is. Until WP-40 the patch got fresh, zero-initialised pieces without a word, so the state of the aggregate was reset by every reload of its source file. A piece that the patch object's own debug records name is a whole variable (LLVM's uniquifier, or a piece that covers the whole aggregate) and is bound as usual.  
- **Message:** `static data <...> in <...> is piece <...> of '<...>', which the compiler split into separate variables; the debug information records no piece, so the old piece in the running process cannot be found and a reload would reset it`  
- **What to do:** Give the variable external linkage or take its address (the compiler then keeps it whole), or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3602

- **Cause:** (warning) Two or more lambdas of one function hold function-local statics and cannot be told apart: their call operators have the same signature (no captures, the same parameters and return type, as with Godot's `SNAME`), and the compiler's lambda numbers are their only names. Chameleon compares the code of each new call operator with each old one (strings and other constants they use by their bytes) and pairs the lambdas by that, also when they changed places. This warning means that did not settle it: the code of two or more of them changed, or a call operator could not be compared (the message says why), so those were paired by their numbers (where the old lambda of a number was taken by another new lambda, the reload is refused instead: `CML3603`). The reload is applied. If the edit reordered these lambdas, their statics now hold each other's values.  
- **Message:** `Lambdas <...> of '<...>' are matched by their numbers (<...>); if you reordered them, the values of their statics may be exchanged.`  
- **What to do:** Rename the statics, keep the order of these lambdas, or hot-restart after reordering.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3603

- **Cause:** Lambdas of one function that cannot be told apart by their types (see `CML3602`) were compared by the code of their call operators. A lambda whose code changed is matched by its number, but here the old lambda of that number is already another new lambda's (their code is the same): the changed lambda has no old lambda left that it can be said to be, and pairing it with one by guesswork could give its statics another lambda's values. The reload is refused.  
- **Message:** `Lambdas <...> of '<...>' cannot be matched to the running program's lambdas: their code changed, and the old lambda of their number is another new lambda's (by the code of its call operator), so the values of their statics would go to the wrong lambda`  
- **What to do:** Keep the order of these lambdas while you edit them, rename their statics, or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`

### CML3630

- **Cause:** The translation unit was compiled by cl with its legacy lambda processor (C++17 or earlier without `/Zc:lambda`, the default before `/std:c++20`), which names every lambda `<lambda_` plus a 32-digit hash, in no scope. The hash follows the lambda's own code: editing a lambda renames it, while edits elsewhere in the file keep every name. Chameleon pairs the lambdas by name, and the one lambda left unmatched on each side (the edited one) by elimination, and then proves the pairing by the code: every function of a name-matched lambda (its call operator, its invoker) must be the code of the old function of that name, with its statics at the old lambda's statics. So one edited lambda per reload works. Refused: two or more lambdas left on either side (two edited lambdas, or a lambda added or removed; after one reload that renamed a lambda, that lambda stays "left" until a restart, so a second lambda edited later counts as two), a name-matched lambda whose code changed next to an edited one, a lambda whose code cannot be compared (a function that is not hooked to an old one, or a lambda inlined into its caller so that only its static is left), and a lambda with external linkage and statics (in an inline function) that was renamed while another lambda with statics vanished (its statics would start again). Other lambdas with external linkage pair by their public name like any other function. Pairing any of these by name or by guess could run one lambda's code, or give its statics, for another. The reload is refused; the program keeps running unchanged.  
- **Message:** `Lambdas <...> in <...> are named by a hash (cl's legacy lambda processor) and were paired with the old lambdas of the same name, which only their code can confirm, and <...>; a wrong pairing would run one lambda's code, or give its statics, for another`  
- **Message:** `static data <...> in <...> belongs to lambda '<...>', which the compiler names by a hash; it was paired with the old static of that name, but no function of the lambda is hooked to an old one whose code could prove that it is the same lambda`  
- **Message:** `the lambdas of <...> are named by a hash of their own code (cl's legacy lambda processor), and after their names are matched <...> lambda(s) of the patch (<...>) and <...> of the old image (<...>) are left; only one changed lambda on each side can be paired (by elimination), so which old lambda (and which old statics) each new one is cannot be told`  
- **Message:** `the lambdas of <...> are named by a hash of their own code (cl's legacy lambda processor); the one left in the patch (<...>) and the one left in the old image (<...>) cannot be shown to be the same lambda: <...>` (the one lambda left on each side is paired only when both are held by the same function, as a named local variable of the closure's type, and have the same captures, the same statics and the same call operator signature; a lambda used without a named local, `[]{...}()`, cannot be shown to be the old one; and only when no user of the lambda has state that would be renamed with it: a static of a template instantiated over the lambda, such as `run<lambda>`, or a variable of the lambda's type, would otherwise be constructed again)  
- **What to do:** Change at most one lambda of this file per reload, compile with `/Zc:lambda` (the default under `/std:c++20`), or hot-restart.  
  Other variants of the advice: Compile with /Zc:lambda (or /std:c++20), keep the lambda's code unchanged, or hot-restart. / Change at most one lambda of this file per reload (adding or removing a lambda counts), compile with /Zc:lambda (the default under /std:c++20), or hot-restart.  
- **Raised by:** `broker/cml_core/include/cml/core/bind_diag_ids.h`


## CML4200 to CML4299

### CML4200

- **Cause:** Parameters unusable (zero/misaligned size, ...).  
- **Message:** `ReserveRange needs a module with a non-zero base and size`  
- **What to do:** Pass the base and size of the module being enabled.  
  Other variants of the advice: Use the defaults (4 blocks of 64 MiB) or a smaller block size. / Align the base and size to 64 KiB.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4201

- **Cause:** no free address space near the module.  
- **Message:** `module <...>+<...> leaves no address window that every REL32 can reach`  
- **What to do:** The module is too large for patching with 32-bit relative calls; see KNOWN_LIMITATIONS.  
  Other variants of the advice: Free address space near the module (fewer or smaller blocks) or reduce the block size. / Choose another range.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4202

- **Cause:** DLL loaded at a base other than the requested one.  
- **Message:** `loaded at <...> instead of`  
- **What to do:** The patch must be linked with /FIXED /BASE:<the reserved address>; relink it.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4203

- **Cause:** Range grabbed by someone else on every attempt.  
- **Message:** `attempt <...>: <...> could not be mapped at <...> (OS error <...>, the range was taken by another allocation<...>)`  
- **What to do:** The agent retries with the next reserved block; if every attempt fails, retry the reload.  
  Other variants of the advice: Retry the reload; if it keeps failing something in the process allocates aggressively near the module.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4204

- **Cause:** LoadLibraryExW failed for a reason other than a collision.  
- **Message:** `cannot open <...> (OS error <...>)`  
- **What to do:** Check that the patch DLL exists and can be read.  
  Other variants of the advice: Pass the linked patch DLL.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4205

- **Cause:** Image range not inside a block the agent reserved.  
- **Message:** `address <...> is not reserved address space (state <...>, type <...>)`  
- **What to do:** Reserve a block with ReserveRange first and place the patch inside it.  
  Other variants of the advice: Only blocks reserved through ReserveRange can be released for a patch. / Reserve a block with ReserveRange and link the patch at a base inside it.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4206

- **Cause:** Loaded image is not what the request described.  
- **Message:** `has ImageBase <...> and SizeOfImage <...> but the request announced base <...> and size`  
- **What to do:** Send the /BASE and the SizeOfImage of the DLL that was actually linked.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4207

- **Cause:** REL32 displacement does not fit 32 bits.  
- **Message:** `REL32 fix-up <...> at RVA <...> (site <...>) cannot reach target <...>: distance <...> does not fit 32 bits`  
- **What to do:** The patch was placed too far from the code it refers to; reserve address space nearer the module (or reload) so the patch lands within 2 GiB.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4208

- **Cause:** Fix-up site outside the image / misaligned / unwritable.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4209

- **Cause:** Initializer entry invalid or it faulted.  
- **Message:** `initializer <...> (table entry at RVA <...>) <...> after <...> initializer(s) had run`  
- **What to do:** Fix the crash in the new global's constructor; a patch whose load failed is unloaded and the old code keeps running, a live patch stays loaded.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4210

- **Cause:** no patch loaded at that base.  
- **Message:** `no patch is loaded at`  
- **What to do:** Only patches loaded by LoadPatch can be uncommitted.  
  Other variants of the advice: Send LoadPatch first; ApplyFixups and RunInitializers address a patch by its load base. / Send LoadPatch again; ApplyFixups and RunInitializers address a patch by its load base.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4211

- **Cause:** ReleaseRange: range not owned / OS refused.  
- **Message:** `VirtualQuery failed at <...> (error <...>)`  
- **What to do:** Report this with the agent log.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4212

- **Cause:** Another operation is using that patch.  
- **Message:** `the patch at <...> is being used by another request`  
- **What to do:** Wait for the previous request on this patch to be answered.  
  Other variants of the advice: The patch stays loaded; unload it again after the hook returned.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4213

- **Cause:** FreeLibrary failed; the image stays mapped.  
- **Message:** `FreeLibrary of the patch at <...> failed (OS error <...>)`  
- **What to do:** The patch stays mapped and its address range is lost; report this with the agent log.  
  Other variants of the advice: Something else loaded the same DLL; once it frees its reference, unload the patch again.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`

### CML4214

- **Cause:** VirtualQuery failed while scanning.  
- **Message:** `VirtualQuery failed at 0x<...> with error <...> while scanning the address space`  
- **What to do:** Retry; if it persists the process is in an unusual state - report it with the agent log.  
- **Raised by:** `core/cml_proc/include/cml/proc/loader_diag_ids.h`


## CML4300 to CML4399

### CML4300

- **Cause:** an instruction in the overwritten region is not understood.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4301

- **Cause:** < 5 bytes and no trailing padding.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4302

- **Cause:** The jump target is not within +-2 GB.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4303

- **Cause:** The function's bytes cannot be read.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4304

- **Cause:** a call ends inside the overwritten region.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4305

- **Cause:** a thread stayed inside the region for all retries.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4306

- **Cause:** Cannot enumerate threads / too many threads.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4307

- **Cause:** New threads kept appearing.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4308

- **Cause:** VirtualProtect failed.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4309

- **Cause:** Threads were stopped longer than the budget.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4310

- **Cause:** Null / equal / duplicate addresses in the request.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4311

- **Cause:** Revert of a function that has no hook.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4312

- **Cause:** The bytes are not the ones recorded or planned.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4313

- **Cause:** Not attempted: another hook of the request failed.  
- **Message:** `%s was not changed: another hook of the same request failed (%s).`  
- **What to do:** Fix the failing hook and send the request again.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4314

- **Cause:** a thread could not be suspended or inspected.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4315

- **Cause:** Request above the per-request limit.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`

### CML4316

- **Cause:** The bytes of two hooks overlap.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/diag_ids.h`


## CML4400 to CML4499

### CML4400

- **Cause:** ExceptionAction for a thread that is not held (answered already, or never held).  
- **Message:** `no thread is held for exception`  
- **What to do:** It was answered already, or its process went through a reconnect; query 'exceptions' for the open ones.  
- **Raised by:** `agent/src/crash/diag_ids.h`

### CML4401

- **Cause:** Leave function: the frame cannot be unwound (no unwind info and no readable return address).  
- **Message:** `cannot leave the function at <...>: it cannot be unwound (no unwind information and an unreadable stack top, or a damaged stack)`  
- **What to do:** Use Ignore, or Continue after a patch fixed the cause.  
- **Raised by:** `agent/src/crash/diag_ids.h`

### CML4402

- **Cause:** Leave function: the unwound frame does not look like a caller.  
- **Message:** `unwinding the faulting function did not lead to a caller (return address <...>)`  
- **What to do:** The stack looks damaged or the frame is not described by unwind data; use Ignore, or Continue after a patch.  
- **Raised by:** `agent/src/crash/diag_ids.h`

### CML4403

- **Cause:** More threads faulted at once than the handler has slots: the extra faults were passed on.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/crash/diag_ids.h`

### CML4404

- **Cause:** AddVectoredExceptionHandler failed: crash handling is off.  
- **Message:** `AddVectoredExceptionHandler failed (error <...>)`  
- **What to do:** Crash handling stays off; the application's faults are unaffected.  
- **Raised by:** `agent/src/crash/diag_ids.h`

### CML4405

- **Cause:** The handler's wait events could not be created: crash handling is off.  
- **Message:** `cannot create the events the crash handler waits on (error <...>)`  
- **What to do:** Close other programs to free system resources; crash handling stays off.  
- **Raised by:** `agent/src/crash/diag_ids.h`

### CML4450

- **Cause:** The command line, directory or environment could not be captured at start-up.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/restart_ids.h`

### CML4451

- **Cause:** Command line + environment do not fit one message.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/restart_ids.h`

### CML4452

- **Cause:** no memory to prepare the restart.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/restart_ids.h`

### CML4453

- **Cause:** a module could not be queued for enabling again after a restart.  
- **Message:** `queued <...> of <...> modules<...>queued <...> modules`  
- **What to do:** restart: %s to enable again.  
- **Raised by:** `agent/src/restart_ids.h`


## CML4600 to CML4699

### CML4600

- **Cause:** The stack of the held thread cannot be unwound to the chosen frame.  
- **Message:** `the re-execute recipe names frame <...> with <...> parameters`  
- **What to do:** Use Leave function or Ignore instead.  
- **Raised by:** `agent/src/crash/diag_ids.h`

### CML4601

- **Cause:** The chosen frame was entered with another RSP than the broker saw.  
- **Message:** `frame <...> was entered with RSP <...>, the recipe expected`  
- **What to do:** The stack changed since the fault was reported; use Leave function or Ignore instead.  
- **Raised by:** `agent/src/crash/diag_ids.h`

### CML4602

- **Cause:** The replacement function is not executable code.  
- **Message:** `the replacement function address <...> is not executable code`  
- **What to do:** Use Leave function or Ignore instead.  
- **Raised by:** `agent/src/crash/diag_ids.h`

### CML4603

- **Cause:** a parameter's location cannot be read (register not supported, memory unreadable).  
- **Message:** `parameter <...> is addressed from register <...>, which re-execute cannot read`  
- **What to do:** Use Leave function or Ignore instead.  
- **Raised by:** `agent/src/crash/diag_ids.h`

### CML4604

- **Cause:** The chosen frame's return address is not executable code (damaged stack).  
- **Message:** `the return address of frame <...> (<...>) is not executable code`  
- **What to do:** The stack looks damaged; use Ignore, or Continue after a patch.  
- **Raised by:** `agent/src/crash/diag_ids.h`

### CML4650

- **Cause:** See the message printed with the id.  
- **Message:** `the agent's reservation of address space near the module`  
- **What to do:** link.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4651

- **Cause:** See the message printed with the id.  
- **Message:** `the agent's load of the patch (after the DLL is mapped: it is unloaded again)`  
- **What to do:** hook-install.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4652

- **Cause:** See the message printed with the id.  
- **Message:** `hook installation, inside the window with the application's threads suspended`  
- **What to do:** post-patch-hooks.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4653

- **Cause:** See the message printed with the id.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4654

- **Cause:** ArmFault names an unknown stage / a stage of the broker.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4655

- **Cause:** The agent was built without CML_FAULT_INJECTION.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4656

- **Cause:** See the message printed with the id.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`


### CML4657

- **Cause:** Failure injection, stage `pre-restart-hooks`: the agent of the old process refused the preparation of a restart after the application's pre-restart hooks ran. The broker reports it as CML5804; the process is not touched.  
- **Message:** `injected failure at the stage 'pre-restart-hooks' (after <n> pre-restart hook(s) ran); the process is not restarted`  
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). The hooks ran: an application that tore state down in its pre-restart hook must cope with running on.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4658

- **Cause:** Failure injection, stage `revert-repoint`: a revert's re-pointing of hooks to an older patch failed inside the window with the application's threads suspended, before anything was written. The broker reports it as CML5851; nothing changed.  
- **Message:** `injected failure at the stage 'revert-repoint' (re-pointing hooks to an older patch, inside the window with the application's threads suspended); nothing was written`  
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). Revert again.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4659

- **Cause:** Failure injection, stage `revert-restore`: a revert's restoring of original bytes failed inside the window with the application's threads suspended, before anything was written. The broker reports it as CML5851 (and undoes a re-pointing it had made); nothing changed.  
- **Message:** `injected failure at the stage 'revert-restore' (restoring original bytes, inside the window with the application's threads suspended); nothing was written`  
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). Revert again.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4660

- **Cause:** Failure injection, stage `revert-uncommit`: the agent did not un-commit a reverted patch image. The revert itself went through; the broker reports the warning CML5855.  
- **Message:** `injected failure at the stage 'revert-uncommit' (the un-commit of the reverted patch at <base>); the patch stays committed, so its application hooks keep being collected`  
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). A later patch of the same functions replaces the hooks; nothing else to do.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`


## CML4700 to CML4799

### CML4661

- **Cause:** Failure injection, stage `exception-hold` (group `exception`, armed for the next fault the crash handler would hold): the agent's crash handler passed the fault on instead of holding the thread, as when every slot is taken (CML4403). The application's own exception handler, or the system, saw the fault as it would without Chameleon. The agent logs this warning; the broker publishes no `exception` event for that fault.
- **Message:** `injected failure at the stage 'exception-hold': <n> fault(s) were passed on instead of held (the application's own handler or the system saw them)`
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). An application without its own handler for the fault ends as it would without Chameleon. The next fault is held as usual.
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4662

- **Cause:** Failure injection, stage `exception-continue` (group `exception-action`, armed for the next `exception-action` of the process; at the end of that action it is disarmed whether it fired or not, and the answer lists an unused one in `injection_disarmed`): the agent refused the action `continue` before it touched the held thread. The thread stays held and `exceptions` still lists it.
- **Message:** `injected failure at the stage 'exception-continue' (the action continue for a held thread: refused before the thread is touched (it stays held))`
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). Send the action again.
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4663

- **Cause:** Failure injection, stage `exception-leave` (group `exception-action`, like CML4662): the agent refused the action `leave` before it touched the held thread. The thread stays held.
- **Message:** `injected failure at the stage 'exception-leave' (the action leave for a held thread: refused before the thread is touched (it stays held))`
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). Send the action again.
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4664

- **Cause:** Failure injection, stage `exception-reexecute` (group `exception-action`, like CML4662): the agent refused the action `reexecute` before it touched the held thread. The thread stays held; `reexecute` stays offered.
- **Message:** `injected failure at the stage 'exception-reexecute' (the action reexecute for a held thread: refused before the thread is touched (it stays held))`
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). Send the action again.
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML4700

- **Cause:** The agent refused an image-relative fix-up (kind Addr32NbDisp): `target - patch image base` does not fit a signed 32-bit displacement. Nothing was written; a patch that was being loaded is unloaded and the program keeps running the old code. The broker checks the same range before it sends the fix-up (CML3550), so this is only seen when the patch landed somewhere else than planned.  
- **Message:** `image-relative fix-up <...> at RVA <...> cannot reach target <...> from the patch image base <...>: distance <...> does not fit a signed 32-bit displacement`  
- **What to do:** The patch was placed too far from the data it refers to; reserve address space nearer the module (or reload) so the patch lands within 2 GiB.  
- **Raised by:** `agent/src/loader/fixup_diag_ids.h`


## CML5200 to CML5299

### CML5200

- **Cause:** Bad length / oversized frame.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5201

- **Cause:** Truncated, trailing bytes, bad count, bad UTF-8.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5202

- **Cause:** See the message printed with the id.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5203

- **Cause:** Unknown id or not implemented yet.  
- **Message:** `is not a loader request`  
- **What to do:** Use an agent and a broker from the same build.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5204

- **Cause:** Encoder: payload or string over the limit.  
- **Message:** `cannot send <...>: a string or the whole payload exceeds the protocol limit`  
- **What to do:** Shorten the path or text, or report it if this is a real-world size.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5205

- **Cause:** Connect failed (no pipe, access denied).  
- **Message:** `cannot connect to broker pipe '<...>': <...> (<...>)`  
- **What to do:** Start cml_broker, or set CML_PIPE_NAME to the pipe the broker listens on.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5206

- **Cause:** Read/write failed with an OS error.  
- **Message:** `accepting a control client failed (injected failure)`  
- **What to do:** Test hook: ControlServer::set_accept_failures_for_testing.  
  Other variants of the advice: Restart the application and the broker; if it persists, report it with the broker log. / Close other programs to free system resources and retry.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5207

- **Cause:** Server side: cannot create the pipe.  
- **Message:** `cannot create the broker's stop event`  
- **What to do:** Close other programs to free system resources and retry.  
  Other variants of the advice: Test hook: set_instance_create_failures_for_testing. / Stop the other broker or pass a different --pipe name.<...>Check the pipe name and that the process may create pipes.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5208

- **Cause:** Pipe name lacks the pipe-namespace prefix.  
- **Message:** `control pipe name '<...>' does not start with \\.\pipe\ or has no name part`  
- **What to do:** Set CML_CONTROL_PIPE (or --control-pipe) to a full name such as \\.\pipe\cml_control_test.  
  Other variants of the advice: Set CML_PIPE_NAME (or --pipe) to a full name such as \\.\pipe\cml_test_1.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5209

- **Cause:** Pipe is owned by a different user.  
- **Message:** `cannot identify the process serving '<...>' (<...>)`  
- **What to do:** Make sure the broker runs on this machine as your user.  
  Other variants of the advice: Stop that process or choose another pipe name with CML_PIPE_NAME.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5210

- **Cause:** Hello/Welcome mismatch or timeout.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5211

- **Cause:** Auto-start of cml_broker.exe failed.  
- **Message:** `cannot start the broker '<...>'`  
- **What to do:** Set CML_BROKER_PATH to cml_broker.exe, or start the broker yourself with 'cml_broker --headless'.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5212

- **Cause:** EnableModule: module not in the process.  
- **Message:** `module '<...>' is not loaded in process`  
- **What to do:** Load the module (or pass its full path) before calling EnableModule.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5213

- **Cause:** DisableModule: module was not enabled.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5214

- **Cause:** Message not allowed in this state.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5215

- **Cause:** Agent destroyed with the request open.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5216

- **Cause:** Connection dropped before the reply.  
- **Message:** `the connection to the agent was closed while sending`  
- **What to do:** The application may have exited; wait for its agent to reconnect.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5217

- **Cause:** Too many outstanding requests.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5218

- **Cause:** Agent thread/event could not be created.  
- **Message:** `cannot create the agent's synchronisation events`  
- **What to do:** Close other programs to free system resources and retry.  
  Other variants of the advice: hooks: request %llu: %s %llu function(s)  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5220

- **Cause:** See the message printed with the id.  
- **Message:** `module enumeration failed (injected failure)`  
- **What to do:** Test hook: set_enumeration_failures_for_testing.  
  Other variants of the advice: Check that the process still exists and that you may inspect it.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5221

- **Cause:** See the message printed with the id.  
- **Message:** `cannot query <...> (<...>)`  
- **What to do:** Check that the process can open its own token and run Chameleon as the same user as the application.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5290

- **Cause:** See the message printed with the id.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5291

- **Cause:** See the message printed with the id.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5292

- **Cause:** See the message printed with the id.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5293

- **Cause:** See the message printed with the id.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5294

- **Cause:** See the message printed with the id.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5295

- **Cause:** Reserved.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5296

- **Cause:** See the message printed with the id.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`

### CML5297

- **Cause:** See the message printed with the id.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_proc/include/cml/proc/diag_ids.h`


## CML5300 to CML5399

### CML5300

- **Cause:** Reload: no process / enabled module to patch.  
- **Message:** `the module '<...>' is no longer loaded in process`  
- **What to do:** Enable it again after it was reloaded.  
  Other variants of the advice: Enable a module with cml::Agent::EnableModule (see 'status'), then reload.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5301

- **Cause:** The module's PDB index is not ready / failed to load.  
- **Message:** `the PDB index of '<...>' failed to load`  
- **What to do:** See the broker log.  
  Other variants of the advice: Wait for the module's index (control command 'wait') and try again.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5302

- **Cause:** The reload did not finish within its time limit.  
- **Message:** `the reload ran out of time before the stage '<...>'`  
- **What to do:** Raise 'timeout_ms' of the reload command, or reduce the number of changed files.  
  Other variants of the advice: Raise 'timeout_ms'; a large PDB can take a few seconds. / Raise 'timeout_ms'.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5303

- **Cause:** (warning) a changed file belongs to no compiland.  
- **Message:** `the changed file '<...>' is not part of any compiland of`  
- **What to do:** No compiland lists the file and no scanned #include chain of the module reaches it (ADR-0013). Edit a source file that includes the header, or rebuild the project if the include path changed.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5304

- **Cause:** Another reload is running.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5305

- **Cause:** The agent could not reserve address space near the module.  
- **Message:** `the reserved address space near <...> has no room left for a patch`  
- **What to do:** Restart the application, or free address space near the module.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5306

- **Cause:** Transport: request not sent / no answer / connection lost.  
- **Message:** `carries no request id`  
- **What to do:** This is a broker bug; report it.  
  Other variants of the advice: Check that the application is still running.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5307

- **Cause:** LoadPatch / ApplyFixups / RunInitializers refused (agent id inside).  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5308

- **Cause:** InstallHooks refused (agent id inside).  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5309

- **Cause:** no HooksInstalled in time (synchronised mode: SyncPoint not reached).  
- **Message:** `the agent did not confirm the hooks in time`  
- **What to do:** In synchronised mode the application must call SyncPoint(); the patch stays loaded and the hooks may still apply.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5310

- **Cause:** The patch DLL could not be prepared (pre-applied fix-ups).  
- **Message:** `cannot read the patch DLL '<...>'`  
- **What to do:** Check that the linker produced the DLL.  
  Other variants of the advice: Report this with the bind plan; the patch cannot be prepared. / Check that no other process holds the file.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5311

- **Cause:** Control command arguments invalid / unknown option.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5312

- **Cause:** (warning) a source file could not be hashed.  
- **Message:** `the tracked file '<...>' is gone`  
- **What to do:** Restore it or ignore this warning.  
  Other variants of the advice: Changes to this file will not be detected; check its permissions.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5313

- **Cause:** The module's file cannot be read (exports, imports, old code bytes).  
- **Message:** `cannot read the module '<...>' to find its import slots`  
- **What to do:** Check that the module file still exists and is a valid x64 image.  
  Other variants of the advice: The module is skipped when resolving symbols; check that the file exists. / All functions of the patched object are hooked this time.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5314

- **Cause:** (warning) old code bytes unavailable: the function is hooked.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5315

- **Cause:** The session's temporary directory cannot be created.  
- **Message:** `cannot create the session directory '<...>'`  
- **What to do:** Check the TEMP directory (free space, permissions).  
  Other variants of the advice: Check the TEMP directory. / Check free disk space and the TEMP directory.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5316

- **Cause:** The module's PDB cannot be opened (cml_pdb's own id inside).  
- **Message:** `the PDB of '<...>' could not be opened`  
- **What to do:** See the broker log.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5317

- **Cause:** The agent disconnected while the reload ran.  
- **Message:** `the agent is no longer connected`  
- **What to do:** Restart the application, or wait for its agent to reconnect.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5318

- **Cause:** Generic wrapper: a stage failed (message names stage and cause).  
- **Message:** `this broker has no compiler driver`  
- **What to do:** Run the broker on Windows.  
  Other variants of the advice: This is a bug in the compiler driver; see the broker log.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5319

- **Cause:** (info) the sweep found no changed source.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`

### CML5320

- **Cause:** no answer within the time limit (the agent may still act on the request).  
- **Message:** `no answer to <...> within <...> ms`  
- **What to do:** The application may be stopped in a debugger or not reaching its sync point; try again.  
- **Raised by:** `broker/cml_core/include/cml/core/pipeline_diag_ids.h`


## CML5400 to CML5499

### CML5400

- **Cause:** Line is not valid JSON (offset given).  
- **Message:** `malformed JSON at byte <...>`  
- **What to do:** Send one complete JSON object per line (see docs/wp/WP-13.md, Control protocol).  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5401

- **Cause:** JSON ok, but not a valid request/response.  
- **Message:** `invalid control request`  
- **What to do:** Send {"id":N,"cmd":"<name>","args":{...}} with an integer id and a lowercase command name.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5402

- **Cause:** Command not known / not provided by this broker.  
- **Message:** `control command '<...>' is not provided by this broker (it provides: <...>)`  
- **What to do:** Query 'status' for the broker's capabilities and only send commands it lists.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5403

- **Cause:** Message above 1 MiB without newline / too big to send.  
- **Message:** `exceeds the 1 MiB control message limit`  
- **What to do:** Send less data per message.  
  Other variants of the advice: Send one JSON object per line, each under 1 MiB.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5404

- **Cause:** Cannot connect to the control pipe.  
- **Message:** `cannot connect to the broker control pipe '<...>'`  
- **What to do:** Start cml_broker with --control-pipe, or set CML_CONTROL_PIPE to the pipe it serves.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5405

- **Cause:** Control pipe closed or failed mid-conversation.  
- **Message:** `the control pipe failed`  
- **What to do:** Check that the broker is still running and reconnect.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5406

- **Cause:** Broker lacks a required capability.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5407

- **Cause:** Response does not match the request / bad shape.  
- **Message:** `the broker answered '<...>' with an error and no diagnostic`  
- **What to do:** Use matching versions of cml_ui and cml_broker.  
  Other variants of the advice: Use a cmltest/client and broker from the same build; report this as a bug otherwise. / Report this as a bug.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5408

- **Cause:** no answer within the timeout.  
- **Message:** `no answer to control command '<...>' within <...> ms`  
- **What to do:** Check that the broker is responsive (see its log).  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5410

- **Cause:** Scenario.toml is not in the supported TOML subset.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5411

- **Cause:** Unknown key/kind, missing field, bad value.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5412

- **Cause:** Unified diff cannot be parsed.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5413

- **Cause:** a hunk does not match the target file.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5414

- **Cause:** Cmake configure/build of the fixture failed.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5415

- **Cause:** a child process could not be started.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5416

- **Cause:** Expect_stdout: no match in time.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5417

- **Cause:** Expect_diag: diagnostic not seen in time.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5418

- **Cause:** Expect_module_enabled: not reported in time.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5419

- **Cause:** Process died early / scenario timeout.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5420

- **Cause:** Expect_stdout pattern cannot be compiled.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5421

- **Cause:** Bad command line / missing tool or file.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5422

- **Cause:** Edit step: file could not be read/written.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5423

- **Cause:** Crash dump could not be written.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5424

- **Cause:** Reload command answered with an error.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5425

- **Cause:** Steps skipped for a missing capability (failure under --require-all-capabilities).  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5426

- **Cause:** Report file could not be written.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5427

- **Cause:** Expect_monotonic: counter reset, constant or too few samples (WP-12).  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5428

- **Cause:** Reload result field differs from the scenario's expectation (WP-12).  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`


## CML5500 to CML5599

### CML5500

- **Cause:** (warning) a directory cannot be watched.  
- **Message:** `the directory '<...>' stopped delivering change events`  
- **What to do:** Saves there are picked up by the next 'reload'; the watch is being rebuilt.  
  Other variants of the advice: Saves in that directory are picked up by the next 'reload' instead.  
- **Raised by:** `core/cml_watch/include/cml/watch/watch_diag_ids.h`

### CML5501

- **Cause:** (warning) events were lost; every tracked file is re-checked.  
- **Message:** `the directory watcher lost events (buffer overflow)`  
- **What to do:** Nothing to do: every tracked source is re-checked by content hash.  
- **Raised by:** `core/cml_watch/include/cml/watch/watch_diag_ids.h`

### CML5502

- **Cause:** Directory watching is not available on this host.  
- **Message:** `directory watching (ReadDirectoryChangesW) is only available on Windows`  
- **What to do:** Use the 'reload' command to pick up edits on this host.  
- **Raised by:** `core/cml_watch/include/cml/watch/watch_diag_ids.h`

### CML5503

- **Cause:** The watcher stopped working (the directory went away, I/O error).  
- **Message:** `none of the <...> source directories could be watched`  
- **What to do:** Check that the source directories exist; use the 'reload' command meanwhile.  
  Other variants of the advice: Use the 'reload' command meanwhile.  
- **Raised by:** `core/cml_watch/include/cml/watch/watch_diag_ids.h`

### CML5504

- **Cause:** (info) compile-on-save is on but no source directory is known yet.  
- **Message:** `compile-on-save is on but no source directory is known yet`  
- **What to do:** Enable a module; the directories are watched as soon as its PDB index is ready.  
- **Raised by:** `core/cml_watch/include/cml/watch/watch_diag_ids.h`

### CML5505

- **Cause:** (warning) the reload started by a save failed (its own diag was published).  
- **Message:** `the reload started by a save failed`  
- **What to do:** Fix the error reported just before this and save again, or run 'reload'.  
- **Raised by:** `core/cml_watch/include/cml/watch/watch_diag_ids.h`

### CML5510

- **Cause:** Expect_event: no event of that name in time.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5511

- **Cause:** Set_option: the broker refused the option.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5512

- **Cause:** Wait: the broker's `wait` failed or timed out.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5550

- **Cause:** a changed file belongs to a precompiled header: cannot be hot-reloaded.  
- **Message:** `the changed <...>file <...>files <...> belong to the precompiled header of <...>; a precompiled header cannot be hot-reloaded`  
- **What to do:** Revert the change, or rebuild and restart the application. Edit the translation units that use the precompiled header instead.  
- **Raised by:** `broker/cml_core/include/cml/core/header_diag_ids.h`

### CML5551

- **Cause:** More compilands affected than `max_affected_compilands`: needs confirmation.  
- **Message:** `the change affects <...> compilands of <...> (<...> changed files), more than the limit of`  
- **What to do:** Repeat the reload with "confirm": true to recompile them all, or raise the option max_affected_compilands.  
- **Raised by:** `broker/cml_core/include/cml/core/header_diag_ids.h`

### CML5552

- **Cause:** a compiland to recompile uses a precompiled header whose files cannot be determined.  
- **Message:** `cannot tell which files make up the precompiled header used by <...>; a recompile could use a stale .pch`  
- **What to do:** Rebuild the project and hot-restart, or build with /Yu<header> naming the precompiled header.  
- **Raised by:** `broker/cml_core/include/cml/core/header_diag_ids.h`


## CML5600 to CML5699

### CML5600

- **Cause:** (warning) a module instance enabled after the session accepted edits for its sources (a DLL freed and loaded again, or a module a hot-restarted process enabled late) could not get those edits re-applied: the reload that re-applies them failed. The instance runs its original code.  
- **Message:** `module '<...>' (process <...>) runs its original code: re-applying the session's edits of <files> failed: <the reload's diagnostic>`  
- **What to do:** Fix the error and run `reload`; the edits stay pending until a reload applies them.  
- **Raised by:** `broker/cml_core/include/cml/core/reenable_diag_ids.h`

### CML5601

- **Cause:** (warning) a module instance enabled after the session accepted edits for its sources could not get those edits re-applied because its PDB index failed. The instance runs its original code.  
- **Message:** `module '<...>' (process <...>) runs its original code: the session's edits of <files> were not re-applied because its PDB index failed: <the index's diagnostic>`  
- **What to do:** Fix what the index diagnostic says, then run `reload` (or load the module again).  
- **Raised by:** `broker/cml_core/include/cml/core/reenable_diag_ids.h`

### CML5650

- **Cause:** a hook record points outside its image: skipped.  
- **Message:** `hook record(s) of <...> point outside the image or have an unknown kind`  
- **What to do:** Build the module and the hooks with the same Chameleon_API.h; the records are skipped.  
- **Raised by:** `agent/src/hook/user_hook_ids.h`

### CML5651

- **Cause:** The hook section of an image cannot be read: skipped.  
- **Message:** `the hook section of <...> cannot be read`  
- **What to do:** The module may have been unloaded; its hooks are skipped.  
- **Raised by:** `agent/src/hook/user_hook_ids.h`

### CML5652

- **Cause:** an application hook raised a processor exception.  
- **Message:** `the <...> hook <...> raised exception 0x`  
- **What to do:** Fix the hook; the next hook of this event runs and the application continues.  
- **Raised by:** `agent/src/hook/user_hook_ids.h`

### CML5653

- **Cause:** More hook records than the per-image limit: the rest are skipped.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/user_hook_ids.h`

### CML5654

- **Cause:** no memory to prepare the hook run: the hooks of this event are skipped.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/user_hook_ids.h`

### CML5655

- **Cause:** DestroyAgent called from inside an application hook: refused.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/user_hook_ids.h`

### CML5656

- **Cause:** a blocking wait called from a hook on the agent thread: refused.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `agent/src/hook/user_hook_ids.h`

### CML5657

- **Cause:** a hook overflowed the stack and the guard page could not be restored.  
- **Message:** `no application hook runs on this thread any more`  
- **What to do:** An earlier hook overflowed the stack and the guard page could not be restored; restart the application.  
  Other variants of the advice: No further hook runs on this thread; fix the recursion in the hook and restart the application.  
- **Raised by:** `agent/src/hook/user_hook_ids.h`


## CML5700 to CML5799

### CML5700

- **Cause:** ui.json cannot be read or is not valid JSON.  
- **Message:** `cannot read the settings file '<...>'`  
- **What to do:** Check the file permissions; delete the file to go back to the defaults.  
  Other variants of the advice: Fix the file by hand or delete it to go back to the defaults.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5701

- **Cause:** ui.json cannot be written.  
- **Message:** `cannot write the settings file '<...>'`  
- **What to do:** Check that the folder exists and is writable; the settings stay in effect until the UI closes.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5702

- **Cause:** The broker does not provide the command (capability missing / CML5402).  
- **Message:** `this broker does not provide '<...>'`  
- **What to do:** Use a broker build that advertises the '<...>' capability; the matching UI stays disabled.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5703

- **Cause:** The editor command template is invalid.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5704

- **Cause:** The editor could not be started.  
- **Message:** `cannot find the editor program '<...>'`  
- **What to do:** Install the editor, put it on PATH, or set its full path in the settings (the 'program' field).  
  Other variants of the advice: Check the editor command in the settings.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5705

- **Cause:** Cml_broker.exe could not be started.  
- **Message:** `cannot start the broker '<...>'`  
- **What to do:** Put cml_broker.exe next to cml_ui.exe, set the broker path in the settings, or start 'cml_broker --headless' yourself.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5706

- **Cause:** The global hotkey could not be registered.  
- **Message:** `the global hotkey <...> could not be registered (error <...>)`  
- **What to do:** Another program uses this key combination; choose another one in the settings.<...>Choose another key combination in the settings.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5707

- **Cause:** a --self-test check failed.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5708

- **Cause:** The control connection to the broker was lost.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5709

- **Cause:** The tray icon could not be created (warning).  
- **Message:** `the tray icon could not be created`  
- **What to do:** Closing the window exits the UI; the window stays reachable from the taskbar.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5710

- **Cause:** a hotkey string cannot be parsed.  
- **Message:** `hotkey '<...>' is not valid`  
- **What to do:** Write modifiers and one key joined by '+', for example Ctrl+Alt+F11.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5711

- **Cause:** Window or Direct3D 11 device could not be created.  
- **Message:** `no Direct3D 11 device could be created`  
- **What to do:** Update the graphics driver, or run on a machine with Direct3D 11 (WARP is tried too).  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5712

- **Cause:** The editor resolves to a batch file, which is never started.  
- **Message:** `the editor '<...>' resolves to the batch file '<...>'`  
- **What to do:** Batch files are never started (file names could inject commands). Point the 'program' setting at the editor's .exe.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5713

- **Cause:** Bad command line.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_ui/include/cml/ui/ui_diag_ids.h`

### CML5750

- **Cause:** Exception-action / exceptions: a missing or mistyped argument.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/exception_diag_ids.h`

### CML5751

- **Cause:** Exception-action: no such thread is held (answered already, or its process is gone).  
- **Message:** `no held thread with exception_id <...> in process`  
- **What to do:** Query 'exceptions' for the threads that wait for a decision; one that was answered or whose process exited is gone.  
- **Raised by:** `broker/cml_core/include/cml/core/exception_diag_ids.h`

### CML5752

- **Cause:** Exception-action: the agent did not answer / the connection is gone.  
- **Message:** `the agent of process <...> did not answer`  
- **What to do:** The application may have exited; the thread was dropped from the pending list.  
  Other variants of the advice: This is a bug; report it.  
- **Raised by:** `broker/cml_core/include/cml/core/exception_diag_ids.h`

### CML5753

- **Cause:** The agent refused (or did not answer) the crash-handler configuration.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/exception_diag_ids.h`

### CML5754

- **Cause:** (info) a held thread was released as Ignore because no client listens for exception events.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/exception_diag_ids.h`

### CML5755

- **Cause:** Exception-action without `pid`: several processes hold a thread with that exception_id.  
- **Message:** `exception_id <...> is held in more than one process`  
- **What to do:** Add "pid" to the request; 'exceptions' lists the processes.  
- **Raised by:** `broker/cml_core/include/cml/core/exception_diag_ids.h`


## CML5800 to CML5899

### CML5800

- **Cause:** This broker cannot restart processes (no process host on this system).  
- **Message:** `this broker cannot restart processes on this system`  
- **What to do:** Restart the application yourself; hot-restart needs the Windows broker.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5801

- **Cause:** `restart` arguments invalid, or no pid given and not exactly one process.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5802

- **Cause:** no connected process with that pid.  
- **Message:** `no connected process with pid`  
- **What to do:** Query 'status' for the processes whose agent is connected.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5803

- **Cause:** Another restart or a reload is running.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5804

- **Cause:** The agent refused or did not answer: the process was not touched.  
- **Message:** `process <...> did not prepare for the restart: <...> (the process was not touched)`  
- **What to do:** In synchronised mode the application must reach SyncPoint() for the pre-restart hooks to run; try again.  
  Other variants of the advice: Use an agent from the same Chameleon build as the broker. / See the agent's diagnostic for what to change.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5805

- **Cause:** The process cannot be opened for termination / handle duplication: not touched.  
- **Message:** `OpenProcess(<...>) failed (<...>)`  
- **What to do:** Run the broker as the same user as the application.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5806

- **Cause:** The old process could not be terminated.  
- **Message:** `process <...> could not be terminated`  
- **What to do:** Close it yourself and start the application again.  
  Other variants of the advice: Close the process yourself.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5807

- **Cause:** The old process is gone, the new one could not be started. Since WP-43 the broker tries twice (the first failure is the warning CML5822).  
- **Message:** `process <pid> was terminated but the new one could not be started (2 attempts): <reason>`  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5808

- **Cause:** The new process did not register in time (left running).  
- **Message:** `the new process (pid <...>) did not register within <...> ms; it was left running; its next restart enables its modules and re-applies the session's edits once it has registered`  
- **What to do:** Check that the application creates its agent at start-up (and that it can reach the broker); raise 'registration_timeout_ms' if it starts slowly. Since WP-43 the broker remembers the modules and edits of that restart: once the process has registered, its next `restart` enables the modules and re-applies the edits. The same budget also covers the new process's PDB indexes and the re-apply: when a restart that did register runs out of it, the broker log's `WP50-INDEX` and `WP50-RESTART` lines say which stage took the time (first steps, item 5).  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5809

- **Cause:** (warning) a module of the old process is not enabled in the new one.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5810

- **Cause:** (warning) the session's edits could not be re-applied to the new process.  
- **Message:** `the PDB index of '<...>' in the new process is not ready, so its edits are re-applied when it is (<n> file(s))` (WP-45: the broker re-applies them once the index is ready; CML5600 or CML5601 follows if that fails), or `... is not ready, so its edits cannot be re-applied` when the module had no edits to carry; `... the session's edits of <files> were NOT carried (the agent of the new process changed)` when another agent registered for the new process's pid during the restart and is not provably the same process (also for a module the old process had freed: `the session's edits of <files> (module '<...>') were NOT carried to the new process`)  
- **What to do:** Nothing to do when the message says the edits are re-applied; otherwise run 'reload' once its index is ready.  
  Other variants of the advice: Fix the error and run 'reload'.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5811

- **Cause:** (warning) a standard handle could not be handed on; the new process has none for it.  
- **Message:** `the <...> handle of process <...> could not be copied (<...>); the new process starts without it`  
- **What to do:** Redirect the output to a file or a pipe the application owns, or restart it from its launcher.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5812

- **Cause:** The new process exited before it registered.  
- **Message:** `the new process (pid <...>) exited with code <...> before its agent registered`  
- **What to do:** Run the application by hand to see why it stops; the old process is already gone.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5813

- **Cause:** (warning) nobody sent `restart-resume` within `hold_ms`, so the broker let the held process run (`restart {hold_new:true}`).  
- **Message:** `nobody resumed process <...> within <...> ms; the broker resumed it`  
- **What to do:** Send `restart-resume` as soon as the debugger is attached, or raise `hold_ms` (at most 60000). If attaching is slow, the debugger attaches to a process that is already running; breakpoints in start-up code may be missed.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5814

- **Cause:** `restart-resume` named a process that is not held: it never was, or it was resumed already (by an earlier `restart-resume`, by the hold timeout, because the client disconnected, or because the broker was shutting down).  
- **Message:** `process <...> is not held by a restart` or `process <...> is not held any more (it was resumed: <...>)`  
- **What to do:** Send `restart-resume` with the `new_pid` of a `restarting` event while its restart is waiting. A process the broker already resumed needs nothing more; this answer is harmless.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5815

- **Cause:** The broker was shutting down while a process was held. The process was resumed and the restart ended before the new agent registered.  
- **Message:** `the broker is shutting down; process <...> was resumed and the restart ended before its agent registered`  
- **What to do:** Nothing to do: the process runs without Chameleon. Start a broker; the application's agent connects to it by itself, or restart the application.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5816

- **Cause:** (warning) The client that sent `restart {hold_new:true}` disconnected while the new process was held. The broker resumed the process and finished the restart anyway.  
- **Message:** `the client that asked for the restart disconnected while process <...> was held; it was resumed and the restart continues`  
- **What to do:** Reconnect and use `status`. A debugger the client meant to attach has to be attached by hand (Debug > Attach to Process).  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5817

- **Cause:** The held process could not be resumed (`ResumeThread` failed). The broker terminated it instead of leaving it suspended. (A `restart-resume` that is not confirmed in time is CML5820 since WP-43, not this id.)  
- **Message:** `process <...> could not be resumed: <...>` (also `ResumeThread failed (Win32 error <...>); the process was terminated instead of being left suspended`)  
- **What to do:** Start the application yourself. If it happens again, check that no tool blocks access to the process and send the broker log with the report.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5818

- **Cause:** `restart` got an invalid `hold_new` or `hold_ms`, or `restart-resume` got no usable `pid`.  
- **Message:** `argument 'hold_new' must be true or false`, `argument 'hold_ms' must be an integer from 1 to 60000` or `` `restart-resume` needs the integer argument 'pid' ``  
- **What to do:** Send `{"hold_new":true,"hold_ms":N}` with hold_ms from 1 to 60000 (absent: 10000), and `{"pid":N}` with the `new_pid` of the `restarting` event.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5819

- **Cause:** `restart {hold_new:true}` was sent to a broker whose process host cannot create a process suspended. Nothing was touched.  
- **Message:** `this broker cannot hold a new process (the process host cannot create it suspended)`  
- **What to do:** Send `restart` without `hold_new`, or attach the debugger after the `restarted` event. Clients check for the `restart-hold` capability first.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5820

- **Cause:** `restart-resume` asked the restart to resume its held process, but the restart did not confirm within 10 s that the process runs. Nothing is known to have failed: the resume stays requested and the restart acts on it; the broker resumes the process by itself when `hold_ms` passes.  
- **Message:** `the restart did not confirm within <n> ms that process <pid> runs; the resume is still requested, and the broker resumes the process by itself when hold_ms passes`  
- **What to do:** Wait for `restarted` (or query `status`); send `restart-resume` again if the process is still held. Before WP-43 this case was reported as CML5817.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5821

- **Cause:** The broker shut down while a restart ran. Before the old process was terminated nothing was touched; after it, the new process was left running (registered or not), and its restart ended without waiting for its registration, its modules or the re-applying of the session's edits.  
- **Message:** `the broker is shutting down; process <pid> was not restarted (it was not touched)`, `... the new process (pid <pid>) was left running before its agent registered` or `... runs, but its restart ended before its modules were enabled and its edits re-applied`  
- **What to do:** Start the broker again. The application's agent connects by itself; reload (or restart) to get the session's edits back.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5822

- **Cause:** (Warning.) The old process was terminated and the first attempt to start the new one failed (CML5807 with the reason). The broker tries once more after a short pause; when that attempt fails too the restart ends with CML5807.  
- **Message:** `process <pid> was terminated and the first attempt to start the new one failed (<CML5807 ...>); trying once more`  
- **What to do:** Nothing if the restart then succeeds. If it happens often, check what holds the executable (a virus scanner, a debugger) when a process ends.  
- **Raised by:** `broker/cml_core/include/cml/core/restart_diag_ids.h`

### CML5840

- **Cause:** Restart: the broker refused or the step could not complete.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5841

- **Cause:** Restart: the result or event differs from what the scenario expects.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`

### CML5850

- **Cause:** no live patch in the targeted modules.  
- **Message:** `no patch to revert<...>: no enabled module<...> in process <...><...> matching '<...>'`  
- **What to do:** A revert undoes patches that a 'reload' applied in this session; reload first.  
- **Raised by:** `broker/cml_core/include/cml/core/revert_diag_ids.h`

### CML5851

- **Cause:** The agent refused the hook changes (its id inside); nothing changed.  
- **Message:** `the agent refused the <...>: <...>unexpected answer`  
- **What to do:** Nothing was changed. See the agent log, then try again.  
  Other variants of the advice: Nothing was changed. If a thread keeps running inside the function, wait until the application leaves it and try again.  
- **Raised by:** `broker/cml_core/include/cml/core/revert_diag_ids.h`

### CML5852

- **Cause:** (warning) no HooksInstalled in time; the agent may still apply the change.  
- **Message:** `the agent did not confirm the <...> of <...> function(s) in time`  
- **What to do:** In synchronised mode the application must call SyncPoint(); the change may still be applied there.  
- **Raised by:** `broker/cml_core/include/cml/core/revert_diag_ids.h`

### CML5853

- **Cause:** `scope` is neither "last" nor "all".  
- **Message:** `argument 'scope' must be "last" or "all"`  
- **What to do:** Send {"scope":"last"} to undo the newest reload, {"scope":"all"} to undo every reload.  
- **Raised by:** `broker/cml_core/include/cml/core/revert_diag_ids.h`

### CML5854

- **Cause:** Several modules: some were reverted, others failed (n of m).  
- **Message:** `of <...> modules could not be reverted`  
- **What to do:** The modules in 'reverted' are done; fix the cause for those in 'failed' and send 'revert' again (it only touches what is still applied).  
- **Raised by:** `broker/cml_core/include/cml/core/revert_diag_ids.h`

### CML5855

- **Cause:** (warning) the agent did not un-commit a reverted patch (older agent?): its application hooks may keep running.  
- **Message:** `the agent did not un-commit <...> of <...> reverted patch(es): <...>. Application hooks (CML_PREPATCH_HOOK, ...) defined in them may keep running`  
- **What to do:** Use an agent from the same build as the broker; reloading again replaces the hooks of the same functions.  
- **Raised by:** `broker/cml_core/include/cml/core/revert_diag_ids.h`

### CML5856

- **Cause:** The module is no longer loaded where the patches were made; the stale patch list was dropped.  
- **Message:** `the module '<...>' is no longer loaded in process <...>; its <...> applied reload(s) can no longer be reverted and were forgotten`  
- **What to do:** Enable the module again, then reload.  
- **Raised by:** `broker/cml_core/include/cml/core/revert_diag_ids.h`

### CML5857

- **Cause:** The second half of a revert failed and the first could not be undone.  
- **Message:** `the revert failed (<...>) and the functions it had already re-pointed could not be put back`  
- **What to do:** The process runs a mix of patches. Restart the application, or reload again.  
- **Raised by:** `broker/cml_core/include/cml/core/revert_diag_ids.h`

### CML5858

- **Cause:** The revert ran out of time before a step.  
- **Message:** `the revert ran out of time before`  
- **What to do:** Raise 'timeout_ms' of the revert command, then try again.  
- **Raised by:** `broker/cml_core/include/cml/core/revert_diag_ids.h`


## CML5900 to CML5999

### CML5900

- **Cause:** no patch applied since the fault replaced a function on the thread's stack.  
- **Message:** `CML<...>: the offer is being evaluated`  
- **What to do:** CML%u: exception %u of process %u released as Ignore: no client listens for exception events.  
- **Raised by:** `broker/cml_core/include/cml/core/reexecute_diag_ids.h`

### CML5901

- **Cause:** The function's compiland was built with optimisation.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/reexecute_diag_ids.h`

### CML5902

- **Cause:** The recorded build command does not say whether optimisation was off.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/reexecute_diag_ids.h`

### CML5903

- **Cause:** The frame's function cannot be identified (patch code, module not enabled, PDB not loaded).  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/reexecute_diag_ids.h`

### CML5904

- **Cause:** The function type cannot be read, or its shape shifts the arguments (variadic, returns a class).  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/reexecute_diag_ids.h`

### CML5905

- **Cause:** a register parameter has a type that is not an integer, pointer, float or double.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/reexecute_diag_ids.h`

### CML5906

- **Cause:** The PDB does not tell where a register parameter lives (or it lives in a register).  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/reexecute_diag_ids.h`

### CML5907

- **Cause:** The agent did not report the stack of the held thread.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `broker/cml_core/include/cml/core/reexecute_diag_ids.h`

### CML5908

- **Cause:** Exception-action "reexecute" while re-execute is not offered.  
- **Message:** `re-execute is not offered for exception <...>`  
- **What to do:** Apply a patch that replaces the faulting function (the event 'exception_updated' announces it), or choose continue, leave or ignore.  
- **Raised by:** `broker/cml_core/include/cml/core/reexecute_diag_ids.h`

### CML5920

- **Cause:** See the message printed with the id.  
- **Message:** `object(s) linked into <...> from static libraries cannot be hot-reloaded: <...>, and <...> more<...>.`  
- **What to do:** Build those libraries with /Z7 (or /Zi) so the PDB records how each object was compiled (clang-cl: -gcodeview-command-line), and keep their sources on disk; edits to the other members of the same libraries are reloaded normally.  
- **Raised by:** `broker/cml_core/include/cml/core/static_lib_diag_ids.h`

### CML5921

- **Cause:** See the message printed with the id.  
- **Message:** `the changed file '<...>' is only owned by compilands of <...> that cannot be reloaded`  
- **What to do:** Rebuild the project (and the library it belongs to) with /Z7 so its PDB records the compile command, then restart; or rebuild and hot-restart now.  
- **Raised by:** `broker/cml_core/include/cml/core/static_lib_diag_ids.h`

### CML5960

- **Cause:** Inject-failure: missing/unknown stage, bad count or pid.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5961

- **Cause:** Inject-failure: no agent with that pid.  
- **Message:** `no agent is connected for process`  
- **What to do:** Query 'status' for the connected processes.  
  Other variants of the advice: Start the application with the agent and wait for 'status' to list it.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5962

- **Cause:** The agent did not arm the stage.  
- **Message:** `the agent of process <...> did not answer ArmFault`  
- **What to do:** Check that the process is still running.  
  Other variants of the advice: Use an agent built with CML_FAULT_INJECTION from the same build as the broker.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5963

- **Cause:** See the message printed with the id.  
- **Message:** `the broker's mapping of the module's PDB (dropped, then fails to open)`  
- **What to do:** change-detection.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5964

- **Cause:** See the message printed with the id.  
- **Message:** `the sweep that hashes the tracked sources`  
- **What to do:** compile.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5965

- **Cause:** See the message printed with the id.  
- **Message:** `the compile stage (the application's compile-error hook runs)`  
- **What to do:** bind-plan.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5966

- **Cause:** See the message printed with the id.  
- **Message:** `the bind planner of the first recompiled compiland`  
- **What to do:** reservation.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5967

- **Cause:** See the message printed with the id.  
- **Message:** `the link of the patch DLL`  
- **What to do:** pre-apply.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5968

- **Cause:** See the message printed with the id.  
- **Message:** `the broker's pre-applied relocations in the patch DLL`  
- **What to do:** send-patch.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5969

- **Cause:** See the message printed with the id.  
- **Message:** `sending LoadPatch to the agent`  
- **What to do:** patch-load.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5970

- **Cause:** Inject-failure: the stage cannot fire for that process (reservation already made).  
- **Message:** `the stage 'reservation' fires only on a module's first patch, and every enabled module of process <...> has reserved its address space already (or none is enabled)`  
- **What to do:** Arm it right after the module is enabled and before its first reload, or start a new process.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5971

- **Cause:** Inject-failure "none": some agents could not be disarmed.  
- **Message:** `the broker's stages are disarmed, but the agents of process(es) <...> could not be disarmed`  
- **What to do:** Check that those processes are still running, then send {"stage":"none"} again.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5972

- **Cause:** Failure injection, stage `restart-terminate` (like every restart and revert stage, armed with count 1 for the next restart of its process; at the end of that restart it is disarmed whether it fired or not, and the answer lists an unused one in `injection_disarmed`): the restart failed before the old process was terminated. The process was not terminated and keeps running; its pre-restart hooks had run.  
- **Message:** `injected failure at the stage 'restart-terminate' (...); process <pid> was not terminated. <n> pre-restart hook(s) ran; the process was not restarted.`  
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). Then `restart` again.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5973

- **Cause:** (Warning.) Failure injection, stage `restart-relaunch`: the first attempt to start the new process (after the old one was terminated) failed. The broker tried once more and started it; the restart went on.  
- **Message:** `injected failure at the stage 'restart-relaunch' (...); process <pid> is gone, trying once more to start the new one`  
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). Nothing else: the new process runs.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5974

- **Cause:** (Warning.) Failure injection, stage `restart-hold`: a restart with `hold_new` could not hold the new process for the debugger. It was resumed at once, no `restarting` event was published, and the restart went on.  
- **Message:** `injected failure at the stage 'restart-hold' (...); process <pid> was resumed at once, without waiting for the debugger, and the restart goes on`  
- **What to do:** Attach the debugger after `restarted`, or by hand. This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage).  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5975

- **Cause:** Failure injection, stage `restart-registration`: the restart stopped waiting for the new process to register. The new process was left running; its agent connects by itself, but without the modules the old process had enabled (unless the application enables them) and without the session's edits. The broker remembers both for that process.  
- **Message:** `injected failure at the stage 'restart-registration' (...); the new process (pid <pid>) was left running, not taken over by the restart; its next restart enables its modules and re-applies the session's edits`  
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). Then `restart` the new process: that restart enables its modules and re-applies the edits.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5976

- **Cause:** (Warning.) Failure injection, stage `restart-reenable`: the request that enables the old process's modules in the new one was not sent. The new process runs; the modules are reported in `not_enabled` (CML5809) and remembered for that process.  
- **Message:** `injected failure at the stage 'restart-reenable' (...); the request to enable <n> module(s) again was not sent to process <pid>`  
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). Then `restart` again (it enables the modules and re-applies the edits), or enable the modules from the application.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5977

- **Cause:** Failure injection, stage `restart-reapply`: the reload that re-applies the session's edits to the new process failed before it compiled. The new process runs the original code; its source baseline was restored, so the edits are pending. Reported as `reapply_error` and the warning CML5810.  
- **Message:** `injected failure at the stage 'restart-reapply' (...)` (in CML5810: `the new process (pid <pid>) runs the original code: re-applying the session's edits failed: CML5977 ...`)  
- **What to do:** This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage). Then `reload`: it applies the session's edits.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5978

- **Cause:** `inject-failure` was asked to arm a stage of the restart (or the revert) while a restart (or a reload or revert) runs. A restart or revert stage is armed for the next operation of its process only; arming it in the middle of one could fail a step that must not fail (the undo of a revert's re-pointing, say).   WP-44: the same holds for the stages of `exception-action` while an `exception-action` runs, and for the watch stages while a batch of saves is handled (the operation counts as running until it has disarmed what is left for it).  
- **Message:** `the stage '<name>' cannot be armed while a restart runs` (or `... while a reload or revert runs`, `... while an exception-action runs`, `... while a batch of saves of the compile-on-save watcher runs`)  
- **What to do:** Wait for the operation to finish (control command `wait`), then arm the stage.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5979

- **Cause:** Failure injection, stage `exception-report` (group `exception`, armed for the next held thread the broker hears of; one arming of the group at a time, so arming it disarms `exception-hold`): the broker did not publish the `exception` event of a held thread and published this diag instead. The thread stays held and pending.
- **Message:** `injected failure at the stage 'exception-report' (...): exception <id> of process <pid> (thread <tid>) was not reported; it stays held`
- **What to do:** Query `exceptions` for the held thread and answer it with `exception-action`. This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage).
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5980

- **Cause:** Failure injection, stage `watch-debounce` (group `watch`, armed for the next batch of saves that touches a tracked file; at the end of that batch every watch stage still armed is disarmed and reported, CML5983): the compile-on-save watcher lost the debounced batch and did what it does after lost events: every tracked source is re-checked by content hash, so the save is still picked up.
- **Message:** `injected failure at the stage 'watch-debounce' (...): <n> tracked file(s) of the batch are re-checked with every other tracked file`
- **What to do:** Nothing: the reload runs. This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage).
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5981

- **Cause:** Failure injection, stage `watch-change-detection` (group `watch`, like CML5980): the watcher's check which saved files are tracked failed and the batch was dropped; no reload started. The change stays pending.
- **Message:** `injected failure at the stage 'watch-change-detection' (...): the save of <n> tracked file(s) starts no reload`
- **What to do:** Save again, or run `reload`. This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage).
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5982

- **Cause:** Failure injection, stage `watch-reload` (group `watch`, like CML5980): the reload that a save started failed before it ran; the watcher's own CML5505 follows. The change stays pending.
- **Message:** `injected failure at the stage 'watch-reload' (...): the reload that the save started did not run`
- **What to do:** Save again, or run `reload`. This failure was requested with the control command `inject-failure` (failure injection, a test facility; capability `fault-injection`). Repeat the operation without arming the stage (`{"stage":"none"}` disarms every stage).
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5983

- **Cause:** (warning) Failure injection: a batch of saves ended without reaching a watch stage that was armed for it (for example `watch-reload` after `watch-change-detection` dropped the batch). The stage was disarmed: an arming of the group `watch` applies to one batch only and never carries over.
- **Message:** `failure injection: the batch of saves did not reach the armed stage(s) <names>; disarmed (injection_disarmed)`
- **What to do:** Arm the stage again before the save that should fail it.
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5984

- **Cause:** Persistent arming (`inject-failure {"persist":true}`): the broker writes no log file, so there is no place next to it to keep the arming; or the state file `<log>.fault-injection.json` could not be written; or, at start, the file could not be used (not JSON, another version, an entry naming no stage of its side or with a bad count or pid). Nothing is armed in that case; a broken file is renamed to `<file>.bad`.
- **Message:** `the arming of '<stage>' cannot be kept across a broker restart: this broker writes no log file to keep it next to`, `cannot write the failure-injection state file '<path>'`, or `the failure-injection state file '<path>' cannot be used (<why>); nothing was armed from it`
- **What to do:** Start the broker with `--log FILE`, check that the log's directory is writable, or arm the stage without `persist`. After a broken file, arm the stages again.
- **Raised by:** `core/cml_ipc/include/cml/ipc/fault_stages.h`

### CML5985

- **Cause:** Inject_failure: the stage is not listed, the command was refused or answered wrongly.  
- **Message:** see the diagnostic text printed with the id  
- **What to do:** follow the advice printed after the message (`What to do:`); if it does not resolve the problem, re-run with `--verbose` and read the log lines before this diagnostic.  
- **Raised by:** `core/cml_ipc/include/cml/ipc/control_ids.h`


### CML5998

- **Cause:** The licence check completed and refused this installation. The open build of Chameleon ships a no-op provider that always allows, so this id is only raised by a build that installs its own `cml::license::Provider`.  
- **Message:** `the licence check refused this installation: <reason>`  
- **What to do:** Install a valid licence for this version of Chameleon, or contact the vendor. The broker exits with code 1 after logging this diagnostic.  
- **Raised by:** `core/cml_license/include/cml/license/license_diag_ids.h`

### CML5999

- **Cause:** The licence check could not be completed (no answer from the licence service, unreadable licence file). Same condition as CML5998: only a build with a non-default provider raises it.  
- **Message:** `the licence check could not be completed: <reason>`  
- **What to do:** Check that the licence file or service is reachable and run the broker again. The broker logs this as a warning and keeps running.  
- **Raised by:** `core/cml_license/include/cml/license/license_diag_ids.h`

### CML1700

- **Cause:** Chameleon supports Windows x64 only; the target is configured for another system or processor. Also raised by `find_package(Chameleon)` for a non-64-bit project.  
- **Message:** `chameleon_enable(<target>): the target is '<system>' / '<processor>' with <n>-byte pointers; Chameleon supports Windows x64 only.`  
- **What to do:** Configure with an x64 MSVC or clang-cl toolchain (Visual Studio generator: -A x64; Ninja: run from an x64 developer prompt).  
- **Raised by:** `chameleon_enable()` (CMake)

### CML1701

- **Cause:** The C++ compiler is a GNU-style one (gcc, clang++); Chameleon needs the CodeView PDBs and MSVC link of cl or clang-cl.  
- **Message:** `chameleon_enable(<target>): the C++ compiler is <id> with the GNU-style command line; Chameleon needs cl or clang-cl.`  
- **What to do:** Configure with cl or clang-cl (clang-cl: -DCMAKE_CXX_COMPILER=clang-cl, not clang++).  
- **Raised by:** `chameleon_enable()` (CMake)

### CML1702

- **Cause:** The target is an INTERFACE or utility target without compiled code; no flags were applied.  
- **Message:** `chameleon_enable(<target>): a <type> target has no compiled code of its own; no flags were applied.`  
- **What to do:** Call chameleon_enable on the executables and libraries that contain the code you reload.  
- **Raised by:** `chameleon_enable()` (CMake, warning)

### CML1703

- **Cause:** No target of that name exists at the point of the call.  
- **Message:** `chameleon_enable(<target>): there is no target of that name.`  
- **What to do:** Call chameleon_enable after add_executable/add_library for the target.  
- **Raised by:** `chameleon_enable()` (CMake)

### CML1704

- **Cause:** The single-configuration build type is Release or MinSizeRel: no debug information for the compiler, and inlining defeats patching.  
- **Message:** `chameleon_enable(<target>): CMAKE_BUILD_TYPE is '<type>'. Release and MinSizeRel record no debug information for the compiler, and their inlining defeats patching.`  
- **What to do:** Configure with -DCMAKE_BUILD_TYPE=Debug or RelWithDebInfo for the build you iterate on.  
- **Raised by:** `chameleon_enable()` (CMake, warning)

### CML1705

- **Cause:** Interprocedural optimisation (/GL, LTCG) is on for the target; it is switched off for it because `cml_broker check` fails /GL builds (CML1103).  
- **Message:** `chameleon_enable(<target>): interprocedural optimisation (/GL, LTCG) is on; it is switched off for this target.`  
- **What to do:** Do not set INTERPROCEDURAL_OPTIMIZATION or CMAKE_INTERPROCEDURAL_OPTIMIZATION for the targets you reload.  
- **Raised by:** `chameleon_enable()` (CMake, warning)

### CML1706

- **Cause:** Flags in effect contain options that `cml_broker check` rejects: /guard:cf, /GL, /LTCG, /clr, /ZW, /DEBUG:FASTLINK.  
- **Message:** `chameleon_enable(<target>): the flags in effect contain options `cml_broker check` rejects: <flags>.`  
- **What to do:** Remove them from CMAKE_*_FLAGS, CMAKE_*_LINKER_FLAGS and the target's options for the targets you reload.  
- **Raised by:** `chameleon_enable()` (CMake, warning)

### CML1707

- **Cause:** The compiler is older than Visual Studio 2022 (cl 19.30) or LLVM 15 (clang-cl); it is not tested with Chameleon.  
- **Message:** `cl <version> is older than Visual Studio 2022 (19.30); it is not tested with Chameleon. (clang-cl: older than LLVM 15.)`  
- **What to do:** Use Visual Studio 2022 (MSVC v143) or newer, or clang-cl 15 or newer.  
- **Raised by:** `chameleon_enable()` (CMake, warning)

### CML1709

- **Cause:** An argument other than NO_AGENT, ALLOW_INLINING or NO_COPY_AGENT was passed.  
- **Message:** `chameleon_enable(<target>) got unknown arguments '<args>'.`  
- **What to do:** Use only NO_AGENT, ALLOW_INLINING and NO_COPY_AGENT.  
- **Raised by:** `chameleon_enable()` (CMake)

### CML1710

- **Cause:** The imported target Chameleon::agent does not exist: find_package(Chameleon) was not called.  
- **Message:** `chameleon_enable(<target>): the target Chameleon::agent does not exist.`  
- **What to do:** Call find_package(Chameleon CONFIG REQUIRED) (with CMAKE_PREFIX_PATH or Chameleon_DIR at the install tree) before chameleon_enable, or pass NO_AGENT.  
- **Raised by:** `chameleon_enable()` (CMake)

### CML1711

- **Cause:** chameleon_enable was called from a different directory than the one that created the target, so cml_agent.dll is not copied next to the executable.  
- **Message:** `chameleon_enable(<target>) was called from a different directory than the one that created the target; cml_agent.dll is not copied next to it.`  
- **What to do:** Call chameleon_enable in the directory of the add_executable call, or copy cml_agent.dll yourself (or pass NO_COPY_AGENT).  
- **Raised by:** `chameleon_enable()` (CMake, warning)

### CML1712

- **Cause:** chameleon_enable (or cml_target_fixture_style) was called for a target created in a directory that has already been processed (the call is in a parent directory, after add_subdirectory), so CMake's default /Ob1 /Ob2 cannot be removed from that directory's flags.  
- **Message:** `<caller>(<target>): <target> was created in <dir>, which has already been processed, so CMake's default /Ob1 /Ob2 cannot be removed from its flags and cl will print D9025 "overriding '/Ob1' with '/Ob0'" for every TU in RelWithDebInfo and Release.` (with clang-cl: "... and the configuration's /Ob1 /Ob2 stay in its flags next to /Ob0.")  
- **What to do:** Call chameleon_enable in the directory that creates the target (at the end of its CMakeLists.txt or in a function it calls).  
- **Raised by:** `chameleon_enable()` and `cml_target_fixture_style()` (CMake, warning)

### CML5990

- **Cause:** No broker answers on the control pipe (not started, wrong pipe name, other user).  
- **Message:** `cannot reach the Chameleon broker on <pipe>: <detail>`  
- **What to do:** Start cml_broker (or cml_ui) as the same user, or set the 'chameleon.controlPipe' setting to the pipe the broker was started with (--control-pipe).  
- **Raised by:** VS Code extension

### CML5991

- **Cause:** The control connection closed or failed after it was established.  
- **Message:** `the control pipe <pipe> was lost: <detail>`  
- **What to do:** Check that the broker is still running; the extension reconnects by itself while 'chameleon.autoConnect' is on, or run 'Chameleon: Connect'.  
- **Raised by:** VS Code extension

### CML5992

- **Cause:** The broker lacks a capability or does not know a command (unsupported broker version). The feature is disabled, nothing else is affected.  
- **Message:** `this broker (version <v>) does not provide <what>`  
- **What to do:** Use a Chameleon broker that advertises this capability (build the broker and the extension from the same release).  
- **Raised by:** VS Code extension

### CML5993

- **Cause:** The broker's answer is unusable (malformed line, wrong shape, no answer in time).  
- **Message:** `the broker's answer cannot be used: <detail>`  
- **What to do:** Check the broker log; make sure the broker and the extension come from the same release.  
- **Raised by:** VS Code extension

### CML5994

- **Cause:** The control pipe cannot be reached: no broker is running, or it serves another pipe name.  
- **Message:** `Cannot reach the Chameleon broker on the control pipe '<pipe>': <detail>.`  
- **What to do:** Start the application with the Chameleon agent (it starts the broker) or run 'cml_broker.exe --headless', then choose Tools > Chameleon > Connect; if the broker serves another pipe name, enter it in Tools > Options > Chameleon > General.  
- **Raised by:** Visual Studio extension

### CML5995

- **Cause:** The connection to the broker was lost, or a request got no answer.  
- **Message:** `The connection to the Chameleon broker on '<pipe>' was lost: <detail>.`  
- **What to do:** Chameleon reconnects by itself while 'Reconnect automatically' is on in Tools > Options > Chameleon > General; otherwise choose Tools > Chameleon > Connect once the broker runs again.  
- **Raised by:** Visual Studio extension

### CML5996

- **Cause:** The broker lacks a capability the extension needs for a feature, or is not a Chameleon broker.  
- **Message:** `The broker <reason>.`  
- **What to do:** Use cml_broker.exe and cml_agent.dll from one Chameleon release; the Visual Studio features that need the missing capability stay disabled.  
- **Raised by:** Visual Studio extension

### CML5997

- **Cause:** The debugger could not be re-attached to the restarted process after a hot-restart.  
- **Message:** `Could not attach the debugger to the restarted process (pid <new>, it replaced pid <old>): <reason>.`  
- **What to do:** Attach by hand with Debug > Attach to Process (pid <new>); to stop Chameleon re-attaching, clear Tools > Chameleon > Re-attach debugger after hot-restart (or the same setting in Tools > Options > Chameleon > General).  
  Other variants (a restart that held the new process for the debugger, capability `restart-hold`): `the debugger did not finish attaching within <n> s; the process was resumed without waiting` (the process runs; attach by hand), and `Could not tell the broker to resume the restarted process (pid <new>): <reason>` (the broker resumes the process by itself when the hold time (30 s by default) has passed; if the application stays frozen, hot-restart again), and (WP-43, broker id CML5820) `The broker did not confirm in time that the restarted process (pid <new>) runs again: <reason>` (the resume is still requested; wait). Since WP-43 one restart reports at most one CML5997; later problems of the same restart are only logged.  
- **Raised by:** Visual Studio extension

### CML1708

- **Cause:** `chameleon_enable()` was called on an IMPORTED target; its flags were fixed when it was built elsewhere.  
- **Message:** `chameleon_enable(<target>): <target> is an IMPORTED target; its flags were fixed when it was built elsewhere.`  
- **What to do:** Call chameleon_enable on the targets this project builds (add_executable/add_library), not on imported ones.  
- **Raised by:** `chameleon_enable()` (CMake)
