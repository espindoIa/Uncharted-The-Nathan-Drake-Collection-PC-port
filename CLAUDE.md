# CLAUDE.md

@AGENTS.md

## Claude Code specifics

- Ship rules: read `.claude/ship.md` first.
- Build and test commands: AGENTS.md "Commands" (same commands in `.claude/ship.md`). Measuring: AGENTS.md "Measuring".
- Read the index block under the title of `analysis/STATUS.md` before digging in it.
- Installing a change into the player's port (any "atualiza/instala o port", "manda pro jogo", "testa no jogo" request): run `tools/dev_sync.sh` from Git Bash, not `package_windows.sh` + `update_port.sh`. Add `--build` when C/C++ sources changed. Check the game is closed first (the script stops if `gow3-probe.exe` runs). In a worktree there is no `gow3_pc/out`: build where one exists and pass `OUT=<that folder>/gow3_pc/out`, only if that build holds this worktree's changes. Use the full package + `update_port.sh` only for a release, or when the DLL set, `python\` or the licenses changed (dev_sync copies none of those). Report the file list it printed.
