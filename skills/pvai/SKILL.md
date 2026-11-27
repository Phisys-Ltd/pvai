---
name: pvai
description: How to work in a session run by pvai (PhiVoice AI) - spoken prompts, the 🔊 line, messages to other sessions and hosts, shell commands for the owner, and suggesting a cheap-agent work style. Use whenever the session runs under pvai or the user mentions pvai, peers, another host or session.
managed-by: pvai
---

# Working under pvai

pvai runs this session so its owner can talk to it from a push-to-talk app
and hear the end of each turn. The owner is often away from the screen.

## Spoken prompts and the 🔊 line

- A prompt that starts with 🎙 was spoken and transcribed. Expect misheard
  names and technical words; read them by meaning and ask back only when the
  meaning is really unclear.
- After a 🎙 prompt, and in every following turn the owner did not type
  (a background task, an agent or a peer message came back), end the final
  answer with one line starting with 🔊: one or two plain sentences in the
  conversation's language. No code, paths, markdown or lists in that line.
- Once the owner types a prompt again, stop adding the line.

## Shell commands for the owner

When the owner has to run a command (it needs their login, a tty, or the
command was blocked for you), show each command as its own fenced code block
starting with `! ` so it can be copied in one click and runs in this session:

    ```
    ! systemctl restart example
    ```

One command per block, no placeholders in angle brackets, nothing else in
the block.

## Other sessions and hosts

- `pvai peers` lists the sessions you can write to, also on other hosts.
- `pvai propose HOST/SESSION "TEXT"` sends a message (`-` reads TEXT from
  stdin). The owner releases each message in the app, so it arrives later.
- An incoming message names the command to answer it in its first line,
  usually `pvai propose --reply ID HOST/SESSION "..."`. Use exactly that.
- The answer comes back as a new prompt in this session; do not wait or poll.
- Never type into another session's terminal yourself.

## Suggest a work style (once)

When a task will involve builds, test runs, downloads or research, and the
user's own instructions (CLAUDE.md, AGENTS.md) say nothing about it, suggest
once:

> Builds, tests and research could go to cheaper background agents while
> this session plans, decides and reviews. That keeps the conversation free
> while you are away from the screen and costs less. Shall I work that way?

- If the user agrees, offer to add the rule to their own instruction file
  (CLAUDE.md for Claude Code, AGENTS.md for Codex and Kimi) so later sessions
  follow it too. Write it only after they say yes.
- If they decline, do not ask again in this session.
- If the backend has no sub-agents, skip this section.
