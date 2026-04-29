---
name: tree-thinking
description: 'Use when designing systems, architecture, modules, algorithms, or performance-sensitive features: think in English, decompose the system top-down like a tree, simulate end-to-end flows, refine each module until every leaf is a concrete algorithm or simple implementation, choose the fastest viable design, and reject compatibility-preserving approaches.'
argument-hint: 'Describe the system, feature, module, or algorithm to design with top-down performance-first reasoning.'
user-invocable: true
---

# Tree Thinking

Use this skill for strict system design, architecture planning, module decomposition, algorithm selection, and performance-sensitive implementation design.

## Core Rules

- Think and write technical design artifacts in English unless the repository explicitly requires another language.
- Start from the whole system goal, then refine it top-down into subsystems, modules, responsibilities, data flow, and concrete operations.
- Treat the design as a tree: every parent node must explain why its child nodes exist, and every leaf node must be precise enough to implement directly.
- Keep simulating the feature through the system while refining the tree. Validate how data, ownership, control flow, errors, synchronization, and resource lifetime move across module boundaries.
- Stop decomposition only when a leaf is either a concrete algorithm, a direct data transformation, a small state transition, or a simple implementation step.
- Compare viable choices at each meaningful branch and choose the option with the best expected performance under the actual workload and constraints.
- If a design depends on compatibility layers, fallback paths, adapters, shims, or preserving obsolete behavior, discard that branch and design a cleaner latest-only path.
- Do not keep multiple overlapping designs alive. Select one design and make the tradeoff explicit.

## Procedure

1. Define the system goal, inputs, outputs, invariants, and performance constraints.
2. Build the root-level tree: identify the minimal set of modules that must cooperate to satisfy the goal.
3. For each module, define ownership, state, data layout, public boundary, and execution order.
4. Simulate the primary flow from entry point to final output. Record where data is created, transformed, cached, synchronized, consumed, and destroyed.
5. Simulate failure and edge flows only when they affect correctness, lifetime, synchronization, or performance.
6. Refine unclear nodes until each leaf maps to a concrete algorithm or a simple implementation.
7. At each branch with multiple feasible options, compare CPU cost, GPU cost, memory bandwidth, allocation behavior, cache locality, synchronization cost, latency, and complexity.
8. Pick the fastest viable option that keeps ownership and control flow clear.
9. Run the compatibility rejection gate. Remove any design branch that preserves legacy behavior or adds a transitional layer without an explicit user requirement.
10. Present the selected design as the final tree, followed by the key flow simulation and the rejected alternatives.

## Compatibility Rejection Gate

Reject the current design and restart the affected branch when it includes any of these without an explicit requirement:

- Legacy interface preservation.
- Fallback implementation paths.
- Compatibility shims or adapters.
- Dual old/new execution paths.
- Pass-through wrapper layers.
- Transitional code that does not represent the final architecture.

## Output Shape

When using this skill, produce a concise design artifact with these sections:

1. Goal and constraints.
2. Top-down module tree.
3. Flow simulation.
4. Algorithm-level leaves.
5. Performance decision.
6. Rejected alternatives.
7. Implementation order.

Keep the output technical, direct, and implementation-ready.
