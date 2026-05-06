#!/usr/bin/env bash
# Block any `gh pr create` that doesn't target Haglerd/homekit-ratgdo32.
# Reads the tool input from stdin (Claude Code PreToolUse hook contract).

input=$(cat)
cmd=$(echo "$input" | grep -oE '"command"\s*:\s*"[^"]*"' | sed -E 's/.*"command"\s*:\s*"(.*)"/\1/')

# Only inspect gh pr create invocations
if echo "$cmd" | grep -qE 'gh\s+pr\s+create'; then
  if ! echo "$cmd" | grep -qE -- '--repo\s+Haglerd/homekit-ratgdo32'; then
    echo "ERROR: gh pr create must include --repo Haglerd/homekit-ratgdo32." >&2
    echo "       Fork-only routing is non-negotiable. Add --repo Haglerd/homekit-ratgdo32." >&2
    exit 2  # exit code 2 blocks the tool call
  fi
fi

# Block pushes to upstream remote
if echo "$cmd" | grep -qE 'git\s+push'; then
  if echo "$cmd" | grep -qE 'ratgdo/homekit-ratgdo32'; then
    echo "ERROR: never push to ratgdo/homekit-ratgdo32 (upstream)." >&2
    exit 2
  fi
fi

exit 0
