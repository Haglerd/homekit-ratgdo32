#!/usr/bin/env bash
# Branch-shift guard: block git mutating operations when the working tree's
# current branch differs from the branch we last operated on. Catches the
# class of failure where another process (auto-release workflow, scheduled
# agent, parallel session) switches branches mid-flight, causing my next
# commit to land on the wrong branch.
#
# State file: .git/.claude_session_branch
# - Stamped on every git mutating op
# - If current branch differs from stamp at next op → block with override path

input=$(cat)
cmd=$(echo "$input" | grep -oE '"command"[[:space:]]*:[[:space:]]*"[^"]*"' | sed -E 's/.*"command"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/')

if ! echo "$cmd" | grep -qE 'git[[:space:]]+(commit|push|cherry-pick|rebase|merge|reset|stash[[:space:]]+(push|pop|drop|clear)|tag[[:space:]]+-)'; then
  exit 0
fi

git_dir=$(git rev-parse --git-dir 2>/dev/null)
if [ -z "$git_dir" ]; then
  exit 0
fi

current_branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null)
state_file="$git_dir/.claude_session_branch"

if [ ! -f "$state_file" ]; then
  echo "$current_branch" > "$state_file"
  exit 0
fi

expected=$(cat "$state_file")
if [ "$expected" != "$current_branch" ]; then
  echo "ERROR: branch shifted mid-session." >&2
  echo "       Expected branch (last seen):  $expected" >&2
  echo "       Current branch:               $current_branch" >&2
  echo "       Something else (auto-release workflow, scheduled agent," >&2
  echo "       parallel session) likely checked out a different branch." >&2
  echo "" >&2
  echo "       Re-orient before continuing. If the new branch is correct," >&2
  echo "       re-baseline by running:" >&2
  echo "         rm \"$state_file\"" >&2
  exit 2
fi

echo "$current_branch" > "$state_file"
exit 0
