# Contributing to ID6Term

Developer notes: what is where, how to build the plugin and its host tests, and how a release is packaged for
[NBTApps](https://github.com/tom-pryor/nbtapps). What the app does and how to install it is in [README.md](README.md).
General notes for writing *any* iDrive PluginApp (loading mechanism, XML reference, framework ABI, id registry, a
compilable skeleton) live in the [BMW-ID6-WS `docs/`](https://github.com/josephworks/BMW-ID6-WS/tree/master/docs) folder.

## Contents

| Path | What |
|---|---|
| `src/id6term_app.cpp` | The PluginApp: entry point `pluginAppInfo`, handlers, model updates, polling. `ID6TERM_RELEASE` here is the version of every release |
| `src/bmw_rhmi.hpp` | Declarations of the BMW framework ABI (PluginApp, Parcel, BasicString, Variant, Map, DataTable) and safe readers for action arguments |
| `src/term_buffer.hpp` | Console text: bounded line ring, control-character / ANSI filter |
| `src/command_line.hpp` | Speller editing (+ letter case), history + suggestions, the session shell's init script, command wrapper and end-of-command sentinel |
| `src/shell_runner.{hpp,cpp}` | The shell session: one long-lived `sh` (`spawn()` + stdin/stdout pipes + reader thread), one command at a time, timeout, stop |
| `res/ID6Term.xml` | UI description: entry row, menu, console, speller, quick commands, about |
| `res/ID6Term.manifest` | Tells CAppManager what to load (connector id 300) |
| `res/make_db.py` | Generates `ID6Term_ALL_Texts.zip` (10 languages) and `ID6Term_ALL_Images.zip` (icons drawn procedurally) |
| `lib/` | `libAppFramework.A.so`, `libCoreFramework.A.so` from the head unit (`/opt/hmi/ID5/lib`), link-time only; not in the repo, take them from your own firmware dump |
| `tests/` | Host tests (Linux g++) for the terminal core |
| `tools/package.py` | Collects `dist/ID6Term/` with everything the head unit needs plus `install.sh` / `uninstall.sh` |
| `tools/nbtpkg_build.py`, `nbtapps/` | Builds the signed `.nbtpkg` for the NBTApps catalogue from `nbtapps/manifest.json.in`, plus the `listing.json` / version record for the [nbtapps-packages](https://github.com/tom-pryor/nbtapps-packages) pull request |
| `common.mk`, `Makefile`, `arm/so-le-v7/` | QNX Momentics recursive build (shared object, armv7le, GNU libstdc++) |

Ids shared between `res/ID6Term.xml`, `res/make_db.py` and the constants in `src/id6term_app.cpp`: state ids 10..15,
actions 5/21..23/26..28/30..37/71..74, models 3/4/12/13/31..37/40..46/50..57. Ids that must not collide with other
apps on the car: the connector id (**300**) and `entryButtonFocusID` (**32760**); see [Plugin ids](#plugin-ids).
Native ids the entry row hooks into (read from HmiMain3D, `CHmiCDrApplication::initMenus`): the ConnectedDrive menu
is state **50** (`ST_CDr_Selection`), one list with section rows - OnlineApps stripline 7513 + label **7514**
("Vehicle Apps"), BMWApps stripline 7515 + label 7516 ("Smartphone Apps"), BMW Assistance stripline 6307. ID6Term
inserts after 7514. For reference, Media/Radio is state 20 (`ST_Ent_Selection`) and its Bluetooth audio row focus
2270 (NBT-AAD's placement, ID6Term's until 1.0.1); `applicationType="OnlineServices"` lands in Smartphone Apps.

## Building

**Momentics IDE (SDP 6.5.0):** import/open the project, right-click -> Clean, then Build. Output:
`arm/so-le-v7/libID6Term.so`.

**Command line (Windows host with the SDP installed):**

```powershell
$env:QNX_HOST="C:/ProgramData/QNX650/host/win32/x86"; $env:QNX_TARGET="C:/ProgramData/QNX650/target/qnx6"
$env:MAKEFLAGS="-I$env:QNX_TARGET/usr/include"; $env:PATH="$env:QNX_HOST/usr/bin;$env:PATH"
make                      # -> arm/so-le-v7/libID6Term.so
python tools\package.py   # -> dist/ID6Term/ (regenerates the text/image zips too)
```

Build facts that matter (all learned from NBT-AAD):
- The library is a PIC shared object built with the **GNU libstdc++** variant (`-Y _gpp`): HmiMain3D and BMW's own
  plugins use `libstdc++.so.6`, not the Dinkumware default.
- It links against the two framework libraries in `lib/` only to resolve the `bmw::` symbols; `--allow-shlib-undefined`
  because those depend on more libraries that only exist on the car. On the unit the RPATH `/opt/hmi/ID5/lib` finds them.
- QNX GCC 4.4.2: no `__sync_*`, no local classes as template arguments, `<stddef.h>` for `size_t`.

### Host tests

Linux g++ (WSL works): `make -C tests check` builds and runs `test_term_buffer`, `test_command_line` and
`test_shell_runner` (the latter runs a session of the host `/bin/sh`: `cd` / variable / function persistence,
streamed output, errors not ending the session, timeout and `stop()` interrupting only the command, SIGKILL
escalation, `exit`, background output). `ID6TERM_TEST_SHELL=/path/to/mksh tests/build/test_shell_runner` runs it
against another shell; mksh is the closest to the head unit's pdksh-based `/bin/sh`.

### Installing a build by hand

`python tools\package.py` writes `dist/ID6Term/` with the five plugin files plus `install.sh` / `uninstall.sh`;
copy it to a USB stick or `scp` it to `/tmp` and run `sh install.sh` on the head unit as the README describes.
`install.sh` copies into `/opt/hmi/ID5/data/ro/Plugins/ID6Term` and `chmod 755`s the library; `uninstall.sh`
removes that folder (history/config in `/var/opt/id6term` are kept). A hand-installed copy must be removed before
the NBTApps package can install: the agent refuses to write over files that no package placed (`stock-mismatch`).

## Releasing

A release is the GitHub release `vX.Y.Z` with two kinds of asset: the plain `dist/ID6Term/` folder zipped, for
installing by hand, and the NBTApps package (`.nbtpkg` + `nbtpkg-record.json`). The version is `ID6TERM_RELEASE`
in `src/id6term_app.cpp`; `make_db.py` and the README footer text should say the same.

### The NBTApps package

ID6Term is a **community package** in the NBTApps catalogue (`https://catalogue.nbtapps.com`), id
`net.josephworks.id6term`. The catalogue repo, [nbtapps-packages](https://github.com/tom-pryor/nbtapps-packages),
holds only the listing and a record per version; the archive itself is a GitHub release asset of this repo, and the
record pins its SHA-256.

- `nbtapps/manifest.json.in` is the package manifest: `payload/ID6Term/` -> `${plugins}/ID6Term/` (mode 0644),
  `reboot: hmi-restart`, `requires.agent >=1.2.0`, and `logs: /dev/shmem/id6term-app.log` so the phone app can fetch
  the log. No hooks, so the only risk flag is `needs-reboot`. `@VERSION@` is filled in at build time.
- `nbtapps/listing.json` is the catalogue listing (`packages/<id>/listing.json` in the PR): summary <= 80 chars,
  category, a description that says what it installs, writes and needs.
- `tools/nbtpkg_build.py` runs `make_db.py` + `package.py`, assembles `dist/nbtpkg/src/` (manifest, payload,
  `licenses/LICENSE`), builds and signs the archive with `nbtpkg.py build`, verifies it with the public key, and
  writes the record files.

It needs a clone of `tom-pryor/nbtapps` beside this repo (for `headunit/tools/nbtpkg.py` and the manifest schema;
`--nbtapps PATH` for another place) and `pip install cryptography`.

#### Signing key

Every archive is signed with an Ed25519 key of the author's; the public half is in the catalogue repo as
`packages/<id>/author.pub` and may not change from version to version (a change is treated as a change of owner).
Make it once and keep the private half out of every repo (`*.key` is in `.gitignore`):

```powershell
python tools\nbtpkg_build.py --keygen josephworks    # -> ~\.nbtpkg\josephworks.key (private) and josephworks.pub
```

#### Steps

```powershell
# 1. bump ID6TERM_RELEASE, commit, tag: the record's source.commit must be the commit the archive is built from
git commit -a -m "Release X.Y.Z"; git tag vX.Y.Z

# 2. build and sign (~/.nbtpkg/... or %USERPROFILE%\... work too)
python tools\nbtpkg_build.py --key "$env:USERPROFILE\.nbtpkg\josephworks.key" --note "What is new" --note "..."
```

The script prints the archive's SHA-256, the risk flags, the plugin ids to name in the PR, and warns when the
working tree is not a clean commit. It writes to `dist/nbtpkg/`:

| File | Goes to |
|---|---|
| `net.josephworks.id6term-X.Y.Z.nbtpkg` | The GitHub release `vX.Y.Z`, under exactly that name; never changed once published |
| `nbtpkg-record.json` | The same release. The catalogue's bot reads it and opens the PR for later versions itself |
| `versions/X.Y.Z.json` | `packages/net.josephworks.id6term/versions/X.Y.Z.json` in nbtapps-packages |
| `listing.json` | `packages/net.josephworks.id6term/listing.json` in nbtapps-packages |

3. Publish the release with both assets. The record's `url` is
   `https://github.com/josephworks/ID6Term/releases/download/vX.Y.Z/net.josephworks.id6term-X.Y.Z.nbtpkg`.
4. For the first version (and for a listing change) open the pull request by hand: fork nbtapps-packages, add
   `packages/net.josephworks.id6term/{author.pub, listing.json, versions/X.Y.Z.json}` (`author.pub` =
   `josephworks.pub`), run its check with the nbtapps clone beside it, then fill in the PR template (what it does
   on a head unit, plugin ids, what it was tested on):

   ```sh
   python3 tools/check.py --nbtapps ../nbtapps/headunit
   ```

   Later versions: once the release is up, the bot proposes the PR from `nbtpkg-record.json` within an hour or so.
   A version marked pre-release on GitHub becomes a `"channel": "prerelease"` record.

A published version never changes: a fix is a new version number and a new archive.

### Plugin ids

The catalogue hands out the connector and focus ids so that no two packages share one; the taken ones are listed at
`https://catalogue.nbtapps.com/v1/hmi-ids.json`. ID6Term uses connector **300** (`res/ID6Term.manifest`) and entry
focus **32760** (`res/ID6Term.xml`). It used 211 until 1.0.1, which is NBTApp's own plugin id: the agent would
refuse the install with `hmi-id-taken`. The plugin's folder name `ID6Term` must also be unique among the plugins in
`/opt/hmi/ID5/data/ro/Plugins` and `/var/opt/hmi/data/ID5/Plugins`.

## License

Contributions are under the PolyForm Noncommercial License 1.0.0 like the rest of the project ([LICENSE](LICENSE)).
Keep the `Required Notice` lines, and never commit the BMW libraries in `lib/` or a private signing key.
