# ConsoleBridge — external Lua eval for Norbyte's Windows BG3SE

Norbyte's Windows extender has **no IPC socket** (unlike the macOS port's
`/tmp/bg3se.sock`). Its console reads Lua from stdin, and it `freopen()`s stdin
to its own `CONIN$`, so a redirected stdin pipe can't reach it. This package adds
a clean, portable request/response channel using two files in the SE user dir.

## Pieces

- `ConsoleBridge/` — an SE-only mod. Polls `bridge_req.json` a few times/sec on
  Tick, evals the code in **server context** (captures `Ext.Print` output + return
  values), writes `bridge_resp.json`.
- `bg3se_win_client.py` — external client (library `BG3SEWin` + one-shot CLI +
  `--repl`). Mirrors the macOS `bg3se_client.BG3SE` API.

Files live in the SE user directory (`Ext.IO` UserProfile root):
`%LOCALAPPDATA%\Larian Studios\Baldur's Gate 3\Script Extender\`

## Install (on the Windows machine)

1. Pack `ConsoleBridge/` into `ConsoleBridge.pak`. From the `ConsoleBridge/`
   folder (the one containing `Mods/`), with LSLib's divine.exe:
   ```
   divine.exe -a create-package -g bg3 --source . --destination ConsoleBridge.pak
   ```
2. Copy `ConsoleBridge.pak` to `%LOCALAPPDATA%\Larian Studios\Baldur's Gate 3\Mods\`.
3. Enable it in the load order (BG3MM, or add to `modsettings.lsx`). UUID is in
   `MOD_UUID.txt`.
4. Launch BG3 **with the Script Extender** and load a save. You should see
   `[ConsoleBridge] ready` in the SE console.

## Use

```
python bg3se_win_client.py "Osi.GetHostCharacter()"
python bg3se_win_client.py -f probe.lua
echo "return 1+1" | python bg3se_win_client.py
python bg3se_win_client.py --repl
```

## Notes / things to verify live

- **Ext.IO caching:** if the mod doesn't see fresh request contents, `Ext.IO.LoadFile`
  may be caching. Fix: have the client write per-request filenames, or add a version
  suffix. (Not expected — LoadFile reads from disk — but confirm on first run.)
- **Poll rate:** `POLL_EVERY = 10` ticks in BootstrapServer.lua (~6 checks/sec).
  Lower it for snappier response, raise it to reduce overhead.
- **Server context only:** eval runs where Tick fires on the server. For client-context
  calls you'd need a BootstrapClient.lua variant with its own file pair.
- This is the Windows analogue of the macOS `bg3se_client.py`; keep the two APIs in sync.
