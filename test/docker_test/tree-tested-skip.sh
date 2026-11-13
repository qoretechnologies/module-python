#!/bin/sh
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
# Source before pending status/build/test commands. Adapted from qore commit
# 22a9b5c721 as in module-ncurses (73d328c) and module-pgsql (251bac2).
# Returning runs the full job; exiting skips its already-tested work.
#
# The job reports success under its own GitHub status context
# ("${REPO_NAME}/${CI_JOB_NAME}"); the pipeline's "${REPO_NAME}" context is set by
# the github-status-* jobs of .gitlab-ci.yml, which do not source this file.

if [ "${CI_COMMIT_BRANCH:-}" != develop ] || [ "${CI_PIPELINE_SOURCE:-}" != push ] \
        || [ "${QORE_CI_FORCE_FULL:-}" = 1 ] || [ "${QORE_TREE_TESTED_CHECKED:-}" != 1 ]; then
    return 0
fi
case "${QORE_TREE_TESTED_IN:-}" in
    ''|*[!0-9]*|0) return 0 ;;
esac
case "${QORE_TREE_TESTED_SHA:-}" in
    ''|*[!0-9a-f]*) return 0 ;;
esac
if [ "${#QORE_TREE_TESTED_SHA}" -ne 40 ] && [ "${#QORE_TREE_TESTED_SHA}" -ne 64 ]; then
    return 0
fi
current_tree=$(git rev-parse 'HEAD^{tree}' 2>/dev/null) || return 0
if [ "${QORE_TREE_TESTED_IN}" = "${CI_PIPELINE_ID:-}" ] \
        || [ "${QORE_TREE_TESTED_TREE:-}" != "$current_tree" ] \
        || [ -z "${GITHUB_ACCESS_TOKEN:+set}" ] || [ -z "${CI_PROJECT_URL:-}" ] \
        || [ -z "${CI_COMMIT_SHA:-}" ] || [ -z "${REPO_NAME:-}" ] || [ -z "${CI_JOB_NAME:-}" ]; then
    return 0
fi

tested_url="${CI_PROJECT_URL}/-/pipelines/${QORE_TREE_TESTED_IN}"
echo "this tree passed every test job in pipeline ${QORE_TREE_TESTED_IN}: ${tested_url}"
# Never trace the credential. If the status cannot be published, run the full
# job so its normal status/reporting path remains responsible for the result.
set +x
if curl -fsS --max-time 30 \
        "https://api.github.com/repos/qoretechnologies/${REPO_NAME}/statuses/${CI_COMMIT_SHA}" \
        -X POST --oauth2-bearer "${GITHUB_ACCESS_TOKEN}" -H "Content-Type: application/json" \
        -d "{\"state\":\"success\",\"context\":\"${REPO_NAME}/${CI_JOB_NAME}\",\"description\":\"tree tested in pipeline ${QORE_TREE_TESTED_IN}\",\"target_url\":\"${tested_url}\"}" \
        > /dev/null; then
    exit 0
fi
echo "could not publish the successful-tree status: running the full job"
