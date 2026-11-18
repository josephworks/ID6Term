# ID6Term - a shell terminal inside iDrive (NBT Evo / ID6)

by **Joseph Robinson**

ID6Term is a native iDrive **PluginApp** (`libID6Term.so`) that adds a **Terminal** row to the Media/Radio list
of an NBT Evo (ID6) head unit. From there you type a command line with the iDrive speller and run it through the
head unit's `/bin/sh`; the output shows in a scrollable console screen. No SSH cable, no laptop - the car's own
screen and controller.

It follows the same mechanism as NBT-AAD's iDrive app ([Supra-iDrive6-Android-Auto](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto),
`rhmiapp/`): HmiMain3D loads every `Plugins/<Name>/<Name>.manifest` under `/opt/hmi/ID5/data/ro` at boot, the
manifest names the library, the UI description XML and the text/image DBs, and iDrive itself creates and places
the menu row. Nothing in the firmware is patched.

General notes for writing *any* PluginApp (loading mechanism, XML reference, framework ABI, build setup, id
registry, a compilable skeleton) live in the workspace-level [`../docs/`](../docs/README.md).

> **Warning.** Commands run on the head unit as the HMI's user (root). A wrong `rm`, a renamed system file, a
> hung process can leave the unit unbootable; only Toyota/BMW can reflash it. There is no undo. The code is
> provided as-is, use it on your own hardware at your own risk.

**Status: 1.0.0 - car-tested** on an NBT Evo (2026-11-14): entry row, menu, speller input, suggestions, console
output and command execution all work. See [Car test results](#car-test-results) for what was confirmed and what
is still open.

## Contents

| Path | What |
|---|---|
| `src/id6term_app.cpp` | The PluginApp: entry point `pluginAppInfo`, handlers, model updates, polling |
| `src/bmw_rhmi.hpp` | Declarations of the BMW framework ABI (PluginApp, Parcel, BasicString, Variant, Map, DataTable) and safe readers for action arguments |
| `src/term_buffer.hpp` | Console text: bounded line ring, control-character / ANSI filter |
| `src/command_line.hpp` | Speller editing, history + suggestions, the `sh -c` wrapper script and its end-of-run sentinel |
| `src/shell_runner.{hpp,cpp}` | Runs one command at a time: `spawn()` + pipe + reader thread, timeout, stop |
| `res/ID6Term.xml` | UI description: entry row, menu, console, speller, quick commands, about |
| `res/ID6Term.manifest` | Tells CAppManager what to load |
| `res/make_db.py` | Generates `ID6Term_ALL_Texts.zip` (10 languages) and `ID6Term_ALL_Images.zip` (icons drawn procedurally) |
| `lib/` | `libAppFramework.A.so`, `libCoreFramework.A.so` from the head unit (`/opt/hmi/ID5/lib`), link-time only |
| `tests/` | Host tests (Linux g++) for the terminal core |
| `tools/package.py` | Collects `dist/ID6Term/` with everything the head unit needs plus `install.sh` |
| `common.mk`, `Makefile`, `arm/so-le-v7/` | QNX Momentics recursive build (shared object, armv7le, GNU libstdc++) |

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

**Host tests** (Linux g++; WSL works): `make -C tests check` builds and runs `test_term_buffer`, `test_command_line`
and `test_shell_runner` (the latter spawns the host `/bin/sh`: streamed output, `cd` persistence, timeout killing a
backgrounded child, `stop()`).

## Installing

### Prerequisites

```sh
ssh root@127.0.0.1 -o HostKeyAlgorithms=+ssh-dss -m hmac-sha1 -p 6969
# or
ssh root@169.254.X.X -o HostKeyAlgorithms=+ssh-dss -m hmac-sha1
# This will be the IP Address mentioned
# The password is ts&SK412
mount -o remount rw /net/hu-omap/fs/sda0
on -f /net/hu-omap/ /bin/sh
ls -l /mnt/
```

Download the latest release from the repository or the [Release page](https://github.com/josephworks/ID6Term/releases).

https://github.com/josephworks/ID6Term/releases

Copy `dist/ID6Term/` or the extracted Release `/ID6Term` to a USB stick (or `scp` it to `/tmp`), then on the head unit:

```sh
cd /mnt/umass01200t7/ID6Term      # or wherever the files are extracted/copied
sh install.sh
```

which does

```sh
mkdir -p /opt/hmi/ID5/data/ro/Plugins/ID6Term
cp libID6Term.so ID6Term.manifest ID6Term.xml ID6Term_ALL_Texts.zip ID6Term_ALL_Images.zip /opt/hmi/ID5/data/ro/Plugins/ID6Term/
```

Then, restart the head unit by holding the power button for ~30 seconds. Or run the following:

```sh
OnOffDSICommander appreset
```

Hold the power button ~30 s to restart. **Media/Radio** then has a **Terminal** row after the Bluetooth audio row.
(The same `Plugins` folder is where NBT-AAD's `NbtAad` lives; both can be installed side by side - ID6Term uses
different focus ids, connector id and application weight.)

Uninstall: `sh uninstall.sh` (= `rm -rf /opt/hmi/ID5/data/ro/Plugins/ID6Term`) and reboot. Kill switch without
uninstalling: `touch /var/opt/id6term/disable` - the library then refuses to create the app at the next HMI start.

## Using it

**Terminal** (menu, hmiState 10) - titled "Terminal - Joseph Robinson", with a footer line "ID6Term 1.0.0 by
Joseph Robinson". The Media/Radio row itself stays the short "Terminal". The name also appears in the console
banner, `help`, the About page and the plugin's vendor string (`ID6TERM_AUTHOR` in `id6term_app.cpp`, `AUTHOR` in
`make_db.py`).

| Row | Does |
|---|---|
| Console | The output screen: a status line (`<cwd> $  ready` / `running 3s: cmd` / `[exit N]`), an **Enter command** row (shows iDrive's busy ring while a command runs), and the output text. Turn the knob to scroll. |
| Enter command | Opens the iDrive speller. Every letter updates the line on the right; the suggestion list holds history entries and quick commands starting with what you typed - **select one to run it**. OK (the result field) runs what you typed; an empty line runs the last command again. |
| Run last command again | What it says |
| Quick commands | `uname -a`, `pidin arg`, `ifconfig`, `df`, `ls -la /`, `ls /mnt /fs /dev/shmem`, `sloginfo \| tail -n 40`, `date` |
| Stop running command | SIGTERM to the command's process group, SIGKILL 2 s later |
| Clear console | Empties the output |
| About | One text |

Behaviour:
- Each line runs as `/bin/sh -c <line>` in a **new process**: pipes, `&&`, redirections, variables all work, but
  variables and functions do not survive to the next line. **`cd` does persist** - the wrapper script reports the
  final `$PWD` and the next command starts there.
- Output is stdout + stderr, streamed into the console while the command runs. The console keeps the last 200
  lines (160 chars each max, longer ones wrap); the screen shows the newest ~6 KB. Tabs expand, ANSI colour
  sequences and control characters are dropped.
- **Timeout:** 20 s per command (then the process group is killed and `[timeout after 20 s, killed]` shows).
  Needed because nothing here is interactive: a command waiting on stdin (which is `/dev/null`) or a server would
  otherwise sit there forever. Raise it in the config for long jobs.
- Built-ins answered by the app: `clear` (`cls`), `help`, `history`.
- History: the last 50 distinct commands, in `/var/opt/id6term/history`, newest first.
- **Environment:** commands get `PATH=/bin:/usr/bin:/sbin:/usr/sbin:/opt/os/bin:/opt/sys/bin:/opt/core/bin:/opt/car/bin:/fs/sda0/opt/mm/bin:/opt/hmi/ID5/bin`,
  `HOME=/`, `TERM=dumb`, `ID6TERM=1` and the rest of HmiMain3D's environment **minus** the dynamic-linker
  variables (`LD_PRELOAD`, `LD_LIBRARY_PATH`, `LD_DEBUG*`, `LD_BIND_NOW`). The HMI is started with
  `LD_PRELOAD=libKD.so.1`; inherited, that made every command print `unknown symbol: kdMain referenced from
  libKD.so.1` (fixed in 1.0.0).

Config `/var/opt/id6term/config` (optional, `key=value` lines): `shell=/bin/sh`, `timeout=20` (seconds, 0 = none),
`cwd=/` (start directory). If `/var/opt` is not writable the app falls back to `/dev/shmem` (lost at reboot).

Logs: `slog` major `0x4954` (`sloginfo | grep ID6Term`) and `/dev/shmem/id6term-app.log` (512 KB cap). Every
speller press logs its arguments with the Variant type codes - that is how the remaining unknowns below get
confirmed.

## How it works

- **Entry row / screens:** declared in `ID6Term.xml`; iDrive renders them. Our code only reacts to `raAction`s
  and fills models. State ids 10..15, actions 5/21..23/26..28/30..37/71..74, models 3/4/12/13/31..37/40..46/50..57.
- **Speller:** the `<input>` component with `action` (per letter, argument **8** = text, or `del` / `delall`),
  `suggestAction` (argument **1** = row) and `resultAction`, as in the BMW Apps RHMI protocol that AAIdrive uses.
  Handlers for those two are registered with `addHandlerWithMap`, so the arguments arrive as
  `Map<unsigned, Variant>`; the Parcel-form handler for the same action only logs the raw bytes.
- **Models:** the typed line goes into an `raDataModel` with `updateModel(id, const char*)`; the console text is a
  1x1 `DataTable` whose cell is a `BasicString<char>` Variant (type 0x22) on a Richtext `raListModel`; the
  suggestion list is an Nx1 table of the same; About is a `TextID` cell like ActiveSeating's intro text.
- **Threads:** the HMI calls us on the app thread; model updates happen only there (handlers and the posted poll
  Runnable, 250 ms while a command runs, 1 s otherwise). The ShellRunner's reader thread only fills a byte buffer
  under a mutex; `pump()` on the app thread moves it into the console.
- **Processes:** QNX forbids `fork()` in a multi-threaded process, so the shell is started with `spawn()` and an
  fd map (`/dev/null`, pipe, pipe) in its own process group (`SPAWN_SETGROUP`), which is what the timeout and Stop kill.

### How the ABI was derived

`lib/libAppFramework.A.so` and `lib/libCoreFramework.A.so` were read with the SDP's `ntoarmv7-nm -D` and
`ntoarmv7-objdump -d`:

- `PluginApp::handleMessage` dispatches message id `0xC0000000` (an action) to vtable slot 16
  `invokeCommand(actionId, Parcel)` **and** slot 27 `invokeCommandWithMap(actionId, Map)` - so both handler forms
  fire; the Map one carries the RHMI arguments.
- `Variant::is<BasicString<char>>()` compares `container->type()` with **0x22**; `Variant::as<BasicString<char>&>()`
  returns `container + 4`. `Variant` is one pointer; the container vtable is `{~D1, ~D0, type(), clone()}`.
- `Map<unsigned, Variant>::operator[]` shows the MFC-CMap layout: `{table, hashSize, count, free, blocks, blockSize}`,
  assoc `{next, hash, key, value}`, bucket `ror(key, 4) % hashSize`. `bmw_rhmi.hpp` walks the buckets for a
  read-only `find()` instead of calling the inserting `operator[]` on the framework's map.
- `BasicString<char>::~BasicString / assignCopy` show the MFC-CString layout: data pointer with
  `{refs, length, alloc}` 12 bytes in front. `variantToString()` uses that header to read a string argument even
  when its Variant type code is not the known 0x22 (a UTF-16 `BasicString<unsigned short>` is detected from the
  character layout).
- Exported and used: `updateModel(unsigned, const char*)`, `updateModel(unsigned, const DataTableBase&)`,
  `addHandlerWithMap`, `Parcel::size/dataAvail/getRawData/readBool`, `setProperty`, `triggerEvent`, `post`.
- Everything else (vtable slot numbers 21/22/23, `kPluginAppStorage`, the Runnable stand-in, `pluginAppInfo`
  layout, manifest/XML schema, placement in state 20 after focus 2270, Richtext list + `TextID` cell, busy ring
  property 8) is taken as-is from NBT-AAD's car-proven `rhmiapp/nbt_aad_app.cpp`.

## Car test results

First car test 2026-11-14 (NBT Evo, 0.1.0 -> 1.0.0). Confirmed working:

1. **The `<input>` speller component in a PluginApp XML** is accepted and delivers letters, suggestions and OK.
   No BMW PluginApp known to us used one before - this is the first confirmation and is reusable (see
   `../docs/open-questions.md`).
2. **Action arguments through `addHandlerWithMap`** arrive and are readable with the readers in `bmw_rhmi.hpp`
   (speller text in argument 8, suggestion index in argument 1). The exact Variant type codes are in the app log
   (`speller: ... 8:type=0x..`, `suggestion: ... 1:type=0x..`) - note them down in `docs/bmw-framework-abi.md`.
3. **`BasicString<char>` cells (type 0x22) in a Richtext list** render: the console and the Nx1 suggestion list work.
4. **`spawn()` of `/bin/sh` from inside HmiMain3D** with fd map + new process group works; output streams.
5. Found and fixed: the inherited `LD_PRELOAD=libKD.so.1` ("unknown symbol: kdMain") - see Environment above.

Still unverified: the preview image next to the entry row (`previewModel`), model-size / update-rate limits (none
hit so far), umlauts through `updateModel(unsigned, const char*)`.

## License

Pick a license before publishing. Note that the framework glue in `src/id6term_app.cpp` (vtable override,
`pluginAppInfo`, the Runnable stand-in, Richtext `TextID` cell) and the ABI notes in `src/bmw_rhmi.hpp` closely
follow NBT-AAD's `rhmiapp/nbt_aad_app.cpp`, which is licensed **PolyForm Noncommercial 1.0.0**
(Copyright (c) 2026 nanaisu, https://github.com/Sq00ky/Supra-iDrive6-Android-Auto) - keep this noncommercial and
credit it. `lib/*.so` are BMW's, taken from the head unit's own firmware and included for linking only; do not
redistribute them with a release, point people at their own firmware dump instead.
