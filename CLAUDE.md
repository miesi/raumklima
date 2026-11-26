# CLAUDE.md - raumklima

Guidance for Claude Code in this repository. Each part has its own CLAUDE.md with the rules
for its code: `hub/CLAUDE.md` (ESP-IDF). `node/` and `protocol/` come after the hub.

## Repository

| Path | Content |
|---|---|
| `hub/` | ESP-IDF firmware for the ESP32-S3 hub (CrowPanel 5.79 e-paper) |
| `node/` | (planned) nRF Connect SDK firmware for the nRF54L15 sensor nodes |
| `protocol/` | (planned) radio protocol between node and hub: description, C header, test vectors |
| `docs/plan.md` | system plan: nodes, radio, more than one hub, KNX |
| `hub/docs/plan.md` | hub plan and test results |
| `tools/receivers/` | local test receivers on the Mac (go-carbon, rsyslogd) |
| `tools/serial_run.py` | serial console helper for agents (`idf.py monitor` is interactive) |

The repository is public: github.com/miesi/raumklima. Everything that you commit must be
fit for publication: no secrets, no code without a license that lets you publish it.

## Git

- Commit and push only when Thomas tells you to do it.
- Write the commit message to a file and use `git commit -F <file>`. Use an imperative summary
  line. The body tells why.
- The remote uses the key of the GitHub account `miesi` (`core.sshCommand` in `.git/config`).
  The first key in the SSH agent belongs to a different account.
- Before `git add -A`, examine `git status --short --untracked-files=all`. Each part keeps
  files that are not for the repository (BSEC download, `wlan.config`, `sdkconfig`).

## Third-party code

- Permitted: code with a license that lets you publish it (MIT, BSD-3, Apache-2.0). Keep the
  license file.
- Not permitted: GPL code, vendor example code without a license, Bosch BSEC binaries and
  examples. You can read them for facts only.

## Writing style

- Code, comments, commit messages and documentation: English, ASCII only. Use `-`, not a
  dash. Use `...`, not an ellipsis. Use straight quotes. Do not use arrows or emojis.
- Documentation (README, plans): ASD-STE100 (Simplified Technical English).
- In documentation, mark each claim as VERIFIED (with the source) or ASSUMPTION.
- Config snippets get a comment for each line that is not obvious.

## Testing

A change is complete when its test on the hardware is satisfactory, not when the build is
successful. Report the test output as it is.
