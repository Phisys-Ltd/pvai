# pvai

The daemon behind PhiVoice AI: it makes Claude Code, Codex and Kimi Code
sessions on a host reachable for an app, so prompts can be spoken instead of typed and the
end of each turn can be read aloud. One C++20 binary for Linux and macOS,
without Qt; the app talks to it over the JSON protocol in
`docs/PROTOCOL.md`.

## Shape

`pvai` is one binary with these parts:

- `pvai run [--name N] -- claude …` starts Claude Code in a pseudo-terminal
  and passes the real terminal through, so the session looks and works as
  always; prompts from the app are typed into it (bracketed paste, then
  Return) only while its prompt is free.
- `pvai hook` is what Claude Code's hooks call (`SessionStart`,
  `UserPromptSubmit`, `Stop`, `Notification`, `PermissionRequest`); it tells the wrapper where
  the turn stands. `pvai run` puts them into `~/.claude/settings.json`
  when they are missing or name another pvai binary, and sets the status
  line to `pvai statusline` (context window and rate limits for the app; an
  existing status line command keeps printing through `--chain`); the rest
  of the file stays as it is, the old one is kept as `settings.json.pvai-bak`.
  `pvai install-hooks` does the same on its own, `pvai uninstall-hooks`
  takes them out again.
- `pvai codex …` starts the Codex TUI on Codex's shared app server
  (`--remote unix://`), where the daemon follows its threads; Codex needs
  no wrapper and no hooks.
- Kimi Code needs no command of pvai's: the daemon runs one `kimi acp`
  (Agent Client Protocol over stdio) for the sessions the app starts.
- `pvai daemon` (started on demand by the first `pvai run`) knows the live
  sessions of the host (Claude Code under `pvai run`, Codex threads in the
  app server, Kimi sessions it started) and speaks `docs/PROTOCOL.md` on 127.0.0.1:7470.
- `pvai token` prints the host's token for the app and starts the daemon
  when none runs.

Claude Code tells pvai where a turn stands through its hooks: `Stop` carries
the final answer (`last_assistant_message`). A spoken prompt starts with
`🎙 `; the `SessionStart` hook gives every pvai session the convention, so
Claude ends such a turn with a `🔊 ` line meant for the ear. An Esc that
interrupts a turn fires no hook: the wrapper reports the key and the daemon
looks for Claude Code's interrupt note in the session transcript.

## Build and use

Installation (apt, Homebrew, source), setting up a user, connecting the
app, updating and removing: see [`docs/INSTALL.md`](docs/INSTALL.md).

The commands:

```
pvai run -- claude                   # instead of plain `claude` (writes the hooks when needed)
pvai install-hooks / uninstall-hooks # the hooks and status line in ~/.claude/settings.json
pvai install-service                 # the daemon as a systemd user service, up after every boot
pvai codex                           # instead of plain `codex`
pvai start [--name L] DIR            # Claude Code without a terminal (as the app's +)
pvai attach <session>                # look at / type into it; Ctrl+\ leaves it running
pvai listen 192.168.1.10            # direct access without ssh (TLS, port 7471)
pvai pair                           # a link and QR code for one device, 10 minutes
pvai devices / pvai revoke <name>   # paired devices
pvai answer <session> allow|deny     # the dialog it waits on
pvai clear <session>                 # empty its prompt box (a draft, a command left in shell mode)
pvai forget <dir> <id>               # delete an ended Claude Code conversation for good
pvai sessions                        # what runs on this host
pvai watch                           # what an app would see, for checks
pvai send --voice <session> "text"   # a prompt without the app
pvai propose <host>/<session> "text" # a message to another session, released by the owner in the app
pvai peers                           # the sessions it can go to, also on other hosts (from the app)
```

## Licence

Apache License 2.0, see `LICENSE`. `third_party/qrcodegen` is Nayuki's QR
Code generator under the MIT licence (its own `LICENSE` there). The
PhiVoice AI app for macOS is a separate, proprietary product of Phisys Ltd.
