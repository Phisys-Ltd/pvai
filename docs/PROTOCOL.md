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
  0600) on its first start; `pvai token` prints it (and starts the daemon
  when none runs). The app reads it with
  `ssh <dest> pvai token` (locally: runs `pvai token`) before it connects. It
  keeps other local users of a host out; ssh does the rest.
- Keepalive: the app sends `ping` every 15 s, the daemon answers `pong`. No
  traffic for 45 s means the link is dead; the app reconnects with backoff
  (1, 2, 5, 10 s, then every 10 s). The daemon drops a TCP or direct app
  connection that has sent nothing for 60 s, one that has not sent `hello`
  within 10 s, and one with more than 4 MB of unsent output. Local unix-socket
  peers (CLI, hooks, wrappers) are never dropped for silence.

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
   line that is no well-formed `pair`: since 0.5.52 that includes JSON without a
   `type`, JSON that is no object, and text that is no JSON at all) or `no-window`; either way it closes
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
 "state": "idle", "queued": 0, "draft": false, "detached": false, "stoppable": false, "restartable": true, "inputClearable": true,
 "dialog": null, "dialogInfo": null, "usage": null, "claudeSession": "4f3b744c-...", "subagents": [], "limited": null}
```

- `id`: opaque, stable while it lives: chosen by the wrapper (`w` + 8 hex
  digits) for Claude Code, Codex's thread id for Codex.
- `assistant`: the `name` of its assistant in `welcome.assistants`: `claude`, `codex` or `kimi`.
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
    A background task or subagent that finishes while the session is
    `idle` hands Claude Code a task notification, and Claude Code may start
    a turn of its own without `UserPromptSubmit`: once Claude answers the
    notification in the transcript (within about half a second), the
    session is `busy`; `Stop` ends that turn as any other. A notification
    that only waits for the next prompt, or one that came and was answered
    within a turn that has ended since, changes nothing.
  - `waiting`: Claude waits on a dialog (`dialog` says which); prompts are
    queued and are **never** typed into the dialog. The app can `answer`
    it, the owner can answer it in the terminal.
- `queued`: prompts waiting for `idle`. They go in one at a time, oldest first,
  each when the previous turn has ended. A prompt that is being typed
  counts until it is `delivered`.
- Kept prompts (since 0.5.52): when a Claude Code session ends with prompts
  it did not take (the queue, and a prompt typed and not yet confirmed),
  the daemon keeps them in memory for 24 hours, under the conversation
  (`claudeSession`) and `cwd`, and says so with `kept: true` in `dropped`.
  Only when `claudeSession` was known: a session that ended before its
  first hook has none, and its prompts are lost (`dropped` without `kept`).
  A session that goes on with that conversation in that folder (`start`
  with `resume`, a `pvai run` of it, an in-place `restart`, or Claude Code
  resuming it in a running session; the match is made when the session
  learns its `claudeSession`) gets the prompts queued again, in their old
  order, in front of its own queue: `queued` counts them, and each one is
  `delivered` later with the `ref` of its `prompt`, as usual. When the 24
  hours pass, when the daemon stops (`pvai daemon stop|restart`), or when
  the conversation is forgotten (`forget`), the daemon sends a second
  `dropped` for the same refs without `kept`. At most 16 prompts are kept
  per conversation and 64 in all; the oldest are pushed out first, with a
  plain `dropped` (for a prompt of the ending session itself, already in
  the first one, without `kept`). Kept prompts are not written to disk: a
  daemon restart loses them.
- Delivery under a `pvai run` of 0.5.15 or later: the wrapper pastes a
  prompt only into Claude Code's prompt box when the screen shows it free
  (the lines between two rules, starting with `❯`, no dialog's footer
  below; since 0.5.30 a box in shell mode, starting with `!`, is not free
  and counts as a `draft`), and presses Return only once the box shows the text. If it
  cannot, nothing more is typed, the prompt goes back to the front of
  `queued`, and the session is `waiting` with `dialog` `screen` and the
  lower part of the screen as `dialogInfo.text`; it goes on by itself
  once the prompt box is free. A prompt that went in with Return but got
  no `UserPromptSubmit` within 15 s (no login, an unknown dialog took
  it) goes back to the front as well and shows the screen the same way;
  that one goes on only after an `answer` from the app (any key) once the
  box is free, so a session without a login does not type it again and
  again. A prompt whose typed check failed is typed again at most 3
  times, and only after a pause of 2 s, then 4 s, so a
  screen that stays the same (a survey, a busy box) is not typed into
  over and over; the `screen` attention text is the last lines above the
  prompt box (or the dialog's first lines), up to 4 lines and 300 bytes; when
  the prompt that did not go in stands unsent in the box (since 0.5.72) it
  is `The prompt stands in the box, not sent: ` and its first 60 characters
  instead, and `dialogInfo.text` stays the screen. `answer` `send` types the first queued prompt without the free-box check
  (the owner sees that the prompt is free and the box was not recognised).
  Since 0.5.71 the wrapper still never presses Return into something that is
  not the paste: it refuses (`typed` `ok: false`) while a `screen` dialog
  stands (answer the dialog instead), and presses Return only once the box
  shows the paste; if it does not within 2 s the paste is cleared once no
  dialog stands and the prompt goes back to the front of `queued` with
  attention `stuck`; it is never retried by itself. Older wrappers type as before.
  Since 0.5.72 the wrapper decides about the box (`typed` `how` `taken`, the
  Return, the `screen` dialog) only on a settled screen: no output for 300 ms,
  and no synchronized update (DEC mode 2026, `ESC[?2026h` ... `ESC[?2026l`)
  open (one left open counts as closed after 1 s). Claude Code draws a tall
  box (a long paste) in several writes, and a screen read in between shows
  the new box above the old empty one, which looked like a prompt it had
  taken. `how` is `taken` only for a prompt that stands as a line above a
  box that no longer holds it, seen on a settled screen. A prompt whose text
  already stands in the box (a retry, or `send` after a Return that did not
  take) is not pasted a second time: only the Return follows, with the usual
  submit check.
  Since 0.5.73 (unreleased) a prompt is not pasted while Claude Code still
  draws a turn: a verified `type` that arrives on a screen that is not
  settled, or with a status line within the six rows above the prompt box
  (`✻ ... (4s`, `· Running stop hooks... (3s)`: a spinner glyph, an ellipsis
  and ` (`), waits in the wrapper until neither holds, at most 3 s; then the
  wrapper answers `typed` `ok: false` as for a busy box and the prompt is
  retried as above. The daemon used to type the next prompt right on the
  `Stop` hook, while Claude Code was still closing the turn; Claude Code
  took that Return into its own queue, the box emptied, and `UserPromptSubmit`
  never came for it. Since 0.5.73 the daemon also reads that queue from the
  transcript (`queue-operation` lines: an `enqueue` with the prompt's text
  and no `dequeue` or `remove` yet) once the wrapper has typed the prompt:
  the prompt stays in flight (counted in `queued`, not `delivered`, no
  `screen` dialog, not typed again), and nothing else is typed behind it.
  Claude Code takes the queue up after the turn and `UserPromptSubmit`
  delivers the prompt as usual. When Claude Code puts the prompt into the
  running turn itself it fires no `UserPromptSubmit`: the transcript has a
  `queue-operation` `remove` with `reason: "absorbed_mid_turn"` and the
  prompt's text, and that is the delivery (`delivered`, the turn counts as
  the prompt's for the voice rule), never a second typing. When the session
  is `idle` and the prompt is still in Claude Code's queue 20 s after the
  turn's `Stop`, it is counted as `delivered` (Claude Code merges it into
  the next prompt) and sent as `attention` `stuck` with the text `In Claude
  Code's queue: ` and its first 40 characters. It is not `dropped` (a resend
  would arrive twice) and not typed again; the prompts behind it go in.
- `draft`: the owner has typed into the session's prompt in the terminal and
  not sent it, or (since 0.5.30) the prompt box is in shell mode (it starts
  with `!`, as after `!` or a command called back from the history: what is
  typed there runs as a shell command). Prompts wait until the prompt is empty again (Return, Ctrl+C,
  Ctrl+U or deleting the text), so they never land in the middle of the
  owner's words. Worked out from the keys; it errs towards `true`.
- `detached`: started by the daemon.
- `stoppable`: `stop` can end it: detached sessions, and Codex threads that
  no `pvai codex` TUI holds (see "Codex sessions"). A Claude Code session
  in a terminal is the owner's to end there.
- `inputClearable`: `clearInput` can empty the session's prompt box now (since
  0.5.30): a Claude Code session under a `pvai run` of 0.5.30 or later,
  `idle` (with or without a `draft`) or `waiting` on a `screen` dialog that
  a prompt which did not go in opened. The app offers it where the box
  holds something: a `draft`, or the box as `dialogInfo.text` shows it.
- `restartable`: `restart` can start this session again in place, going on
  with the same conversation: Claude Code sessions (in a terminal or
  detached) once Claude Code's own session id is known (`claudeSession`,
  from its first hook) and under a `pvai run` of 0.5.0 or later (older
  wrappers keep running after an update of pvai and cannot); never Codex
  threads, whose `usage.version` is the app server's.
- `dialog`: while `waiting`, `permission` (Claude asks to use a tool),
  `question` (any other notification that waits on the owner), `trust`
  (a new session asks whether to trust its folder; state `waiting` before
  Claude Code has started) or `screen` (since 0.5.13: a dialog no hook
  reports, such as a picker or a notice to confirm, read off the
  terminal of a `pvai run` of 0.5.13 or later), else `null`.
- `dialogInfo`: while `dialog` is set, what it is about, else `null`:
  ```json
  {"tool": "Bash", "text": "git push origin main", "description": "Push the branch",
   "message": "Claude needs your permission to use Bash"}
  ```
  `tool` the tool that asks (`null` for a question); `text` what it would
  do: the command, the file path, the URL or pattern, the questions (one
  per line), the folder for `trust`, the dialog as the screen shows it for
  `screen` (title, options with the `❯` at the selected one, footer, one
  line each, common indent removed), else the tool's input as JSON;
  `description` only for a shell command that has one; `message` the
  assistant's own words (Claude Code's notification, Codex's `reason`).
  Each is cut to 2000 bytes, any may be `null`. A `screen` dialog is one
  whose footer ("Enter to confirm", "Enter to continue", "Enter to
  select", "Enter to submit", "Esc to cancel", "Esc to exit", "Esc to go
  back", "Esc to close") is in the last two lines of the screen, or (since
  0.5.37) a panel taller than the screen shows only its scroll mark `↓`
  alone in the last line (`/usage` at 40 rows; `text` is then the panel's
  lowest 30 lines); it counts
  at once at the prompt, in a turn after a second without a hook's dialog
  (a permission dialog shows such a footer too). `text` follows the
  screen while it is open (a `sessions` update for each change); when it
  is gone the session is back in the state it had before (`idle`: the
  queue goes on). Claude Code's session surveys ("How is Claude doing this
  session? (optional)", the recollection and memory variants, "Can
  Anthropic look at your session transcript ...") are drawn above the
  prompt box, which stays; they count as `screen` dialogs when question
  and options row stand directly over the box's top rule. `text` is the
  question and the options row; `dialogInfo.options` lists the keys shown,
  `[{"id": "1", "label": "Bad"}, ...]` (`1`-`3`, `4` Unsure in some
  variants, `0` Dismiss; or `y` Yes, `n` No, `d` Don't ask again). They are
  answered with `answer` `key:<id>` (one key, passed on unchanged); the
  daemon never types into one or dismisses it by itself, and keeps the
  queue back while it stands. Since 0.5.71 the same holds for an ask drawn
  under the prompt box (Claude Code's offers, such as "Teach auto mode about
  your environment?" with `1. Yes`, `2. Not now`, `3. Don't show again`, then
  "Enter to confirm · Esc to cancel"): numbered options and a footer in the
  last two lines under the box. Its select takes Return and the digits, so
  text typed for the box would answer it. `dialogInfo.options` lists the
  digits (`id` `1`...`9`) for any `screen` dialog with two or more numbered rows. Claude Code: from its
  `PermissionRequest` hook (tool name and input; `AskUserQuestion` makes a
  `question`, see "Claude Code's questions"), the words from the
  `Notification` that follows or comes first; an older Claude Code without that hook gets the last tool call
  in the transcript (its short summary) instead. Codex: from the approval
  request; `tool` is `shell` for a command, `edit` for a patch (`text` its
  files, one per line), `permissions` for more permissions (`text` the
  profile as JSON), `null` for a question.
- `usage`: what `/context` and `/usage` would show, in short, from Claude
  Code's status line input (so `pvai install-hooks` sets the status line to
  `pvai statusline`); `null` until the first status line. (The wrapper also
  marks Claude Code's onboarding as done, `hasCompletedOnboarding` and
  `theme` in `~/.claude.json`, when the keys are missing, so a fresh install
  starts without the first-run pages.):
  ```json
  {"model": "Opus 5.5",
   "context": {"percent": 5, "tokens": 50971, "size": 1000000},
   "fiveHour": {"percent": 26, "resetsAt": 1793220000},
   "sevenDay": {"percent": 10, "resetsAt": 1793710800},
   "costUsd": 0.0034, "version": "2.1.314",
   "cache": {"ttl": 3600, "at": 1793448005}}
  ```
  `percent` may be `null` before the first answer; `fiveHour`/`sevenDay`
  are `null` until Claude Code knows them (after the first request) and are
  the account's limits, the same in every session of that login; `resetsAt`
  is Unix time. `version` is the version of Claude Code this session runs
  (from the status line input); it can be older than `welcome.installed`
  after an update, until the session restarts. `cache` says how long the
  prompt cache stays warm: `ttl` the seconds it lives after a request
  (`300` or `3600`, from the cache writes of the last request that wrote
  one: `ephemeral_5m`/`ephemeral_1h`), `at` the Unix time of the last API
  request (the timestamp of Claude's last answer in the transcript); it is
  warm until `at + ttl`. `null` until a request wrote to the cache, and
  again after `/clear`; a change of it alone sends a `usage` update.
- `mode` (since 0.5.25): the permission mode the session is in, one of
  its assistant's `modes` (see "Assistants"), `null` when not known.
  Claude Code: from its hooks (`permission_mode`) and, under a `pvai run`
  of 0.5.25 or later, from the footer under the prompt box ("plan mode
  on", "accept edits on", "auto mode on"), so that a Shift+Tab in the
  terminal shows at once. Codex: the thread's approval policy. Kimi: its
  ACP mode. A change sends a `sessions` update.
- `claudeSession`: Claude Code's own session id once known, else `null`
  (always `null` for Codex). It survives a restart of the daemon (kept
  beside the wrapper's socket) once a hook has named it. With it the
  state does too (since 0.5.22): a session whose transcript has a turn
  going on (a prompt or an answer after Claude Code's last turn end) is
  `busy` after the restart, else `idle`.
- `wrapperVersion` (since 0.5.52): the version of the `pvai run` that holds
  a Claude Code session, from `version` in its `register` message; `null`
  for Codex and Kimi sessions and for a wrapper before 0.5.52, which does
  not send it.
- `stale` (since 0.5.52): `true` when `wrapperVersion` differs from the
  daemon's `welcome.version` (a missing one counts), else omitted. The
  wrapper keeps running the old code after pvai is updated, and `restart`
  does not replace it: end the session and `start` it again with `resume`.
  A wrapper that reports a version older than 0.5.44 does
  not relay keys and screens as the daemon expects: `answer`, `clearInput`, `mode`,
  `model` and `restart` to its session are `rejected` with `stale-wrapper`.
  A missing version (`null`, wrappers before 0.5.52) is `stale` but served.
  Prompts still go through (typing works with any wrapper). Versions are
  compared by their dotted numbers, part by part. The wrapper's first line
  to the daemon is `{"type": "register", "version", "id", "label", "cwd",
  "pid", "running", "detached", "restart", "verify", "modes", "clear",
  "paced", "submitCheck"}`; `version` is new in 0.5.52. `submitCheck` (0.5.63)
  says the wrapper checks that the Return of a verified paste took: when
  the prompt box still shows the pasted text 1.5 s after the Return, it
  presses Return again (twice at most, 1.5 s apart); if the box still holds
  the text after that, it empties the box (`draft` dirty until done) and
  sends `{"type": "submitted", "ok": false}`; a Return that took sends
  `{"type": "submitted", "ok": true}` (the daemon ignores it). On `ok: false`
  the daemon ends a pending slash or shell command (`attention` `stuck`) and
  puts a normal prompt back at the front of the queue: typed again once,
  silently, once the box is empty; the second time for the same prompt it
  is `screen`-blocked with `attention` `stuck`, as for any prompt that did
  not go in. `pvai daemon stop|restart`
  prints how many sessions run a `pvai run` that is not its own version
  (the daemon lists the wrappers' versions in `shutting-down.wrappers`).
- `limited` (since 0.5.50): `null`, or `{"resetsAt": <Unix seconds, null
  when unknown>, "type": "<rateLimitType, e.g. five_hour|seven_day, or
  null>", "text": "<the assistant's text, one line>"}` while a Claude Code
  session waits for its usage limit to reset; see "Usage limit". The
  state stays `busy`. A missing field (an older daemon) means `null`.
- `subagents`: the subagents of a session that run now, oldest first; they
  can outlive the turn that started them (background agents), so a session
  can be `idle` with subagents running. A finished one drops out, and every
  change sends a `sessions` update. A missing field (an older daemon) means
  none.
  ```json
  [{"id": "a2b2c3d4e5f6a7b8c", "type": "build-runner",
    "description": "Build PhiVoice AI app", "background": true,
    "startedAt": 1793381087}]
  ```
  `id` is Claude Code's agent id, `type` its agent type, `description` the
  task's short description, `background` whether it was started in the
  background, `startedAt` Unix time. Read from Claude Code's
  `<session>/subagents/agent-<id>.meta.json` next to the transcript: an
  agent runs until its meta file says `finished`. One left unfinished by
  an earlier process (a restart, a crash) is not counted once its own
  transcript has not changed for 10 minutes.

  Codex: the subagent threads of the thread (`parentThreadId`) that are
  `active` in the app server; a subagent thread is never a session of its
  own. `id` is the thread id, `type` its `agentRole`, else the kind of
  Codex's own helper (`review`, `compact`, `memory_consolidation`), else
  empty; `description` the start of its first prompt (`preview`), else its
  nickname; `name` its `agentNickname` (Codex only, may be empty);
  `background` always `true` (Codex does not tell); `startedAt` the
  thread's `createdAt`.

### Claude Code's questions

Since 0.5.33 a `question` from Claude Code's `AskUserQuestion` carries the
questions with their choices, and `answer` takes them:

```json
"dialogInfo": {"tool": null, "text": "Welche Früchte?\nWelche Stadt?", "message": "...",
  "current": 0,
  "questions": [
    {"question": "Welche Früchte?", "header": "Früchte", "multiSelect": true,
     "options": [{"label": "Apfel", "description": "rot"}, {"label": "Kiwi", "description": "grün"}],
     "selected": [2]},
    {"question": "Welche Stadt?", "header": "Stadt", "multiSelect": false,
     "options": [{"label": "Bern", "description": ""}, {"label": "Zürich", "description": ""}]}]}
```

Claude Code shows one question at a time; `current` is its index, and
equals the number of questions on the review that comes after the last
of several ("Submit answers" / "Cancel"). A single question has no review:
its answer ends the dialog. `answer` choices:

- `option:<n>`: the n-th choice (from 1) of the current question. Single
  select: picked, on to the next question. Multi select: toggled, `selected`
  lists the numbers that are on.
- `next`: a multi-select question is done (Tab), on to the next.
- `text:<words>`: the owner's own answer to a single-select question
  (Claude Code's "Type something": the field after the choices, the words,
  Return), at most 2000 bytes, no line breaks; needs a `pvai run` of
  0.5.33 or later, else `not-supported`.
- `allow` on the review submits; `deny` (Esc) cancels the whole dialog at
  any point, as `interrupt` does.
- `key:NAME` as before, for anything else; the daemon does not follow it.

Wrong choices (no such option, `text:` or `next` where they do not fit,
`option:` on the review) are `rejected` with `bad-choice`.

## Messages

App to daemon:

| type | fields | meaning |
|---|---|---|
| `hello` | `token`, `client`, `version` | first line on every connection; `token` only on loopback (see "Direct access") |
| `prompt` | `ref`, `session`, `text`, `voice`, `from`, `fromLabel` | type `text` into that session; `ref` is the app's id for the answer; `voice: true` when the text was spoken; `from` and `fromLabel` only for an agent message the owner released (see "Agent messages") |
| `start` | `ref`, `cwd`, `label`, `args`, `assistant`, `resume` | start a session in `cwd` (absolute) without a terminal; `label` optional; `assistant` one of `welcome.assistants` (default `claude`); `args` optional strings: extra arguments for `claude`; for Codex and Kimi only `-m`/`--model <model>` is read; `resume` optional: an id from `conversations`, to go on with that conversation instead of a new one (see "Ended conversations") |
| `conversations` | `assistant`, `cwd` | the ended conversations of that assistant in `cwd` (see "Ended conversations") |
| `forget` | `ref`, `assistant`, `cwd`, `id` | delete an ended conversation for good (see "Ended conversations") |
| `update` | `ref` | update every assistant on this host to its latest version (see "Versions and updates") |
| `model` | `ref`, `session`, `model` | (since 0.5.34) switch the session's model; a `model` event follows (see "Models") |
| `mode` | `ref`, `session`, `mode` | (since 0.5.25) switch the session's permission mode to one of its assistant's `modes`; a `mode` event follows (see "Permission modes") |
| `install` | `ref`, `assistant` | (since 0.5.24) install an assistant for the daemon's user, only on the owner's click (see "Install and login") |
| `login` | `ref`, `assistant`, `method` | (since 0.5.24) log an assistant in on this host: a link and a code for the owner (see "Install and login"); `method: "token"` (Claude only, `welcome.tokenLogin`) runs `claude setup-token` instead, see "Claude token login" |
| `set-token` | `ref`, `assistant`, `token` | (`welcome.tokenLogin`) store a Claude Code token the owner pasted; answered with `token-set`, see "Claude token login" |
| `login-code` | `ref`, `assistant`, `code` | the code the login page showed, for a login in state `code-needed` (Claude Code) |
| `login-cancel` | `ref`, `assistant` | end a running login |
| `logout` | `ref`, `assistant` | (since 0.5.39) log an assistant out on this host (see "Install and login") |
| `answer` | `ref`, `session`, `choice` | answer the session's dialog: `allow` (the first choice of a permission or question, "Yes, I trust this folder" for trust; Return for `screen`) or `deny` (Esc: the tool is refused and the turn ends; for trust, Claude Code exits; for `screen` it closes the dialog). Since 0.5.13 also `key:NAME`, one key into any dialog but `trust`: `up`, `down`, `left`, `right`, `enter`, `esc`, `tab`, `space`, or a digit or lower-case letter (an option's number, a shortcut such as `s`); the dialog stays open until the screen or a hook says otherwise. `send`: see "Delivery" under the session fields. Kimi: also `always` and `option:<id>`, see "Kimi sessions" |
| `interrupt` | `ref`, `session` | stop the running turn, like Esc in the terminal (into a dialog: deny); nothing happens while `idle`, `starting` or on the trust dialog |
| `stop` | `ref`, `session` | end a `stoppable` session |
| `rename` | `ref`, `session`, `label` | (since 0.5.74, `welcome.rename`) give a running session a new label, see "Renaming a session" |
| `clearInput` | `ref`, `session` | empty the session's prompt box, wherever its cursor is, and leave shell mode (Ctrl+K and Ctrl+U to each line, then Backspace; Esc does not empty it); queued prompts go on once the box is free. Only while `inputClearable`. Not Claude Code's `/clear`, which starts a new conversation |
| `restart` | `ref`, `session` | end the session's process and start it again, going on with the same conversation (see "Restart") |
| `projects` | | the directories Claude Code has run in on this host |
| `browse` | `path` | the subdirectories of `path` |
| `subscribe` | `session`, `limit` | follow that session's conversation: `history` with up to `limit` (default 200) of the last entries, then `entry` for each new one |
| `unsubscribe` | `session` | stop following it |
| `dismiss` | `ref`, `proposal` | the owner sent or dropped that agent message; the daemon forgets it |
| `propose` | `ref`, `to`, `text`, `from` | from `pvai propose` only, on loopback (ignored over TLS): an agent message for the owner to release (see "Agent messages"); answered with `proposed` (`ref`, `id`, `apps`, optional `warning`, `seenAt`) or `rejected` |
| `peers` | `self`, `peers: [Peer]` | the sessions on other hosts that agents here may write to, as the app knows them; the whole list each time, sent after `welcome` and after every change (see "Peers") |
| `ping` | | keepalive |

Daemon to app:

| type | fields | meaning |
|---|---|---|
| `welcome` | `host`, `user`, `version`, `assistants`, `installed`, `conversations` | answer to a good `hello`; `host` is the short hostname; `user` (since 0.5.35) the login name the daemon runs as, which every session runs as too; `assistants` what `start` can start on this host, see "Assistants"; `installed` the version of each, see "Versions and updates"; `conversations: true`: the daemon answers `conversations` and `start.resume`; `peers: true`: it takes `peers`; `forget: true`: it takes `forget`; `logout: true` (since 0.5.39): it takes `logout`; `rename: true` (since 0.5.74): it takes `rename`, an older daemon ignores the request without an answer; `tokenLogin: true`: it takes `login` with `method: "token"` and `set-token` |
| `sessions` | `sessions: [Session]` | the full list, sent after `welcome` and after every change of any session (state, queue, label, start, end) |
| `accepted` | `ref`, `session`, `delivery` | `delivery` is `typed` (went in now) or `queued`; under a `pvai run` of 0.5.15 or later always `queued`, and `delivered` follows once Claude Code has the prompt |
| `delivered` | `session`, `ref` | (since 0.5.15) a prompt is Claude Code's: `UserPromptSubmit` confirmed it, or for a slash command the wrapper saw it in the prompt box; `ref` the one of its `prompt`; to every app |
| `rejected` | `ref`, `session`, `reason` | to `prompt`: `unknown-session`, `empty`, `too-long` (over 32 KiB); to `start`: `bad-cwd`, `unknown-assistant`, `no-assistant` (Codex's app server or `kimi acp` is not reachable, yet), `bad-start` (the assistant refused), `not-logged-in` (Kimi: no login on the host), `not-installed` (the assistant's program is not on this host, see "Assistants"), `unknown-conversation` (no such `resume` id); to `answer`: `unknown-session`, `no-dialog`, `bad-choice`; to `stop`: `unknown-session`, `not-detached` (not `stoppable`); to `interrupt`: `unknown-session`; to `restart`: `unknown-session`, `not-restartable`, `busy` (only from `idle` without a `draft`); to `answer`, `clearInput`, `mode`, `model` and `restart` (also): `stale-wrapper` (since 0.5.52, the session's `pvai run` reports a version older than 0.5.44, see `Session.stale`); to `rename`: `unknown-session`, `bad-label`, `taken` (see "Renaming a session"); to `clearInput`: `unknown-session`, `not-supported` (not Claude Code, or a `pvai run` older than 0.5.30), `busy` (not `inputClearable` now); to `propose`: `empty`, `too-long`, `bad-target`, `too-many` |
| `started` | `ref`, `session` | the new session's id; it shows up in `sessions` when its wrapper is up (`starting`) |
| `answered` | `ref`, `session` | the keys went in |
| `renamed` | `ref`, `session`, `label` | the session carries `label` (trimmed) now; the `sessions` list follows |
| `inputCleared` | `ref`, `session` | the keys for `clearInput` went in; `draft` and the state follow from the screen |
| `interrupting` | `ref`, `session` | the Esc went in; the session turns `idle` when the transcript shows the interrupt, or after about 4 s anyway, then with its queue dropped (a hung session, such as one without a login) |
| `restarting` | `ref`, `session` | the session is ending for a restart; it goes `starting`, then `idle` again with the same `id` |
| `stopping` | `ref`, `session` | the session is told to end; it leaves `sessions` when it has |
| `projects` | `dirs: [path]` | most recently used first |
| `browse` | `path`, `dirs: [name]`, `ok` | sorted, hidden ones left out; `ok: false` when `path` cannot be read |
| `say` | `session`, `text`, `voice` | a turn ended; `text` is what to speak; `voice` tells whether the turn counts as spoken (see "What gets spoken") |
| `attention` | `session`, `kind`, `text` | the session started waiting on a dialog; `kind` as `dialog`; `text` is Claude Code's notification message (for `trust`: the folder; for `screen`: the last lines above the prompt box, or the dialog's first lines). Also `kind: "login"` (Kimi lost its login, see "Kimi sessions") and, since 0.5.18, `kind: "ended"`: a Claude Code session the app started ended within 90 s or with an error, not by `stop`; `text` is the last lines its screen showed (up to 10, such as "command not found" or "Not logged in"), or the exit status; the session leaves `sessions` as usual. Since 0.5.34 also `kind: "model"`: the assistant changed the model on its own (see "Models"); since 0.5.50 `kind: "limited"`: a Claude Code session hit its usage limit, `text` is Claude Code's message (see "Usage limit"); since 0.5.63 `kind: "stuck"`: sent when the wrapper reports that Claude Code did not act on the Return (`submitted` with `ok: false`, see `register`): for a slash or shell command the box was emptied and the session is idle again; for a prompt it was typed a second time and failed again, and it waits at the front of the queue behind the `screen` dialog; `text` is the first 40 characters |
| `history` | `session`, `entries: [Entry]` | answer to `subscribe`, oldest first; sent again, to every subscriber, when the session moves to another transcript (`/clear`): the view starts over |
| `usage` | `session`, `usage` | the session's `usage` changed (not sent for a change of `costUsd` alone) |
| `conversations` | `assistant`, `cwd`, `conversations: [Conversation]` | answer to `conversations` |
| `forgotten` | `ref`, `assistant`, `cwd`, `id` | answer to `forget`: the conversation is deleted |
| `updated` | `ref`, `installed`, `changed`, `errors` | answer to `update` |
| `install` | `ref`, `assistant`, `state`, `step`, `text`, `error` | an install's progress, to every app: `state` `running`, then `done` or `failed` (`error` why); `step` `download` (claude), `node`, `npm` (codex, kimi), `check`; `text` the last line of output |
| `login` | `ref`, `assistant`, `state`, `url`, `code`, `text`, `error` | a login's progress, to every app: `state` `waiting` (show `url` and `code`; the owner signs in in a browser), `code-needed` (Claude Code: the page shows a code, send it with `login-code`), `done` or `failed` (`error`: why, also `cancelled` and `timed out`) |
| `token-set` | `ref`, `assistant` | answer to `set-token`: the token is stored (it is never sent back); `rejected` otherwise |
| `logout` | `ref`, `assistant`, `state`, `text`, `error` | a logout's progress, to every app: `state` `running`, then `done` or `failed` (`error` why, also `timed out`); `text` the last line of output |
| `model` | `ref`, `session`, `model`, `state`, `error` | a model change ended, to every app: `done` or `failed` (see "Models"); `rejected` before (`unknown-session`, `bad-model`, `not-supported`) |
| `mode` | `ref`, `session`, `state`, `mode`, `error` | a mode change ended, to every app: `state` `done` (`mode` the new one) or `failed` (`error`: `unavailable`, the mode is not in this session's cycle; `no change on the screen`; Kimi's own words); `rejected` before (`unknown-session`, `unknown-mode`, `not-supported`, `busy`) |
| `assistants` | `assistants`, `installed` | (since 0.5.24) `welcome.assistants` and `welcome.installed` again, to every app, when an install, a login or a logout changed them |
| `proposals` | `proposals: [Proposal]` | the agent messages waiting for the owner, sent after `welcome` and after every change (see "Agent messages") |
| `dismissed` | `ref`, `proposal` | answer to `dismiss`; `proposal` is gone from `proposals` (also when it was unknown) |
| `dropped` | `prompts: [{session, ref, text, kept?}]` | (since 0.5.21, extended 0.5.52) prompts that will not be typed: a session ended with a queue or a prompt typed and not yet confirmed (Claude Code exited, `stop`, a restart that failed, the daemon dropped its wrapper; Codex and Kimi: the thread or session ended), or the daemon is stopping by `pvai daemon stop|restart --force`. Since 0.5.71 also a prompt typed and lost without a trace: Claude Code took a slash command (a `UserPromptSubmit` starting with `/`) instead of it, or the queue was dropped after an interrupt of a hung session; the daemon never types it again, the app may offer to resend. `ref` is the one of their `prompt`; `text` is the whole prompt (before 0.5.52 the first 80 bytes, and only on a forced stop). `kept: true` (since 0.5.52, Claude Code only) says the daemon keeps that prompt for the conversation (see "Kept prompts"); it is omitted otherwise. A prompt kept and later lost for good (24 h, `daemon stop`, `forget`, the caps) comes in a second `dropped` with the same `session` and `ref`, without `kept`. To every app. Without `--force` the daemon does not stop while any prompt is queued (`pvai` gets `rejected` with `queued` over loopback, from `{"type": "shutdown", "ref", "force"}`) |
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

1. The agent runs `pvai propose [--reply ID] TO TEXT` (or the text on
   stdin) in its session. `TO` is `host/session`: `host` is the target
   daemon's short hostname (`welcome.host`; `PVAI_HOST` in the environment
   overrides the machine's name, the tests use it), `session` a session id or a
   label. A bare `host` means "whichever session the owner picks".
   `--reply ID` names the proposal this one answers (the id in the first
   line of the message being answered).
2. `pvai propose` hands it to its own daemon over loopback:
   `{"type": "propose", "ref", "to", "text", "from", "replyTo"}`, where `from` is
   `PVAI_ID` from the environment (set by `pvai run`; empty in a Codex
   thread, then only the host is known) and `replyTo` optional. A label
   as `session` becomes the id when exactly one session of that host has
   it (this host's own sessions, or the app's `peers` for other hosts);
   when several have it the proposal is `rejected` with
   `ambiguous-target`, so it cannot land in the wrong one; an unknown name
   stays as it is (the app may know it). The daemon answers `proposed`
   with the proposal's `id`, the `to` it resolved and `apps` (how many apps are connected now;
   connections whose `hello.client` is `pvai`, the CLI's own, do not count)
   and, since 0.5.69, optionally a `warning` (see "Peers"; the proposal is made and queued all the same)
   or `rejected` (`empty`, `too-long` over 32 KiB, `bad-target`,
   `ambiguous-target`, `bad-reply` (`replyTo` is not a proposal id: `p`
   and hex digits), `too-many`). `TO` is a bad target unless `host` is 1 to 64 letters,
   digits, `-`, `.` or `_`, and `session`, when given, 1 to 128 bytes
   without control characters.
   The CLI prints that the message waits for the owner and returns at
   once; it never waits for an answer.
3. The daemon keeps the proposal and lists it in `proposals` to every
   connected app:
   ```json
   {"id": "p3f9a01c2", "from": "w8bb65867", "fromLabel": "phi-claude-voice",
    "to": "taifun/wd718313b", "text": "...", "replyTo": "p77aa0b1e", "at": 1793300000}
   ```
   `replyTo` (since 0.5.19) is the proposal it answers, else `null`: the
   app can show a message under the one it answers, and see which got no
   answer.
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
   `from: "M4/w8bb65867"` (sender host and session), `fromLabel` (the
   proposal's, which only the sender's daemon knows), and since 0.5.19
   `proposal` (its id) and `replyTo` (the proposal's). The target daemon
   types this first line before the text, so the receiving agent knows
   which message it is, who wrote, what it answers and how to answer it:
   ```
   [Message p3f9a01c2 from M4/w8bb65867 (phi-claude-voice), answering p77aa0b1e, released by the owner. Answer with: pvai propose --reply p3f9a01c2 M4/w8bb65867 "..."]
   ```
   Without `proposal` the id and `--reply` are left out, without
   `replyTo` the "answering" part (as before 0.5.19).
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

## Peers

An agent can only propose to a session it knows of, and a daemon knows
only its own host. So the app tells each daemon the sessions on the other
hosts:

```json
{"type": "peers", "self": "taifun", "peers": [
  {"host": "M4", "session": "w97dde1bd", "label": "Projekte", "assistant": "claude",
   "state": "idle", "cwd": "/Users/m/Projekte"},
  {"host": "M4", "session": "w290e2a02", "label": "Docs", "offline": true, "seenAt": 1762330000}]}
```

`self` is the app's name for the receiving host (optional; else the
daemon's `welcome.host`). `host` is the name an agent puts into `pvai
propose`, so the app must resolve it later; `session` the id (may be
empty: the host as a whole). `label`, `assistant`, `state` and `cwd` are
optional. The receiving host's own sessions belong not in the list (the
daemon knows them). Each `peers` replaces the last; the daemon keeps it in
memory only, after the app disconnects too, and gives it to no other app.
At most 256 entries; an entry whose `host` is no good target host (see
"Agent messages") is dropped, strings are cut (`label` 64, `assistant`
and `state` 16, `cwd` 512 bytes).

Since 0.5.69 a Peer may also carry `offline` (boolean: the app knows the
host but cannot reach it now) and `seenAt` (number, Unix time the app last
reached it). Both are optional, kept as given and passed on in `peerList`;
a value of the wrong type is dropped. A host is offline when all its
entries are.

`proposed.warning` (since 0.5.69, absent when there is none) tells the
agent what the last `peers` says about a target on another host; never for
this host's own sessions, for a host not in the list, or before any list
came. The proposal is created and queued as without it; the app decides at
release. Values: `host-offline` (the host is in the list and all its
entries are offline; `proposed.seenAt` is the latest `seenAt` of them, if
any) and `unknown-session` (the host is online in the list, the target names
a session and it is neither an id nor a label there). `pvai propose` prints
it as a sentence and still exits 0; the daemon log line carries it too.

What to share is the app's choice: a host the owner does not trust with
the list of the others gets none, or only host names without `cwd`.

`pvai peers` (loopback only, `{"type": "listPeers"}`, answered with
`peerList`: `self`, `local`, `peers`, `at`) prints this host's sessions and
then the list, each line starting with the target for `pvai propose`
(an offline entry ends with `(offline, last seen 12 min ago)`, or `(offline)`
without `seenAt`; ages go in min, then h from 1 hour, days from 2 days):

```
this host (taifun):
  taifun/wb4c7dc28  claude  busy  "phi-claude-voice"  /root/projects  (this session)
other hosts (from the app, 2 min ago):
  M4/w97dde1bd  claude  idle  "Projekte"  /Users/m/Projekte
  M4/w290e2a02  "Docs"  (offline, last seen 12 min ago)
write to one with: pvai propose HOST/SESSION "TEXT" (a label works instead of the id when only one session has it)
```

The context every session gets (the SessionStart hook, Codex's developer
instructions) mentions `pvai peers` and `pvai propose`, so an agent told
"ask M4" knows what to do.

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
- `stop` on a thread this daemon started (`detached: true`) archives it,
  which unloads it from the app server; `codex resume` no longer lists it,
  but it is kept: `conversations` lists it and `start.resume` unarchives
  it. On another client's thread that no `pvai codex` TUI holds
  (`stoppable: true`, `detached: false`) it only lets go: the daemon
  unsubscribes, the thread leaves `sessions` and is not taken on again
  while it stays loaded (a TUI without pvai or the Codex app may still
  have it open and goes on); it stays in `codex resume`. On a thread a
  `pvai codex` TUI holds it is `rejected` with `not-detached`.
- After the app server restarts, the daemon takes on what it reports as
  loaded (`thread/loaded/list`); a thread that another client loads again
  shows up again. TUI threads and threads from before a
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
  limits, `costUsd` always `null`, no `cache`, `version` the app server's (from its
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

## Kimi sessions

Since 0.5.16. The daemon starts one `kimi acp` (Kimi Code's Agent Client
Protocol server, stdio) when `kimi` is installed and speaks to it for all
Kimi sessions; if it ends, its sessions end and it is started again.
Every Kimi session is the daemon's (`detached`, `stoppable`; `stop` closes
it in Kimi, where `conversations` still finds it). Sessions a `kimi` TUI
has open are Kimi's own and are not shown (Kimi locks them).

- `start` with `assistant: "kimi"` is `session/new` in `cwd`; `args` only
  `-m`/`--model <model>` (a value of `welcome.assistants[].models`). Without
  a login it is `rejected` with `not-logged-in`, and `loggedIn` turns
  `false`.
- A prompt is `session/prompt`; the first of a session carries the 🔊
  convention as embedded context (not part of the conversation Kimi keeps).
  The turn ends with Kimi's answer to it: `say` with the 🔊 line as usual;
  a cancelled turn is an `interrupted` entry. A prompt that fails because
  the login is gone gets a text entry saying so, `attention` with `kind:
  "login"`, and `say`.
- Dialogs: Kimi's `session/request_permission`. A tool's approval is
  `permission` (`dialogInfo.tool` `shell`, `edit`, `read`, `search`,
  `fetch`, ... from the tool's kind, `text` the command or path, `message`
  Kimi's title for it); a question Kimi asks (its AskUserQuestion, bridged
  as a permission request) is `question` (`text` the question).
  `dialogInfo.options` lists Kimi's choices, in its order:
  `[{"id": "approve_once", "name": "Approve", "kind": "allow_once"},
  {"id": "approve_always", "name": "...", "kind": "allow_always"},
  {"id": "reject", "name": "Reject", "kind": "reject_once"}]` (a question's
  ids are `q1_opt_<i>` and `q1_skip`). `answer` takes `allow` (the first
  allow option), `deny` (the first reject option), `always` (the
  allow_always option: Kimi asks no more for that kind of call in this
  session) or `option:<id>` for any of them. `interrupt` cancels the
  dialog and the turn.
- `usage`: `model` the model's name, `context` from Kimi's `usage_update`
  after each turn (`tokens` the context used, `size` the model's window),
  `version` Kimi Code's; `fiveHour`, `sevenDay`, `costUsd` stay `null`
  (ACP has no limits or cost), there is no `cache`.
- `update` leaves Kimi alone (installed with npm): it is never in
  `changed` or `errors`.

## Assistants

`welcome.assistants` lists what `start` can start on this host, in the
order the app should offer them:

```json
[{"name": "claude", "title": "Claude Code", "models": ["opus", "sonnet", "haiku", "fable"], "installed": true},
 {"name": "codex", "title": "Codex", "models": ["<a Codex model>", "..."], "installed": true},
 {"name": "kimi", "title": "Kimi Code", "models": ["<a Kimi model>"], "installed": true, "loggedIn": false}]
```

`name` is what `start.assistant`, `conversations.assistant` and
`Session.assistant` carry; `title` is for people; `models` are the values
`start` takes as `args: ["--model", m]` (Claude Code's aliases; Codex's
non-hidden models from its app server, empty until it answered; Kimi's
values of its `model` option, empty until a session started). The app
knows no assistant by name: a new one shows up here and works without a
change to the app. `installed` (since 0.5.18): whether `start` can start it, its program
being on this host. Since 0.5.24 every assistant is listed, installed or
not (before, Codex and Kimi only when installed); see "Install and login". `start` of one
that is not is `rejected` with `not-installed`. `loggedIn` (Kimi since 0.5.16,
all since 0.5.24):
`true` or `false` once the assistant said, `null` before; `false` means
the owner has to log in (`login`, see "Install and login"; for Kimi
`start` is then `rejected` with `not-logged-in`).

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
   - Kimi: its `session/list` for that `cwd` (title and `updatedAt` as
     Kimi keeps them); running sessions are left out.
2. `start` with `resume: "<id>"` (and `assistant`, `cwd`) goes on with it:
   Claude Code as `claude --resume <id>` plus `args` (a detached session
   that reads the transcript), Codex by unarchiving and resuming the
   thread (a detached session; a thread that is still loaded only becomes
   `detached`), Kimi with `session/load` (a detached session; Kimi plays
   the history back, its text only). An id that is not there is
   `rejected` with `unknown-conversation`.
3. `{"type": "forget", "ref", "assistant", "cwd", "id"}` deletes an ended
   conversation for good (since 0.5.31, `welcome.forget: true`), answered
   with `{"type": "forgotten", "ref", "assistant", "cwd", "id"}`; the app
   asks the owner first, nothing brings it back. Claude Code: the
   transcript `<id>.jsonl` and the folder beside it with the same name
   (subagents' transcripts, tool results). `rejected` (with `assistant`,
   `cwd`, `id` echoed): `unknown-conversation` (no such id there),
   `running` (a session goes on with it now), `not-supported` (Codex and
   Kimi, for now), `unknown-assistant`, `bad-cwd`, `failed` (the file
   could not be removed).

## Permission modes

Since 0.5.25 every assistant lists its modes in `welcome.assistants[].modes`
as `[{"id", "name"}]`, the session shows its current one as `mode`, and
`{"type": "mode", "ref", "session", "mode"}` switches it:

- Claude Code: `default` (Ask), `acceptEdits` (Accept edits), `plan`
  (Plan), `auto` (Auto), `bypassPermissions` (Bypass); the names are kept
  short and alike across assistants, in English. The daemon presses Shift+Tab, as the owner would,
  until the footer shows the mode; at most once round Claude Code's
  cycle, so a mode this session does not offer (`auto` without the
  setting, `bypassPermissions` without its flag) ends `failed` with
  `unavailable`, back where it started minus the presses. Not while a
  dialog is open (`busy`), and only under a `pvai run` of 0.5.25 or later
  (`not-supported`), which reports the footer. It holds for that session;
  Claude Code's settings stay as they are.
- Codex (since 0.5.34): its three permission presets, approval policy and
  sandbox together: `read-only` (Read only: `on-request`, read-only
  sandbox), `auto` (Auto: `on-request`, writes in the workspace),
  `full-access` (Full access: `never`, no sandbox). They go with the next
  turn (`turn/start`) and hold from there; `done` at once. The session's
  `mode` follows the thread's settings (start, resume,
  `thread/settings/updated`); any other combination (`untrusted`, a
  granular policy) shows as `null`. The approval-only modes of 0.5.25
  (`untrusted`, `on-request`, `never`) are gone.
- Kimi: its ACP modes as it lists them (`default` Ask, `plan` Plan,
  `auto` Auto, `yolo` YOLO; Kimi's own name for any other), with
  `session/set_mode`.

## Models

Since 0.5.34 (`welcome.model: true`) `{"type": "model", "ref", "session",
"model"}` switches a running session's model; `model` is one of
`welcome.assistants[].models` or any name the assistant takes (letters,
digits, `.-_[]:/`, at most 80; else `rejected` with `bad-model`). The
answer is `{"type": "model", "ref", "session", "model", "state": "done" |
"failed", "error"?}` to every app; `usage.model` shows what runs.

- Since 0.5.57 a `/model` that shows no output and no dialog within 30 s
  after it went in fails with `error` `no answer`, and the session is idle
  again. A wrapper that reports no version (before 0.5.52) rejects `model`
  with `stale-wrapper`: restart the session, which takes the new model.
- Claude Code: its own `/model <name>`, queued like a prompt (it goes in
  when the prompt is free); since 0.5.41 `done` once Claude Code's
  `/model` answered "Set model to ..." in the transcript, `failed` with
  Claude Code's message (e.g. "Kept model as Opus 5.5") otherwise, and
  `rejected` with `busy` while another switch is pending. A usage-credits
  dialog Claude Code may show first arrives as a `screen` dialog
  (attention kind `screen`) the app answers like any screen dialog.
  `usage.model` follows from the status line.
- Codex: with the next turn (`turn/start` `model`), holding from there;
  `done` at once and `usage.model` is the new one.
- Kimi: its `model` option (`session/set_config_option`); `done` or
  `failed` with Kimi's error.

When the assistant changes the model on its own, `usage.model` follows and,
where the assistant says so, the apps get `{"type": "attention", "session",
"kind": "model", "text": "<old> → <new> (<reason>)"}`: Codex's
`model/rerouted` (a limit, a capacity or safety reason). Claude Code's
fallback shows in `usage.model` only (its status line).

## Usage limit

Since 0.5.50 a Claude Code session that runs into its usage limit shows it.
Claude Code ends the turn with an API error (a 429) in the transcript and
goes on by itself at the reset, without a hook, so the session stays
`busy` and `Session.limited` says why: `{"resetsAt": 1791826200, "type":
"five_hour", "text": "You've hit your session limit · resets 7:30pm
(Europe/Zurich)"}`. When the limit is hit the apps get once `{"type":
"attention", "session", "kind": "limited", "text": "<text>"}` (to show and
to speak), and the conversation view a `limited` history entry.

A model's own quota (Fable's, `apiError` `model_requires_usage_credits`,
since 0.5.57) is read the same way from `apiErrorParams.rate_limit_info`,
but Claude Code does not go on by itself at its reset: the session goes
`idle` at once, `limited` stays `null`, the apps get the one `attention`
`kind: "limited"` (its `text`, to offer a model switch), and the history
entry carries `"model": true`.

`limited` goes back to `null`, with a `sessions` update, when the turn
goes on or ends: Claude Code's resumption (a `resumed` history entry), an
answer that is no API error, UserPromptSubmit, the session ending or
restarting, or the owner's Esc. A Stop hook that comes while the reset is
still ahead is ignored, since Claude Code continues anyway; a limit still
set 3 minutes after `resetsAt` is given up and the session goes `idle`. A
daemon that takes a session on again after a restart shows it `busy` and
`limited` when the limit is still ahead.

## Install and login

Since 0.5.24 the app can install an assistant that is missing and log one
in, without a terminal. Both run only when the owner asks (a click), for
the user the daemon runs as (with `root`, in `/root`), never system wide,
one task per assistant at a time.

- `{"type": "install", "ref", "assistant"}` (`rejected`: `unknown-assistant`,
  `already-installed`, `busy`). Claude Code comes from its official
  installer (`curl -fsSL https://claude.ai/install.sh | bash`, into
  `~/.local/bin/claude`). Codex (`@openai/codex`) and Kimi Code
  (`@moonshot-ai/kimi-code`) come from npm into a prefix of their own
  under `~/.local/share/pvai/`, with the system's Node when it is new
  enough (Codex 20, Kimi 22.19), else Node 22 from nodejs.org, checked
  against its `SHASUMS256.txt`, beside them; a wrapper in `~/.local/bin`
  starts each with that Node. The system's Node is never touched. Events
  `install` follow (`running` with `step` and the last line of output,
  then `done` or `failed`), at most 15 minutes. Then `assistants` with the
  new `installed`.
- `{"type": "login", "ref", "assistant"}` (`rejected`: `unknown-assistant`,
  `not-installed`, `busy`) runs the assistant's own login in a
  pseudo-terminal, without a browser on the host, and reads link and code
  off its output:
  - Codex: `codex login --device-auth`: `url`
    `https://auth.openai.com/codex/device` and a `code` such as
    `ABCD-12345` (15 minutes); it is done when the owner has entered it.
  - Kimi: `kimi login`: `url` with the code in it, and the `code`
    (30 minutes).
  - Claude Code: `claude auth login`: `url` only, then `code-needed`: the
    page shows a code after the sign-in, which the app sends with
    `{"type": "login-code", "ref", "assistant", "code"}` (`rejected`
    `no-login` when none waits for it); `waiting` again until it is through.
  - Claude Code with `"method": "token"`: `claude setup-token`, see
    "Claude token login".
  `login-cancel` ends it; after 30 minutes it ends by itself. The output's
  forms are the assistants' own and may change with their versions: `text`
  always carries the last line as it came.
- `{"type": "logout", "ref", "assistant"}` (since 0.5.39; `welcome.logout:
  true`) runs the assistant's own logout as a child process: `claude auth
  logout`, `codex logout`. `rejected`: `unknown-assistant`, `not-installed`,
  `busy` (a login or logout of that assistant is running), `not-logged-in`
  (`loggedIn` is `false`), `not-supported` (Kimi Code has no logout
  command). Events `logout` follow (`running` with the last line of output
  as `text`, then `done` or `failed`; at most 1 minute), then `assistants`
  with `loggedIn` read again. Running sessions of that assistant are left
  as they are. For Claude Code the stored fallback token is deleted too,
  even when `claude auth logout` fails (see "Claude token login").
- `loggedIn` in `welcome.assistants` for all three: Claude Code from
  `claude auth status --json`, Codex from `codex login status`, Kimi from
  its ACP answers (and `true` after a login went through); read with the
  versions, after every install, login and logout. `null` when not known.
- `welcome.assistants` lists every assistant pvai knows, installed or not
  (`installed: false`), so the app can offer to install it. The `claude`
  entry also has `loginMethod` and `tokenStored` (see "Claude token login").

### Claude token login

Claude Code on macOS can fall back to the Keychain (`/usr/bin/security`),
and every session then risks a prompt. A long-lived token in the
environment (`CLAUDE_CODE_OAUTH_TOKEN`) beats the Keychain, so pvai can keep
one itself and hand it to Claude Code, also with no app running and on a
host that has no app. It is only a fallback: where the account login works
it is not used. `welcome.tokenLogin: true` says the daemon knows both
requests below.

- `{"type": "login", "ref", "assistant": "claude", "method": "token"}` runs
  `claude setup-token` in the login terminal (1000 columns wide, so that it
  does not wrap its link; a wrapped link or token is put together anyway).
  Same events as `claude auth login`: `url`, then `code-needed` (the page's
  code goes back with `login-code`), then `done` or `failed`. The token it
  prints is recognized (`sk-ant-oat` and URL-safe characters), stored and
  never sent on: with this method `text` holds fixed words only (`Waiting
  for the sign-in`, `Waiting for the code`, `Token stored`), never the
  output. `setup-token` exiting successfully without a recognized token is
  `failed` with `error: "token-not-recognized"`; the app then offers the
  paste field (`set-token`). A failing exit is `failed` with `error:
  "setup-token failed"`. `rejected`: as for `login`, and `bad-method` for an
  unknown `method` (or `token` for another assistant). The login code is
  typed into the terminal and logged nowhere. On macOS this method may
  prompt once for the Keychain (`claude setup-token` itself touches it).
- `{"type": "set-token", "ref", "assistant": "claude", "token"}` stores a
  token the owner pasted. The only check is local: a non-empty string of up
  to 1024 printable characters without whitespace, else `rejected` with
  `bad-token`. Claude Code is not started to check it, so a paste never
  causes a Keychain prompt. `rejected` also: `unknown-assistant`,
  `not-supported` (not Claude), `busy` (a login or logout of Claude runs),
  `cannot-store-token`. Answer: `{"type": "token-set", "ref", "assistant":
  "claude"}`, then `assistants`. Set only: no message, log line or event
  ever returns the token. What else happens depends on the decision below:
  with the account login missing the token is the fallback at once and
  `claude auth status --json` runs once with it (never one without it);
  with the account logged in, or a token in the daemon's environment, the
  token is on standby and nothing runs.
- Where: the file `claude-token` in pvai's config directory
  (`~/.config/pvai`, `$XDG_CONFIG_HOME/pvai`), mode 0600, one line, written
  to a 0600 temp file in that directory and renamed: never readable by
  others, not even briefly. Next to it the empty marker file
  `claude-token-active` (0600) says that the fallback is active.
- The decision, cached: the daemon runs `claude auth status --json` without
  the token only where it checks the login anyway (at its start, on a `hello`
  when the versions are old, after every install, login and logout), never
  before a session start and not on a timer (on a Mac with a damaged
  Keychain that very call prompts). `loggedIn: true`: the fallback is
  inactive (marker removed), starts get no token. `loggedIn: false` and a
  token file: the fallback is active (marker created) and the status is read
  again with the token, whose answer is the `loggedIn` shown. `loggedIn:
  false` and no file: inactive, `loginMethod: "none"`. An unreadable answer
  leaves the decision as it was.
- Who gets it, while the fallback is active: every Claude Code start as
  `CLAUDE_CODE_OAUTH_TOKEN` in its environment, never on a command line.
  All sessions are `pvai run` wrappers, the app's detached ones and the
  owner's in a terminal, and a wrapper does not talk to the daemon for this:
  at every launch of claude (a `restart`'s relaunch too) it adds the token
  when the marker exists, the file exists and its own environment has no
  `CLAUDE_CODE_OAUTH_TOKEN`. The daemon's status run with the token gets it
  in its environment likewise. `pvai run -- COMMAND` with another command is
  treated alike.
- Precedence: a `CLAUDE_CODE_OAUTH_TOKEN` that is already in a process's
  environment (the daemon's or the wrapper's shell) is left alone and the
  file is never applied over it. Without it the file is a fallback as
  above.
- `loginMethod` of the `claude` entry in `welcome.assistants` and
  `assistants`: the login in effect, `"token"` (the fallback is active, or
  the daemon's environment has a token), `"account"`, or `"none"` (the account
  is not logged in and there is no token to fall back to). `tokenStored`:
  whether a token file exists (standby or not). The token itself is never
  sent.
- `logout` of Claude deletes the token file and the marker, and forgets the
  decision, even when `claude auth logout` fails; the status is read again
  afterwards. A token in the daemon's own environment is not removed by it.
- Limit: `claude auth status --json` does not check a token against the
  server. Any value in `CLAUDE_CODE_OAUTH_TOKEN` reads as `loggedIn: true`
  (`authMethod: "oauth_token"`, tried with an obviously fake token on Claude
  Code 2.1.319). So an expired or revoked token is not seen as `loggedIn:
  false`; sessions fail when they first need the server. pvai does not probe
  the network for it.

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

## Renaming a session

`{"type": "rename", "ref", "session", "label"}` (since 0.5.74, `welcome.rename:
true`) renames a running session. The daemon trims the label (spaces, tabs,
CR, LF at both ends) and answers `{"type": "renamed", "ref", "session",
"label"}` with the trimmed label; every app then hears the new label in
`sessions`. Otherwise `{"type": "rejected", "ref", "session", "reason"}`:

- `unknown-session`: no such session on this host (or it has ended).
- `bad-label`: empty after trimming, not a string, longer than 64 bytes, or
  holding a control character (below 0x20, or 0x7f; a newline inside counts).
- `taken`: another session on this host already has exactly that label.
  Renaming a session to the label it has is a success and changes nothing.

A rename stays over restarts:

- Claude sessions: the daemon sends the wrapper `{"type": "label", "label"}`
  (daemon to `pvai run`, since 0.5.74); the wrapper keeps it and sends it as
  `label` in every later `register`, so a wrapper that registers again after
  a daemon restart brings the new name, not the one from its command line.
  Nothing else sets a Claude session's label afterwards. A session the daemon
  replaces for a `restart` starts with the current label.
- Codex and Kimi sessions are rebuilt from the assistant's own state, which
  does not know the name. The daemon keeps these renames in
  `~/.config/pvai/labels.json` (`$XDG_CONFIG_HOME/pvai`; mode 0600), a JSON
  object from `"<assistant>/<session id>"` (such as `"codex/019a..."`) to the
  label. The name is put on the session whenever it is created again, and
  from then on a label from the assistant's title or from `start.label` does
  not replace it. `forget` of the conversation removes the entry.

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
| `limited` | `text`, `resetsAt`, `type`, `model` | since 0.5.50: the usage limit was hit; `resetsAt` (Unix seconds) and `type` (`five_hour`, `seven_day`) are null when the line has none; since 0.5.57 `model` is true for a model's own quota (Fable), which does not hold the session busy |
| `resumed` | | since 0.5.50, internal and optional: Claude Code went on by itself after a `limited`; an app that does not know the kind skips it |
| `command` | `text` | a slash command that ran in Claude Code itself (`/compact`, `/context`, `/model opus`), a shell command the owner typed with `!` (`! git status`), or `/compact` for a compaction Claude Code started itself (the context ran full, or idle before the prompt cache expired) |
| `output` | `text` | what such a command printed (for `!`: stdout, then stderr), terminal colours removed, cut at 4 KiB (empty output is not passed on) |

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
- (Since 0.5.36) A turn the owner did not type counts as spoken when the
  owner's last own prompt was: a turn a finished background task or agent
  started, and an agent message the owner released (`[Message …]`). A
  prompt the owner typed ends that. The 🔊 convention says the same.
  (Since 0.5.54) A prompt the daemon typed itself decides by what it is,
  not by its text: a `voice: true` prompt is spoken, an agent message
  (`from`) leaves the state as it is, any other app prompt ends it. Only a
  prompt typed in the terminal (or after a daemon restart) is judged by its
  text, and Claude Code's `<pasted_content id="...">` frame around a long
  prompt is ignored there, in the transcript's `prompt` entries too.
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

## Shared constants

Values both implementations must agree on, so neither drifts on its own.

- Config directory: `$XDG_CONFIG_HOME/pvai`, or `~/.config/pvai` when the
  variable is empty or unset. `token`, `server.key`, `devices.json` live
  there. Whoever creates it makes the base directory 0755 and `pvai` 0700.
- base32 (fingerprints, pairing codes): RFC 4648 alphabet in lower case
  (`a`-`z`, `2`-`7`), no padding. A decoder accepts upper case as lower
  case; it rejects every other character, a trailing group of 5 or more
  bits, and non-zero leftover bits. The daemon and the app decode the same string to the same
  bytes or both refuse it.
- Pairing proof: 32 bytes of TLS exporter output (RFC 8446 7.5) with the
  label `EXPORTER-pvai-pair` and an empty context.
- Environment of an assistant process (the daemon's wrapper, the app's login
  check): remove every `CLAUDE_CODE_*` variable except
  `CLAUDE_CODE_OAUTH_TOKEN`, and `CLAUDECODE`, `PVAI_ID`, `PVAI_SOCK`. The
  prefix marks a nested session and would make Claude Code refuse to start;
  the OAuth token is the owner's login and stays.
- Finding `pvai`, `claude`, `codex`: the directories of `PATH` first (empty
  entries skipped), then `~/.local/bin`, `/usr/local/bin`,
  `/opt/homebrew/bin`; the first regular executable file wins, symlinks
  followed. An app started from Finder has a short `PATH`, which is what the
  three fixed directories are for. The daemon takes `PVAI_CLAUDE` as the
  path of `claude` before any search, with no fallback when it is set but
  not executable.
