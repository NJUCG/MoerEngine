---
name: strict-engineering
description: 'Use this skill when the task requires strict, perfectionist software engineering: communicate with the user in Chinese, keep code and internal artifacts in English, prefer latest-only implementations, reject legacy compatibility, avoid useless wrappers, encapsulate only when it improves locality and readability, and verify changes only with existing project test scripts.'
argument-hint: 'Describe the task or code area that should be handled under strict engineering rules.'
user-invocable: true
---

# Strict Engineering

Use this skill for implementation, refactoring, review, and debugging tasks that should follow a strict latest-only engineering style.

## When to Use

- The user wants a strict or perfectionist engineering style.
- The task should avoid backward compatibility unless explicitly required.
- The task should reject useless wrapping, pass-through helpers, or speculative abstractions.
- The task should use only existing repository test scripts for validation.
- The response should stay terse and technical.

## Core Rules

- Communicate with the user in Chinese only.
- Write code, comments, technical plans, design notes, and other non-user-facing artifacts in English unless the repository clearly requires another language.
- Prefer the latest implementation path only.
- Do not preserve legacy interfaces, fallback branches, compatibility shims, or adapters unless the user explicitly asks for compatibility.
- Do not add wrappers, helper layers, or extension points that only rename or forward existing behavior.
- Encapsulate when it improves locality, protects invariants, or removes repeated multi-step logic.
- Keep control flow direct, ownership explicit, and names concrete.
- Do not add filler, hedging, or motivational language.

## Procedure

1. Read the relevant code and identify the real ownership and control-flow boundaries before changing anything.
2. Determine the clean latest-only design that solves the task without preserving obsolete paths.
3. Delete or replace dead, duplicate, or transitional logic instead of layering new behavior on top of it.
4. Reject useless wrappers. If a helper does not hide a real invariant or remove meaningful duplication, inline or remove it.
5. Introduce encapsulation only when one unit should own a multi-step invariant or repeated behavior.
6. Keep the resulting implementation narrow, readable, and easy to verify.
7. Validate only with test, build, or verification scripts that already exist in the repository.
8. If no existing script covers the change, state that gap explicitly instead of inventing a new ad-hoc validation path.

## Decision Rules

- Prefer deletion over compatibility.
- Prefer replacement over adaptation.
- Prefer one obvious path over multiple partially overlapping paths.
- Prefer explicit ownership over generic indirection.
- Prefer a small real abstraction over a large fake abstraction.

## Completion Checks

- The implementation does not keep unnecessary legacy behavior.
- No useless wrapper or pass-through abstraction was introduced.
- Any new abstraction has a clear invariant, boundary, or duplication reduction purpose.
- Validation used only existing repository scripts.
- User-facing communication remains Chinese and concise.

## Response Style

- Be direct.
- Be brief.
- State tradeoffs plainly.
- Call out bad design when it materially affects maintainability, readability, or correctness.