# pvai protocol

Between the PhiVoice AI app and `pvai daemon` on each host. Both sides own
this file; a change lands here first, then in both implementations.

## Transport

- TCP, **loopback**: `pvai daemon` listens on `127.0.0.1:7470`. Direct
  access over the network is a separate, opt-in TLS port (see below).
- The Mac app reaches its own Mac directly and every other host through an
  ssh port forward it starts itself:
  `ssh -N -o BatchMode=yes -o ExitOnForwardFailure=yes -L <free port>:127.0.0.1:7470 <dest>`.
  `<dest>` is the user that runs Claude Code on that host (`root@taifun`).
- Framing: one JSON object per line, UTF-8, ended by `\n`. Every object has
  a `type`. Unknown types and unknown fields are ignored, on both sides.
- Token: the daemon creates `~/.config/pvai/token` (32 random hex chars, mode
  0600) on its first start; `pvai token` prints it. The app reads it with
  `ssh <dest> pvai token` (locally: runs `pvai token`) before it connects. It
  keeps other local users of a host out; ssh does the rest.
- Keepalive: the app sends `ping` every 15 s, the daemon answers `pong`. No
  traffic for 45 s means the link is dead; the app reconnects with backoff
  (1, 2, 5, 10 s, then every 10 s).

## Direct access (draft)

A host can also be reached without ssh, straight over the network. This is
off until the owner turns it on, and ssh stays as the fallback outside the
App Store. Whoever talks to the daemon can make an agent run shell commands,
so this path must hold up like ssh.

- `pvai listen ADDR[:PORT]` turns it on (default port `7471`, no default
  address: the owner names the interface, or `0.0.0.0` on purpose; IPv6 as
  `[ADDR]:PORT`); `pvai listen off` turns it off, `pvai listen` shows it.
  Kept in `~/.config/pvai/listen`; the daemon picks it up without a
  restart, and `pvai listen` starts the daemon. Only a running daemon
  listens: after a reboot, until something starts it (`pvai listen`, the
  first `pvai run`), the host is not reachable directly.
- Identity: the daemon's Ed25519 key `~/.config/pvai/server.key` (0600) with
  a self-signed certificate, made on the first `listen` or `pair`. Each app
  install has its own Ed25519 key and self-signed certificate (on the Mac in
  the keychain). A **fingerprint** is the SHA-256 of a certificate's
  SubjectPublicKeyInfo (DER), written in lowercase base32 without padding.
- TLS 1.3 only, ALPN `pvai/1`, client certificate required. The daemon
  accepts a client whose fingerprint is in `~/.config/pvai/devices.json`
  (`[{"fingerprint", "name", "added", "lastSeen"}]`) and, while a pairing
  window is open, any client for one `pair` message. Everyone else fails in
  the handshake and never reaches the JSON.
- After the handshake the framing is the same as on loopback. `hello` needs
  no `token` here: the client certificate is the login.
- The app pins the daemon's fingerprint from pairing and refuses any other,
  with a loud warning; a new server key means pairing every device again.

### Pairing

1. On the host, `pvai pair [--name NAME] [--host HOST]` opens a window of
   10 minutes for one device and prints a link (and a QR code of it):
   `pvai://pair?host=<address>&port=<port>&fp=<daemon fingerprint>&code=<secret>&name=<hostname>`.
   `code` is 128 random bits in lowercase base32 (26 characters). `host` is
   `--host`, else the listening address, else (listening on a wildcard) the
   host's first non-loopback IPv4 address; `name` is `--name`, else the
   short hostname. The command waits and prints the device's name once it
   is paired, or that the window closed; ending it closes the window. A new
   `pvai pair` replaces an open window.
2. The app connects to `host:port`, checks the daemon against `fp`, shows
   its own certificate, and sends one line:
   `{"type": "pair", "name": "<device name>", "proof": "<hex>"}`, where
   `proof` is HMAC-SHA256, in lowercase hex, keyed with the 16 bytes the
   code encodes (base32-decoded), over 32 bytes of TLS exporter output
   (RFC 8446 7.5, label `EXPORTER-pvai-pair`, empty context). `name` is
   stored without control characters and cut to 64 bytes. A man in the
   middle has a different TLS connection, hence a different exporter value,
   and cannot make a valid proof.
3. The daemon answers `paired` (`host`, `fingerprint` of the device) and
   adds the device, or `rejected` with `reason` `bad-proof` (also for a
   line that is no well-formed `pair`) or `no-window`; either way it closes
   the connection and the window. The app then connects again as a paired
   device.

Over an ssh link the app may pair itself: it runs `pvai pair --link` over
ssh (prints only the link and returns; the window stays open for its 10
minutes) and pairs at once, without the owner typing.

### Devices and limits

- `pvai devices` lists paired devices with their last contact;
  `pvai revoke <name|fingerprint>` removes one and closes its connections.
  In `devices.json`, `added` and `lastSeen` are Unix time (`lastSeen` 0
  before the first login).
- Before login only the handshake and, in a pairing window, one line of at
  most 4 KiB are read, all within 10 s of the connection.
- Failed handshakes, proofs and pre-login timeouts count per source
  address: after 5 in a minute that address is dropped for 60 s, doubling
  up to an hour; a blocked address is closed right after `accept`, before
  any TLS.
- Every login, pairing, revoke and failure goes to the daemon's log with the
  peer address.

## Sessions

A session is one Claude Code process under `pvai run`: started by the owner
in a terminal, or by the daemon on the app's `start`, without a terminal
(`detached`; `pvai attach <id>` on that host shows it in a terminal, Ctrl+\
leaves it running). Or it is a Codex thread (see "Codex sessions"). The
daemon knows every live one on its host.

```json
{"id": "w1a2b3c4d", "assistant": "claude", "label": "phi-core", "cwd": "/root/projects/phi-core",
 "state": "idle", "queued": 0, "draft": false, "detached": false, "restartable": true,
 "dialog": null, "usage": null, "claudeSession": "4f3b744c-..."}
```

- `id`: opaque, stable while it lives: chosen by the wrapper (`w` + 8 hex
  digits) for Claude Code, Codex's thread id for Codex.
- `assistant`: the `name` of its assistant in `welcome.assistants`: `claude` or `codex`.
- `label`: `pvai run --name`, else the basename of the working directory.
- `state`:
  - `starting`: wrapper up, Claude Code has not reported `SessionStart` yet.
  - `idle`: the prompt is free; a prompt is typed in at once.
  - `busy`: Claude is working on a turn; prompts are queued. A turn ends
    with Claude Code's `Stop` hook, or when the owner interrupts it with Esc
    (seen in the transcript within about half a second). A typed prompt
    that Claude Code does not confirm with `UserPromptSubmit` within 15 s
    (it answers "Not logged in" without a hook, for one) ends the turn
    too. A slash command
    sent as a prompt (`/compact`, `/context`) fires no hook: it ends when
    its output appears in the transcript. One that opens a picker
    (`/model`, `/usage`) stays `busy` until the picker is closed in the
    terminal.
  - `waiting`: Claude waits on a dialog (`dialog` says which); prompts are
    queued and are **never** typed into the dialog. The app can `answer`
    it, the owner can answer it in the terminal.
- `queued`: prompts waiting for `idle`. They go in one at a time, oldest first,
  each when the previous turn has ended.
- `draft`: the owner has typed into the session's prompt in the terminal and
  not sent it. Prompts wait until the prompt is empty again (Return, Ctrl+C,
  Ctrl+U or deleting the text), so they never land in the middle of the
  owner's words. Worked out from the keys; it errs towards `true`.
- `detached`: started by the daemon; only these can be `stop`ped.
- `restartable`: `restart` can start this session again in place, going on
  with the same conversation: Claude Code sessions (in a terminal or
  detached) once Claude Code's own session id is known (`claudeSession`,
  from its first hook) and under a `pvai run` of 0.5.0 or later (older
  wrappers keep running after an update of pvai and cannot); never Codex
  threads, whose `usage.version` is the app server's.
- `dialog`: while `waiting`, `permission` (Claude asks to use a tool),
  `question` (any other notification that waits on the owner) or `trust`
  (a new session asks whether to trust its folder; state `waiting` before
  Claude Code has started), else `null`.
- `usage`: what `/context` and `/usage` would show, in short, from Claude
  Code's status line input (so `pvai install-hooks` sets the status line to
  `pvai statusline`); `null` until the first status line:
  ```json
  {"model": "Opus 5.5",
   "context": {"percent": 5, "tokens": 50971, "size": 1000000},
   "fiveHour": {"percent": 26, "resetsAt": 1793220000},
   "sevenDay": {"percent": 10, "resetsAt": 1793710800},
   "costUsd": 0.0034, "version": "2.1.314"}
  ```
  `percent` may be `null` before the first answer; `fiveHour`/`sevenDay`
  are `null` until Claude Code knows them (after the first request) and are
  the account's limits, the same in every session of that login; `resetsAt`
  is Unix time. `version` is the version of Claude Code this session runs
  (from the status line input); it can be older than `welcome.installed`
  after an update, until the session restarts.
- `claudeSession`: Claude Code's own session id once known, else `null`
  (always `null` for Codex).

## Messages

App to daemon:

| type | fields | meaning |
|---|---|---|
| `hello` | `token`, `client`, `version` | first line on every connection; `token` only on loopback (see "Direct access") |
| `prompt` | `ref`, `session`, `text`, `voice`, `from`, `fromLabel` | type `text` into that session; `ref` is the app's id for the answer; `voice: true` when the text was spoken; `from` and `fromLabel` only for an agent message the owner released (see "Agent messages") |
| `start` | `ref`, `cwd`, `label`, `args`, `assistant`, `resume` | start a session in `cwd` (absolute) without a terminal; `label` optional; `assistant` one of `welcome.assistants` (default `claude`); `args` optional strings: extra arguments for `claude`; for Codex only `-m`/`--model <model>` is read; `resume` optional: an id from `conversations`, to go on with that conversation instead of a new one (see "Ended conversations") |
| `conversations` | `assistant`, `cwd` | the ended conversations of that assistant in `cwd` (see "Ended conversations") |
| `update` | `ref` | update every assistant on this host to its latest version (see "Versions and updates") |
| `answer` | `ref`, `session`, `choice` | answer the session's dialog: `allow` (the first choice of a permission or question, "Yes, I trust this folder" for trust) or `deny` (Esc: the tool is refused and the turn ends; for trust, Claude Code exits) |
| `interrupt` | `ref`, `session` | stop the running turn, like Esc in the terminal (into a dialog: deny); nothing happens while `idle`, `starting` or on the trust dialog |
| `stop` | `ref`, `session` | end a detached session |
| `restart` | `ref`, `session` | end the session's process and start it again, going on with the same conversation (see "Restart") |
| `projects` | | the directories Claude Code has run in on this host |
| `browse` | `path` | the subdirectories of `path` |
| `subscribe` | `session`, `limit` | follow that session's conversation: `history` with up to `limit` (default 200) of the last entries, then `entry` for each new one |
| `unsubscribe` | `session` | stop following it |
| `dismiss` | `ref`, `proposal` | the owner sent or dropped that agent message; the daemon forgets it |
| `propose` | `ref`, `to`, `text`, `from` | from `pvai propose` only, on loopback (ignored over TLS): an agent message for the owner to release (see "Agent messages"); answered with `proposed` (`ref`, `id`, `apps`) or `rejected` |
| `ping` | | keepalive |

Daemon to app:

| type | fields | meaning |
|---|---|---|
| `welcome` | `host`, `version`, `assistants`, `installed`, `conversations` | answer to a good `hello`; `host` is the short hostname; `assistants` what `start` can start on this host, see "Assistants"; `installed` the version of each, see "Versions and updates"; `conversations: true`: the daemon answers `conversations` and `start.resume` |
| `sessions` | `sessions: [Session]` | the full list, sent after `welcome` and after every change of any session (state, queue, label, start, end) |
| `accepted` | `ref`, `session`, `delivery` | `delivery` is `typed` (went in now) or `queued` |
| `rejected` | `ref`, `session`, `reason` | to `prompt`: `unknown-session`, `empty`, `too-long` (over 32 KiB); to `start`: `bad-cwd`, `unknown-assistant`, `no-assistant` (Codex's app server is not reachable, yet), `bad-start` (Codex refused), `unknown-conversation` (no such `resume` id); to `answer`: `unknown-session`, `no-dialog`, `bad-choice`; to `stop`: `unknown-session`, `not-detached`; to `interrupt`: `unknown-session`; to `restart`: `unknown-session`, `not-restartable`, `busy` (only from `idle` without a `draft`); to `propose`: `empty`, `too-long`, `bad-target`, `too-many` |
| `started` | `ref`, `session` | the new session's id; it shows up in `sessions` when its wrapper is up (`starting`) |
| `answered` | `ref`, `session` | the keys went in |
| `interrupting` | `ref`, `session` | the Esc went in; the session turns `idle` when the transcript shows the interrupt, or after about 4 s anyway, then with its queue dropped (a hung session, such as one without a login) |
| `restarting` | `ref`, `session` | the session is ending for a restart; it goes `starting`, then `idle` again with the same `id` |
| `stopping` | `ref`, `session` | the session is told to end; it leaves `sessions` when it has |
| `projects` | `dirs: [path]` | most recently used first |
| `browse` | `path`, `dirs: [name]`, `ok` | sorted, hidden ones left out; `ok: false` when `path` cannot be read |
| `say` | `session`, `text`, `voice` | a turn ended; `text` is what to speak; `voice` tells whether the turn's prompt was spoken |
| `attention` | `session`, `kind`, `text` | the session started waiting on a dialog; `kind` as `dialog`; `text` is Claude Code's notification message (for `trust`: the folder) |
| `history` | `session`, `entries: [Entry]` | answer to `subscribe`, oldest first; sent again, to every subscriber, when the session moves to another transcript (`/clear`): the view starts over |
| `usage` | `session`, `usage` | the session's `usage` changed (not sent for a change of `costUsd` alone) |
| `conversations` | `assistant`, `cwd`, `conversations: [Conversation]` | answer to `conversations` |
| `updated` | `ref`, `installed`, `changed`, `errors` | answer to `update` |
| `proposals` | `proposals: [Proposal]` | the agent messages waiting for the owner, sent after `welcome` and after every change (see "Agent messages") |
| `dismissed` | `ref`, `proposal` | answer to `dismiss`; `proposal` is gone from `proposals` (also when it was unknown) |
| `entry` | `session`, `entry: Entry` | one new piece of the conversation |
| `pong` | | keepalive |
| `error` | `code`, `text` | `auth` (then the daemon closes), `bad-message`, `unknown-session` (`text` is the id) |

## Agent messages (draft)

An agent on one host can write to a session on another host, but only
through the owner. There is no way, on any host, to type a prompt into a
session without the app: the CLI has no `pvai send` (removed; it was for
tests), and the daemon never turns anything into a prompt on its own.
Every prompt goes back to a person who typed, spoke or released it in the
app. That keeps Claude Code and Codex on subscription logins in plain
interactive use, and no agent can steer the others.

The flow:

1. The agent runs `pvai propose TO TEXT` (or the text on stdin) in its
   session. `TO` is `host/session`: `host` is the target daemon's short
   hostname (`welcome.host`), `session` a session id or a label. A bare
   `host` means "whichever session the owner picks".
2. `pvai propose` hands it to its own daemon over loopback:
   `{"type": "propose", "ref", "to", "text", "from"}`, where `from` is
   `PVAI_ID` from the environment (set by `pvai run`; empty in a Codex
   thread, then only the host is known). The daemon answers `proposed`
   with the proposal's `id` and `apps` (how many apps are connected now;
   connections whose `hello.client` is `pvai`, the CLI's own, do not count)
   or `rejected` (`empty`, `too-long` over 32 KiB, `bad-target`,
   `too-many`). `TO` is a bad target unless `host` is 1 to 64 letters,
   digits, `-`, `.` or `_`, and `session`, when given, 1 to 128 bytes
   without control characters.
   The CLI prints that the message waits for the owner and returns at
   once; it never waits for an answer.
3. The daemon keeps the proposal and lists it in `proposals` to every
   connected app:
   ```json
   {"id": "p3f9a01c2", "from": "w8bb65867", "fromLabel": "phi-claude-voice",
    "to": "taifun/wd718313b", "text": "...", "at": 1793300000}
   ```
   `fromLabel` is the sender session's label, empty when the daemon does
   not know `from` (a Codex thread). At most 16 open proposals per daemon
   (then `too-many`), 10 a minute per sender session (all senders without
   `from` share one budget), each kept for 24 hours. They live in memory only: a restart of
   the daemon drops them.
4. The app shows the proposal to the owner, in the target session's tab
   when it knows the host and the session, else in the sender's tab:
   sender, target and the full text, with *Send*, *Edit* and *Drop*.
   There is no "always allow": each message is released by hand (click,
   or the spoken "senden").
5. On *Send* the app sends a normal `prompt` to the target host, with
   `from: "M4/w8bb65867"` (sender host and session) and `fromLabel` (the
   proposal's, which only the sender's daemon knows). The target daemon
   types this first line before the text, so the receiving agent knows
   who wrote and where to answer:
   ```
   [Message from M4/w8bb65867 (phi-claude-voice), released by the owner. Answer with: pvai propose M4/w8bb65867 "..."]
   ```
   Control characters in `from` and `fromLabel` are dropped, `from` is cut
   to 128 bytes and `fromLabel` to 64; without `fromLabel` the parentheses
   are left out. The line counts toward nothing: the 32 KiB limit is on
   `text` alone.
   Then the app sends `dismiss` to the sender's daemon. *Drop* sends only
   `dismiss`.

Answers go the same way, as a new proposal from the other side. Nothing
here waits or blocks: a proposal the owner never releases simply expires.

`pvai propose` is no lock: any process of the same user can read the
token and talk to the daemon. The rule is that pvai offers no path from
an agent to a prompt; it is not a sandbox around the agent.

## Codex sessions

On a host with Codex (`codex` on the PATH or in `~/.local/bin`) the daemon
connects to Codex's shared app server (its per-user daemon, which `pvai`
starts with `codex app-server daemon start` when it does not run) and
shows its threads as sessions with `assistant: "codex"`: the ones `start`
made and those of Codex TUIs on the host. The app sees the same messages as
for Claude Code; the differences:

- `state` comes from the thread: `busy` while a turn runs, `waiting` while
  Codex waits for an approval (`dialog: "permission"`: a command, a file
  change, more permissions) or an answer (`"question"`: a question of the
  model, an MCP server's form). Never `starting`; `draft` is always
  `false`.
- `prompt` starts a turn; while one runs, prompts are queued as for Claude.
  Slash commands are Claude Code's: Codex gets them as plain text.
- `interrupt` interrupts the running turn (an open approval is refused
  first).
- `answer`: `allow` accepts the approval (for a question: the first option
  of each); `deny` refuses it and ends the turn.
- `stop` works on threads this daemon started (`detached: true`): it
  archives the thread, which unloads it from the app server; `codex resume`
  no longer lists it, but it is kept: `conversations` lists it and
  `start.resume` unarchives it. TUI threads and threads from before a
  daemon restart are not `detached`.
- When a TUI started with `pvai codex` ends, its threads leave `sessions`:
  the daemon unsubscribes them (which unloads them unless another client
  holds them) and does not take them on again while they stay loaded. A
  TUI's threads are the one `codex resume <id>` named and every thread the
  daemon takes on while that TUI is the only `pvai codex` running in the
  thread's folder; with two TUIs in one folder, new threads belong to
  neither. Detached threads never leave this way. Threads of a plain
  `codex` (without pvai), and all threads after a daemon restart, stay
  loaded and stay sessions until `stop` or the app server unloads them.
- `usage`: `model`, `context` from Codex's token count of the last request
  (`tokens` its total, `size` the model's context window; for a thread that
  ran before the daemon saw it, the last count in its rollout file; `0` and
  `null` before the first turn), `fiveHour`/`sevenDay` from the account's rate
  limits, `costUsd` always `null`, `version` the app server's (from its
  user agent). The app server updates itself, apart from the `codex` CLI
  that `update` updates, so its version can differ from
  `installed.codex` either way: compare versions, not strings, before
  calling a session out of date.
- Entries: `prompt`, `text` (commentary and the final answer), `tool`
  (`shell` with the command, `edit` with the paths, `web_search`, MCP tools
  as `server.tool`), `result` (command output, failed edits and tools),
  `interrupted`, `command` (`/compact` for a context compaction).
- Slash commands are Codex TUI features and reach a thread as plain text,
  except a prompt that is exactly `/compact`: it compacts the thread
  (`thread/compact/start`) and runs as a turn.
- Spoken turns: threads `pvai` starts get the 🔊 convention as developer
  instructions. TUI threads do not; their `say` falls back to the first
  sentence.
- A plain `codex` TUI runs its threads in the shared app server only when
  its version matches the server's (the server updates itself); otherwise
  it runs them in its own process and the daemon does not see them.
  `pvai codex [ARGS...]` (or `pvai run -- codex ...`) starts the TUI as
  `codex --remote unix:// -C "$PWD" ...`, which always uses the shared
  server, and starts the server and the daemon first when they do not run.
  It runs the TUI as its child and stays connected to the daemon with
  `{"type": "attach", "assistant": "codex", "cwd", "resume", "pid"}`
  (local only, nothing is answered) until the TUI ends.
  A TUI thread shows up with its first prompt (within about 15 s); prompts
  from the app go into it like the owner's.

## Assistants

`welcome.assistants` lists what `start` can start on this host, in the
order the app should offer them:

```json
[{"name": "claude", "title": "Claude Code", "models": ["opus", "sonnet", "haiku"]},
 {"name": "codex", "title": "Codex", "models": ["<a Codex model>", "..."]}]
```

`name` is what `start.assistant`, `conversations.assistant` and
`Session.assistant` carry; `title` is for people; `models` are the values
`start` takes as `args: ["--model", m]` (Claude Code's aliases; Codex's
non-hidden models from its app server, empty until it answered). The app
knows no assistant by name: a new one (Moonshot's Kimi, say) shows up here
and works without a change to the app.

## Ended conversations

The app can go back to a conversation whose session has ended (the tab was
closed, the host restarted):

1. `{"type": "conversations", "assistant": "claude", "cwd": "/root/projects/x"}`
   is answered with
   `{"type": "conversations", "assistant", "cwd", "conversations": [Conversation]}`,
   `assistant` and `cwd` echoed; newest first, at most 20:
   ```json
   {"id": "4f3b744c-...", "title": "the first prompt, about 80 characters", "at": 1793300000}
   ```
   `at` is the last activity (Unix time). An unknown or not installed
   assistant, or a `cwd` that is not absolute, gets an empty list. Right
   after a daemon restart a running Claude Code session can still show up
   here: the daemon learns its session id with its next hook.
   - Claude Code: the transcripts in `~/.claude/projects/<cwd with every
     character but letters and digits as ->/*.jsonl`; `id` is the
     session id, `title` the first prompt the owner wrote (without a
     leading 🎙; transcripts with none, such as only a `/model`, are left
     out). Sessions that run now are left out.
   - Codex: the app server's threads with that `cwd`, archived ones
     included (`stop` puts them there); `title` the thread's name, else its
     first prompt. Loaded threads (live sessions) and subagents' threads
     are left out.
2. `start` with `resume: "<id>"` (and `assistant`, `cwd`) goes on with it:
   Claude Code as `claude --resume <id>` plus `args` (a detached session
   that reads the transcript), Codex by unarchiving and resuming the
   thread (a detached session; a thread that is still loaded only becomes
   `detached`). An id that is not there is `rejected` with
   `unknown-conversation`.

## Versions and updates

- `welcome.installed`: the version of each assistant installed on this
  host, by `name`, e.g. `{"claude": "2.1.314", "codex": "0.166.0"}` (from
  `claude --version` and `codex --version`, read at start and again on a
  `hello` when older than 10 minutes). A missing key means unknown. The
  field's presence means the daemon understands `update`.
- `Session.usage.version`: the version the session runs (see "Sessions"
  and "Codex sessions").
- `{"type": "update", "ref"}` runs `claude update` and `codex update` (one
  at a time per assistant; a second `update` while one runs only reports)
  and answers
  ```json
  {"type": "updated", "ref": 1, "installed": {"claude": "2.1.314", "codex": "0.166.0"},
   "changed": {"codex": ["0.162.1", "0.166.0"]}, "errors": {"claude": "..."}}
  ```
  `changed` and `errors` are `{}` when nothing happened; an error is the
  last line of the updater's output (e.g. "needs root"), "timed out" after
  10 minutes, or "not installed". Running sessions are never stopped or
  restarted for an update: they keep the version they started with until
  they end, and new sessions get the new one.

## Restart

A session whose `usage.version` is older than `welcome.installed` gets the
new version by starting again; `restart` does that without losing the
conversation or the tab:

- Only a `restartable` session in `idle` without a `draft`; a running turn,
  an open dialog or the owner's unsent words are refused with `busy`.
- The wrapper ends Claude Code (SIGHUP, after 5 s SIGKILL) and starts it
  again in a fresh pseudo-terminal of the same `pvai run`, with the same
  arguments except that `--continue` and `--resume [id]` give way to
  `--resume <claudeSession>`. In a terminal the owner sees Claude Code
  start again; `pvai attach` viewers stay attached.
- The session keeps its `id`, label and queue. It is `starting` until
  Claude Code's `SessionStart` (then `idle`, and what was queued goes in);
  if Claude Code writes the resumed conversation into a new transcript,
  subscribers get `history` again. `usage.version` follows with the first
  status line.
- An initial prompt given as an argument of `pvai run -- claude "..."`
  would be sent again; sessions the app starts never have one.

## Conversation entries

The daemon follows the session's transcript (Claude Code's own file, whose
format is internal and changes) and passes on what the owner should see, in
this stable shape. Entries arrive when Claude Code writes them: a text block
or a tool call when it is complete, not token by token, usually within half
a second.

```json
{"id": "886a7876-...", "time": "2026-10-28T20:19:35.872Z", "kind": "text",
 "text": "Done: the echo printed `pvai-ok`.", "tool": "Bash", "error": false}
```

| `kind` | fields | what it is |
|---|---|---|
| `prompt` | `text` | what the owner sent, typed or spoken (with its `🎙 `) |
| `text` | `text` | Claude's words, markdown (up to 64 KiB) |
| `tool` | `tool`, `text`, `file` | a tool call; `text` is one line saying what it does; `file`, when present, names the file whose content the result shows (a `Read`, or a shell command that names exactly one file), so an app can colour it by its extension |
| `result` | `tool`, `text`, `error` | what the tool returned, cut at 4 KiB |
| `interrupted` | | the owner stopped the turn |
| `command` | `text` | a slash command that ran in Claude Code itself (`/compact`, `/context`, `/model opus`) |
| `output` | `text` | what such a command printed, terminal colours removed, cut at 4 KiB (empty output is not passed on) |

- `id` is unique per transcript; an app that reconnects and subscribes again
  replaces its view with the `history`.
- The `say` of a turn may arrive before that turn's last `text` entry.
- Thinking, subagents' inner turns and Claude Code's own bookkeeping are not
  passed on.

## What gets spoken

- A prompt with `voice: true` reaches Claude prefixed with `🎙 ` (not a slash
  command, which must start with `/`). For such
  turns Claude ends its answer with one line that starts with `🔊 `: one or
  two plain sentences for the ear, in the chat language, no code, no paths.
- At the end of every turn the daemon sends `say`. `text` is the last `🔊 `
  line of the turn's final answer, without the marker. Without such a line:
  empty for a voice turn (the app speaks its own "done, details in the
  terminal" phrase), the first sentence of the answer (up to 200 chars,
  markdown stripped) otherwise.
- The app speaks `say` with `voice: true`; `voice: false` only when the user
  turned on "speak every turn".
- `attention` is spoken as the app's "<host>: Claude asks for permission"
  (or "… asks you something", "… asks to trust a folder") phrase unless
  muted.
- Fixed phrases belong to the app: English in the source, German through its
  translation, like every phi UI string. The daemon never sends prose of its
  own.
