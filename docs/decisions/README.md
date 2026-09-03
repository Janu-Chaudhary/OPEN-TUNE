# Decisions

A record per decision worth remembering. Two sources:

1. A resolved open question from `specs.md` §12.
2. A significant design decision made during implementation — a dependency chosen, an
   algorithm deviating from the obvious one, a constraint discovered the hard way.

Short — the point is that the *reasoning* survives, not just the outcome. The consequences
section matters most: it is what a future contributor needs and what the code cannot say.
Name: `NNNN-short-slug.md`.

Template:

```markdown
# NNNN — Title
**Date:** YYYY-MM-DD · **Resolves:** Q# · **Status:** accepted

## Context
What forced the decision. Two or three sentences.

## Options
- A — what it was, why it lost
- B — what it was, why it won

## Decision
One sentence.

## Consequences
What this makes easier, what it makes harder, what it rules out.
```
