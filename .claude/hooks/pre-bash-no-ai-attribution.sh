#!/usr/bin/env bash
# Block any git commit whose message contains AI attribution.
# Project rule: "No AI attribution in commits" is non-negotiable.

input=$(cat)

if echo "$input" | grep -qE 'git[[:space:]]+commit'; then
  if echo "$input" | grep -qiE '(co-authored-by:[[:space:]]*claude|generated[[:space:]]+with[[:space:]]+\[?claude|claude[[:space:]]+code|noreply@anthropic\.com|🤖[[:space:]]*Generated)'; then
    echo "ERROR: commit message contains AI attribution. Project rule: no AI attribution in commits." >&2
    echo "       Strip the Co-Authored-By / Generated-with / Claude Code lines and retry." >&2
    exit 2
  fi
fi

exit 0
