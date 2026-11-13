#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Find successful CI for an identical develop tree; uncertainty runs the full pipeline.

Based on qore commit 22a9b5c721's tree-already-tested.sh, as adapted in module-ncurses
(73d328c) and module-pgsql (251bac2). Python is available in both module CI images and
lets us validate API JSON and test job evidence.
"""

import json
import os
from http.client import HTTPException
from pathlib import Path
import subprocess
from urllib.error import URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


# Every job of the test stage of .gitlab-ci.yml: a pipeline is evidence for the tree only
# when all of them passed (test_tree_tested.py checks that this set matches the CI config).
REQUIRED_JOBS = frozenset(("test-ubuntu", "test-ubuntu-python3.11", "test-ubuntu-python3.15",
                           "test-ubuntu-python3.15t", "test-ubuntu-python3.16", "test-alpine"))


class EvidenceUnavailable(Exception):
    """No trustworthy evidence is available; this is not a reason to skip tests."""


class GitLabApi:
    def __init__(self, env):
        self.base = env["CI_API_V4_URL"].rstrip("/")
        self.headers = [
            {header: env[token]}
            for header, token in (("JOB-TOKEN", "CI_JOB_TOKEN"),
                                  ("PRIVATE-TOKEN", "QORE_CI_READ_API_TOKEN"))
            if env.get(token)
        ]

    def read(self, path):
        # Do not print exceptions, URLs with credentials, response bodies or tokens.
        for headers in self.headers:
            try:
                request = Request(self.base + path, headers=headers)
                with urlopen(request, timeout=15) as response:
                    return json.load(response)
            except (OSError, URLError, HTTPException, ValueError):
                continue
        raise EvidenceUnavailable("cannot read valid pipeline evidence")


def git(*args):
    return subprocess.run(
        ["git", *args], check=True, capture_output=True, text=True, timeout=15
    ).stdout.strip()


def eligible(env):
    return (env.get("CI_COMMIT_BRANCH") == "develop"
            and env.get("CI_PIPELINE_SOURCE") == "push"
            and env.get("QORE_CI_FORCE_FULL") != "1")


def positive_id(value):
    return type(value) is int and value > 0


def find_evidence(env, api):
    project = int(env["CI_PROJECT_ID"])
    current_pipeline = int(env["CI_PIPELINE_ID"])
    if project <= 0 or current_pipeline <= 0:
        raise EvidenceUnavailable("invalid CI identity")
    head = git("rev-parse", "HEAD")
    if head != env["CI_COMMIT_SHA"]:
        raise EvidenceUnavailable("checkout does not match the pipeline commit")
    tree = git("rev-parse", "HEAD^{tree}")
    candidates = [head]
    # Missing shallow-history parents cannot establish an identical tree. No fetch
    # is necessary: retaining the full pipeline is the conservative outcome.
    for parent in git("rev-list", "--parents", "-n", "1", "HEAD").split()[1:]:
        try:
            if git("rev-parse", parent + "^{tree}") == tree:
                candidates.append(parent)
        except subprocess.CalledProcessError:
            continue

    for commit in candidates:
        query = urlencode({"sha": commit, "status": "success", "per_page": 20})
        pipelines = api.read(f"/projects/{project}/pipelines?{query}")
        if not isinstance(pipelines, list):
            raise EvidenceUnavailable("pipeline response is not a list")
        for pipeline in pipelines:
            if not isinstance(pipeline, dict):
                continue
            pipeline_id = pipeline.get("id")
            if (not positive_id(pipeline_id) or pipeline_id == current_pipeline
                    or pipeline.get("project_id") != project
                    or pipeline.get("sha") != commit
                    or pipeline.get("status") != "success"):
                continue
            jobs = api.read(f"/projects/{project}/pipelines/{pipeline_id}/jobs"
                            "?per_page=100&include_retried=false")
            if not isinstance(jobs, list):
                raise EvidenceUnavailable("job response is not a list")
            # A successful pipeline with only a lint/manual/subset job does not
            # establish that every test suite passed. Retried jobs are omitted.
            platform_jobs = [job for job in jobs if isinstance(job, dict)
                             and isinstance(job.get("name"), str)
                             and job.get("name") in REQUIRED_JOBS]
            if (len(platform_jobs) == len(REQUIRED_JOBS)
                    and {job["name"] for job in platform_jobs} == REQUIRED_JOBS
                    and all(job.get("status") == "success" for job in platform_jobs)):
                return pipeline_id, commit, tree
    return None


def main():
    out = Path("tree-tested.env")
    # Always replace stale evidence, even when this invocation is ineligible.
    out.write_text("QORE_TREE_TESTED_CHECKED=1\n")
    env = os.environ
    if not eligible(env):
        print("full verification requested: running the full pipeline")
        return
    try:
        evidence = find_evidence(env, GitLabApi(env))
    except (EvidenceUnavailable, KeyError, ValueError, OSError,
            subprocess.SubprocessError):
        print("cannot establish successful CI for this tree: running the full pipeline")
        return
    if evidence is None:
        print("no successful test pipeline for this tree: running the full pipeline")
        return
    pipeline, commit, tree = evidence
    out.write_text("QORE_TREE_TESTED_CHECKED=1\n"
                   f"QORE_TREE_TESTED_IN={pipeline}\n"
                   f"QORE_TREE_TESTED_SHA={commit}\n"
                   f"QORE_TREE_TESTED_TREE={tree}\n")
    print(f"tree {tree} passed every test job in pipeline {pipeline} on {commit}")


if __name__ == "__main__":
    main()
