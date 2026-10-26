---
name: ship
description: Turn finished, verified work into focused commits (one per fix or concern, each building and passing tests on its own), push the branch and open or prepare a PR into dev. Use when the user asks to commit, split work into commits, push, or open a PR.
---

# Commit, push, PR

Only commit when the user asks. PRs target **`dev`** (`main` lags far behind).
Branch names: `fix/…`, `feat/…`, `chore/…`, created from an up-to-date `dev`.

## 1. Plan the split

Group the diff by concern (one fix / one feature slice per commit) and order them
so each commit compiles and passes tests: interfaces and core fixes first, then
code that depends on them, then routes/UI/docs. Tests move with the code they
cover (a commit adds the regression test for its own fix). Propose the split to
the user if it isn't obvious.

## 2. Build each commit's tree

When files contain changes for several commits (very common), do not split hunks
by hand:

1. Back up the final state of every changed and untracked file into the
   scratchpad (`git status --porcelain -uall`; copy untracked dirs explicitly).
2. For each multi-commit file, generate the intermediate versions with a small
   Python script that applies exact-match `replace` edits (assert each anchor
   occurs exactly once) to `git show HEAD:<file>` or removes pieces from the
   final file. For test files, include only the tests whose code exists at that
   commit (and their includes/helpers).
3. Self-check: rebuilding the last commit's version must reproduce the final
   file byte for byte (`assert generated == final`).
4. Reset the tree to `HEAD` (`git reset -q && git checkout -q -- . && rm` new
   files), then for each commit: copy in its files, `git add` them, **verify**,
   commit.
5. After the last commit, `diff -rq <backup> .` must show the tree identical to
   the verified final state, and `git status` must be clean.

Verify at every commit:

```bash
(cd src/orcha && cmake cmake-build-debug >/dev/null && cmake --build cmake-build-debug -j 8) \
  && src/orcha/cmake-build-debug/test_new_features && src/orcha/cmake-build-debug/test_dependency_resolver
```

For UI-only commits also run the JS syntax check from the `admin-ui` skill, and
for an early commit that changes behaviour on its own, exercise it (e.g. a
focused browser or curl check) before committing.

Gotcha: in a script with `set -euo pipefail`, an expected non-zero exit in a
pipeline (e.g. `ls` of deleted files) aborts the script; confirm with `git log`
before assuming a commit failed.

## 3. Commit messages

Subject in imperative mood, ≤ 72 chars, describing the user-visible effect
("Fail workflow steps whose command reports {"success": false}"). Body: what was
wrong and why it mattered, then what changed; mention behaviour changes. End
with the attribution trailer the session instructions specify.

## 4. Push and PR

```bash
git fetch -q origin && git log --oneline origin/dev..HEAD   # only your commits
git push -u origin <branch>
```

Write the PR description to a file in the scratchpad: a table of commits →
defect → fix, **Behaviour changes** (what existing clients/deployments must do),
**Testing** (suites, sanitizers, end-to-end, with numbers), **Follow-ups**. End it
with the PR attribution line the session instructions specify. Then
`gh pr create --base dev --head <branch> --title … --body-file <file>`.
If `gh auth status` says not logged in, suggest `! gh auth login`, or give the
user the `https://github.com/sbrosgol/orcha/pull/new/<branch>` link and the
description file path.

## 5. After the user merges

`git fetch && git checkout dev && git merge --ff-only origin/dev`, confirm the
branch is merged (`git branch --merged dev`), and suggest deleting it remotely.
