# Commit convention

Every commit in this repo explains itself. Not what changed — `git diff` already says that —
but **why it was built this way, and what it might do to us later.**

This is not ceremony. We are writing a PowerPC recompiler and a GPU backend against a moving
browser target, and most of the expensive mistakes in a project like this are decisions that
looked obvious at the time and left no record of the alternatives considered. A year from now
the only person who can explain a design choice is the commit that made it.

## Format

```
<type>(<scope>): <imperative summary, ≤72 chars>

## What this is
Plain description of the feature or change. What it does, what it is for.

## Design choices
The concrete decisions taken. Structure, data flow, boundaries, dependencies.

## Why
Why these choices over the alternatives. Name the alternatives and say why they lost.
If a decision was forced by a constraint, name the constraint.

## Caveats and risks
What is incomplete, assumed, or known-weak. Things a reviewer should not mistake for
finished. Performance characteristics we have not measured.

## What might go wrong
Concrete failure modes, in a form a future reader can check against. Prefer
"if X, then Y breaks" over "this might be fragile."
```

All five sections are required. A section with nothing to say gets `None.` — an explicit
"nothing here" is information; a missing heading is an unanswered question.

## Types

| Type | Use for |
|---|---|
| `feat` | New capability |
| `fix` | Corrects wrong behaviour |
| `perf` | Changes speed or memory, with numbers in the body |
| `spike` | Throwaway experiment answering one question — body must state the answer |
| `chore` | Build, tooling, dependencies, repo structure |
| `docs` | Documentation only |
| `refactor` | Behaviour-preserving restructure |
| `test` | Harness, fixtures, oracle traces |

## Scopes

`ppc2wasm` · `dolphin-web` · `webgpu` · `harness` · `web` · `portal` · `storage` · `repo`

## Example

```
perf(ppc2wasm): lazy condition-register evaluation

## What this is
Defers computing PowerPC condition-register bits until an instruction actually reads
them, storing the operands and the comparison kind instead.

## Design choices
CR state is a tagged pair (op_kind, lhs, rhs) rather than four resolved bits. Readers
call resolve_cr(), which materialises bits on demand and memoises. Dirty tracking is
per-CR-field, not per-register.

## Why
Most PowerPC arithmetic sets CR0, and most of those results are never read. Computing
all four bits eagerly costs 3-4 extra WASM instructions per arithmetic op. Resolving on
demand moves that cost to the ~15% of sites that actually branch on it.

Rejected: emitting the bits eagerly and letting the WASM engine DCE them. Verified it
does not — the stores escape into the guest-state struct, so the engine cannot prove
them dead.

## Caveats and risks
Memoisation cache is not invalidated across a guest context switch. Safe today because
we have one guest thread, unsafe the moment we emulate more.

## What might go wrong
If an interrupt lands between the operand capture and the resolve, we resolve against
stale operands and branch the wrong way. Guarded by resolving CR eagerly at every
interrupt boundary — if that guard is removed, expect nondeterministic wrong branches
that reproduce only under load.
```

## Setup

`scripts/bootstrap.sh` points `commit.template` at `.gitmessage`, so `git commit` opens
with the headings prefilled.

```sh
git config commit.template .gitmessage
```
