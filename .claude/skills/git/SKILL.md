---
name: git
description: Git operations for MoerEngine — branch strategy, merge priority, commit format.
---

# Git Skill

## Branch Strategy

- **`main`** — stable branch, production-ready
- **`dev_parallel_rhi`** — active RHI development branch (current workspace default)

### Merge Priority

**`dev_parallel_rhi` has higher merge priority than `main`.**

When integrating changes:
1. Merge/cherry-pick into `dev_parallel_rhi` first
2. `dev_parallel_rhi` → `main` happens later, in batched releases

When resolving conflicts between the two, prefer `dev_parallel_rhi`'s version — it represents the active development line.

## Commit Format

```
feat: [简短功能描述]
详细说明（多行）

- 具体变更点1
- 具体变更点2

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

### Rules
- **Subject line:** `feat:` prefix, followed by a short description in Chinese or English. Keep it under 72 chars.
- **Body:** one blank line after subject, then detailed description. Explain what changed and why.
- **Scope tags** (optional, after `feat:`): `(rhi)`, `(render)`, `(vk)`, `(editor)`, `(build)`
  - `feat(rhi): add timeline semaphore support for queue submission`
  - `feat(vk): fix VUID-12221 descriptor buffer address validation`
  - `feat(editor): add scene reload on file change`

### Examples

```
feat(rhi): fix descriptor buffer validation for non-UAV buffers

Auto-add STORAGE_BUFFER_BIT | SHADER_DEVICE_ADDRESS_BIT to buffer usage
flags in METoVKBufferUsageFlags() so vkGetDescriptorEXT passes validation
for buffers that aren't explicitly flagged as UAV.

- Fixes VUID-VkDescriptorGetInfoEXT-type-12221 and -12222
- Only applies when usage doesn't include ACCELERATION_STRUCTURE
```

```
feat: set Raster as default renderer, fix init barrier and shader issues
```

## Common Operations

### Check current branch and status
```bash
git branch --show-current
git status -s
```

### Create a feature branch from dev_parallel_rhi
```bash
git checkout dev_parallel_rhi
git checkout -b feat/my-feature
```

### Commit with the required format
```bash
git add <files>
git commit -m "feat: [short description]" -m "[detailed body]"
```

Note: The first `-m` is the subject, the second `-m` (and beyond) are the body paragraphs.
Git auto-adds a blank line between subject and body.

### Interactive rebase (for cleanup before PR)
```bash
git rebase -i HEAD~N   # N = number of commits to clean up
```
Interactive rebase requires a TTY — it won't work in this agent environment. Instead, use `git reset --soft` to squash: `git reset --soft HEAD~N && git commit -m "..."`

### Merge dev_parallel_rhi into your feature branch
```bash
git checkout feat/my-feature
git merge dev_parallel_rhi
```

## Remote

- **GitHub org/repo:** Check `git remote -v` for the exact URL
- Default remote name is `origin`
- Push feature branches before creating a PR: `git push -u origin feat/my-feature`
