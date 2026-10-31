# Installing pvai

pvai is the host side of PhiVoice AI. Install it on every machine where
Claude Code or Codex runs and that the PhiVoice AI app should reach; the
app itself runs on the Mac.

## Requirements

- **Systems**: Debian 13 (trixie) and Ubuntu 24.04 (noble) on amd64 or
  arm64, from the apt repository below; other Linux systems from source;
  macOS (Apple silicon or Intel) through Homebrew or from source.
- **Claude Code** (`claude` on the PATH) and/or **Codex** (`codex` on the
  PATH or in `~/.local/bin`). pvai is tested with Claude Code 2.1.314 and
  Codex 0.166.0. It reads hook payloads, status line fields and app server
  methods that older versions may lack, so update those first if something
  is missing (`claude update`, `npm install -g @openai/codex` or however
  you installed Codex).
- **For the app over ssh**: an ssh server on the host and a key login from
  the Mac (see "Connect the app").

## Install

### Debian and Ubuntu over apt

The repository is signed with this key:

```
pub   rsa4096 2026-02-08 [SC]
      196F CEFA B8FB B779 774B  D1A0 2206 F9E6 FEBE F00A
uid   Aptly Repo Signing <aptly@phisys.com>
```

Fetch the key, check that it is this one, then add the source for your
release (`trixie` on Debian 13, `noble` on Ubuntu 24.04; the line below
picks it from `/etc/os-release`):

```
sudo install -d /etc/apt/keyrings
curl -fsSL https://apt.phisys.com/repo-signing-key.asc -o /tmp/phisys.asc
gpg --show-keys --with-fingerprint /tmp/phisys.asc   # must show the fingerprint above
sudo gpg --dearmor -o /etc/apt/keyrings/phisys-archive-keyring.gpg /tmp/phisys.asc
. /etc/os-release
echo "deb [signed-by=/etc/apt/keyrings/phisys-archive-keyring.gpg] https://apt.phisys.com/testing $VERSION_CODENAME main" \
  | sudo tee /etc/apt/sources.list.d/phisys.list
sudo apt update && sudo apt install pvai
pvai --version
```

apt checks every package against the key; the fingerprint check above is
what makes the key itself trustworthy. (Plain `http://` works as well, for
older sources.) pvai is in `testing` for now; once it moves on to
`stable`, the same line with `stable` instead of `testing` follows only
released versions.

The package installs `/usr/bin/pvai`, two firewall profiles for direct
access (ufw and firewalld, see "Connect the app"; they open nothing) and a
fail2ban filter and jail that are off (see "fail2ban"): no
service, nothing in anyone's home directory. The daemon starts on demand, and each user sets pvai up
once (see "Set up per user").

### macOS with Homebrew

```
brew install phisys-ltd/tap/pvai
pvai --version
```

The formula lives in
[Phisys-Ltd/homebrew-tap](https://github.com/Phisys-Ltd/homebrew-tap) and
builds the release tarball from
[Releases](https://github.com/Phisys-Ltd/pvai/releases).

### From source

Dependencies: a C++20 compiler, CMake 3.21 or newer, Ninja, nlohmann-json
3 and OpenSSL 3.

```
# Debian / Ubuntu
sudo apt install build-essential cmake ninja-build nlohmann-json3-dev libssl-dev git
# macOS (with the Xcode command line tools: xcode-select --install)
brew install cmake ninja nlohmann-json openssl@3
```

Build, test and install (to `/usr/local/bin/pvai`):

```
git clone https://github.com/Phisys-Ltd/pvai.git && cd pvai
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
(cd build && ctest --output-on-failure)
sudo cmake --install build
```

On macOS, CMake finds Homebrew's OpenSSL by itself in most setups; if not,
add `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)` to the first `cmake`.

## Set up per user

Do this as the user that runs Claude Code or Codex.

**Claude Code.** pvai learns where a turn stands from Claude Code's hooks
and status line, configured in `~/.claude/settings.json`. Either run

```
pvai install-hooks
```

or just start the first session with `pvai run -- claude`, which does the
same when something is missing. That adds an entry `<path>/pvai hook` to
the hook events `SessionStart`, `UserPromptSubmit`, `Stop`,
`Notification` and `PermissionRequest`, and sets `statusLine` to `<path>/pvai statusline`. A status
line you already had keeps printing: pvai calls it through
`pvai statusline --chain '<your command>'`. Nothing else in the file
changes. Before each change the previous file is kept as
`~/.claude/settings.json.pvai-bak`. `pvai uninstall-hooks` takes the
entries out again.

Then start Claude Code through pvai. With an alias in `~/.bashrc` or
`~/.zshrc` nothing else changes for you:

```
alias claude='pvai run -- claude'
```

The session looks and works as always; the app sees it while it runs.
`pvai run --name NAME -- claude …` gives it a label in the app.

**Codex** needs no hooks. Start the Codex TUI as `pvai codex …` (all
arguments go to Codex), or with an alias:

```
alias codex='pvai codex'
```

The daemon also finds Codex threads that the app starts itself.

## Connect the app

The app reaches a host in one of two ways.

### (a) Over ssh (recommended)

The app opens `ssh -N -o BatchMode=yes -L …:127.0.0.1:7470 USER@HOST`
and runs `ssh USER@HOST pvai token` first. So:

1. Log in with a key, without a password or passphrase prompt: put the
   Mac's public key into the host's `~/.ssh/authorized_keys` (for example
   `ssh-copy-id USER@HOST`) and keep the key in the Mac's agent or
   keychain (`ssh-add --apple-use-keychain ~/.ssh/id_ed25519`).
2. Check from the Mac, exactly as the app does it:

   ```
   ssh -o BatchMode=yes USER@HOST pvai token
   ```

   This must print a 32-character token and nothing else, without any
   question. It also starts the daemon on the host if it is not running.
3. `pvai` must be on the PATH of this non-login, non-interactive shell.
   `/usr/bin` (Debian package) and `/usr/local/bin` always are. Homebrew
   on Apple silicon (`/opt/homebrew/bin`) is not: on a Mac host, add
   `export PATH="/opt/homebrew/bin:$PATH"` to `~/.zshenv` (zsh reads that
   file for every shell, `~/.zshrc` only for interactive ones). On macOS,
   ssh must be turned on: System Settings → General → Sharing → Remote
   Login.
4. In the app, add the host as `USER@HOST`.

### (b) Direct, without ssh (TLS)

For devices without ssh, such as an iPhone. Turn on listening on one
address of the host (port 7471 unless you give one):

```
pvai listen 192.168.1.10          # or 192.168.1.10:7471; 0.0.0.0 for every interface
pvai pair                         # prints a link and a QR code, valid 10 minutes for one device
```

Scan the code or open the link in the app. Only paired devices get past the
TLS handshake; `pvai devices` lists them, `pvai revoke NAME` removes one,
`pvai listen off` turns it off.

Let port 7471 through the host's firewall, ideally only from your own
network:

```
# ufw (the Debian package brings the profile "pvai")
sudo ufw allow from 192.168.1.0/24 to any app pvai
# without the package: ... to any port 7471 proto tcp

# firewalld (the Debian package brings the service "pvai")
sudo firewall-cmd --permanent --add-rich-rule='rule family=ipv4 source address=192.168.1.0/24 service name=pvai accept'
sudo firewall-cmd --reload

# nftables (with an inet filter table and an input chain; add it to
# /etc/nftables.conf to keep it after a reboot)
sudo nft add rule inet filter input ip saddr 192.168.1.0/24 tcp dport 7471 accept

# iptables (keep it with: sudo apt install iptables-persistent; sudo netfilter-persistent save)
sudo iptables -A INPUT -p tcp -s 192.168.1.0/24 --dport 7471 -j ACCEPT
```

The package only describes the port; it opens nothing by itself. Open
it only for networks you know (your LAN, a VPN), not for the whole
internet: a device that is not paired never gets past the TLS handshake,
but every open port draws scanners.

### fail2ban

Each refused direct access goes to the system log (ident `pvai`, facility
`auth`), one line with the client's address and a fixed reason, nothing
the client sent:

```
pvai[1234]: rejected 203.0.113.7: unknown device
```

The reasons: `unknown device`, `no client certificate`, `handshake
failed`, `handshake timed out`, `no pairing window`, `bad proof`,
`pairing timed out`. pvai itself drops an address for a minute after 5
failures within a minute (`blocked <address>: repeated failures`, doubling
up to an hour). fail2ban bans it at the firewall for longer: the Debian
package brings `/etc/fail2ban/filter.d/pvai.conf` and
`/etc/fail2ban/jail.d/pvai.conf` (port 7471, the journal, 5 failures in
10 minutes, banned for an hour), off until you turn it on:

```
sudo apt install fail2ban
sudo sed -i 's/^enabled = false/enabled = true/' /etc/fail2ban/jail.d/pvai.conf
sudo systemctl reload fail2ban
sudo fail2ban-client status pvai
```

With `pvai listen` on another port, change `port` there. Without the
journal (rsyslog writing auth messages to a file), use the `backend =
auto` and `logpath` lines in the jail instead. Without the package, copy
both files from `contrib/fail2ban/` in the source.

On macOS with the firewall on (System Settings → Network → Firewall),
allow incoming connections for pvai when macOS asks, or beforehand:

```
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add "$(command -v pvai)"
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp "$(command -v pvai)"
```

Only a running daemon listens. After a reboot it starts with the first
`pvai run`, `pvai codex`, `pvai listen` or `pvai token`.

## Update

- Debian: `sudo apt update && sudo apt upgrade`.
- Homebrew: `brew upgrade pvai`.
- From source: `git pull`, build and install as above.

Then restart the daemon with `pvai daemon restart`; running sessions
report to the new one by themselves. It refuses while prompts wait in a
session's queue (they live in the daemon's memory and would be lost);
`pvai daemon restart --force` drops them and lists which. A pvai binary in a new place (another
prefix) makes `pvai run` rewrite the hooks on its next start.

**macOS, when you replace the binary by hand**: delete the old file first,
then copy the new one (`rm /usr/local/bin/pvai && cp build/pvai
/usr/local/bin/`). Copying over the existing file changes it in place,
and macOS kills every running pvai process with SIGKILL because its code
signature no longer matches. `cmake --install`, `brew` and `apt` replace
the file and are safe.

## Remove

As each user that set pvai up:

```
pvai uninstall-hooks              # takes pvai's hooks and status line out of ~/.claude/settings.json
pvai daemon stop --force         # stops this user's daemon
rm -r ~/.config/pvai              # token, TLS key, paired devices, listen setting
```

and remove the `claude`/`codex` aliases. `uninstall-hooks` restores a
status line that pvai chained and leaves the rest of the file as it is.
Then the program:

- Debian: `sudo apt remove pvai`, and if you like
  `/etc/apt/sources.list.d/phisys.list` and
  `/etc/apt/keyrings/phisys-archive-keyring.gpg`.
- Homebrew: `brew uninstall pvai`.
- From source: `sudo rm /usr/local/bin/pvai`.

## Troubleshooting

- **What does the daemon see?** `pvai sessions` lists the host's sessions,
  `pvai watch` prints everything it tells the apps, live.
- **The app says the token is wrong / cannot connect.** Run the check in
  "Connect the app" (a). The token is `~/.config/pvai/token` of the user
  the app logs in as; it must be the same user that runs Claude Code.
  Deleting the file makes a new token; the app fetches it again on its
  next connection.
- **A session does not show up.** Was it started through `pvai run` or
  `pvai codex`? Is `pvai install-hooks` happy ("in place")? Is the
  daemon running (`pgrep -fl 'pvai daemon'`)?
- **Restart the daemon**: `pvai daemon restart` (refused while prompts are
  queued; `--force` drops them). A daemon that no longer answers:
  `pkill -f '^pvai daemon'; pvai token >/dev/null`.
  Its log is `daemon.log` in `$XDG_RUNTIME_DIR/pvai/` (on macOS and
  without `XDG_RUNTIME_DIR`: `$TMPDIR/pvai-<uid>/`).
- **The daemon dies when the ssh connection ends.** On systems where
  logind kills a user's processes at logout (`KillUserProcesses=yes`),
  allow them to linger: `sudo loginctl enable-linger USER`.
- **Direct access does not answer.** `pvai listen` shows what the daemon
  listens on; check the firewall; a device that was revoked or a host
  whose key changed must pair again (`pvai pair`).
