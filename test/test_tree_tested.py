#!/usr/bin/env python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Exercise CI tree reuse with real Git histories, a local API and a fake status sink."""

import contextlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from urllib.parse import parse_qs, urlsplit


ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / "test/docker_test/tree-already-tested.py"
SKIP = ROOT / "test/docker_test/tree-tested-skip.sh"
CI_CONFIG = ROOT / ".gitlab-ci.yml"


def load_check_module():
    spec = importlib.util.spec_from_file_location("tree_already_tested", CHECK)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


REQUIRED_JOBS = sorted(load_check_module().REQUIRED_JOBS)


def ci_test_jobs():
    """The jobs of the test stage of .gitlab-ci.yml (no YAML parser is needed in the images).

    A job is a top-level key that is not hidden (".name"); it is in the test stage when it
    says so, or when it extends .test-ubuntu, whose stage is test.
    """
    jobs = {}
    name = None
    for line in CI_CONFIG.read_text().splitlines():
        match = re.match(r"([A-Za-z0-9_.-]+):\s*$", line)
        if match:
            name = match.group(1)
            jobs[name] = []
        elif name and line.startswith(" "):
            jobs[name].append(line.strip())
    return {job for job, body in jobs.items() if not job.startswith(".")
            and ("stage: test" in body or "extends: .test-ubuntu" in body)}


@contextlib.contextmanager
def api_server(responder):
    requests = []
    errors = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append((self.path, dict(self.headers)))
            try:
                status, body = responder(self.path, self.headers)
            except Exception as error:
                errors.append(error)
                status, body = 500, {"message": "test responder failed"}
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    stop_read, stop_write = socket.socketpair()

    def serve():
        while True:
            readable = select.select([server, stop_read], [], [])[0]
            if stop_read in readable:
                return
            server.handle_request()

    thread = threading.Thread(target=serve)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}/api/v4", requests
    finally:
        stop_write.send(b"x")
        thread.join(timeout=5)
        server.server_close()
        stop_read.close()
        stop_write.close()
        if thread.is_alive():
            raise AssertionError("test API server did not exit")
        if errors:
            raise errors[0]


class TreeTestedTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="python-ci-test-")
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        self.repo = self.directory / "repo"
        self.repo.mkdir()
        self.git("init", "-q", "-b", "develop")
        self.git("config", "user.name", "CI Test")
        self.git("config", "user.email", "ci-test@example.invalid")
        (self.repo / "source").write_text("base\n")
        self.git("add", "source")
        self.git("-c", "commit.gpgsign=false", "commit", "-qm", "base")
        self.head = self.git("rev-parse", "HEAD")
        self.tree = self.git("rev-parse", "HEAD^{tree}")
        self.env = dict(os.environ)
        self.env.pop("BASH_ENV", None)
        for key in list(self.env):
            if key.startswith(("CI_", "QORE_TREE_TESTED_", "QORE_CI_")):
                del self.env[key]
        self.env.update(CI_COMMIT_BRANCH="develop", CI_PIPELINE_SOURCE="push",
                        CI_COMMIT_SHA=self.head, CI_PROJECT_ID="83", CI_PIPELINE_ID="200",
                        CI_JOB_TOKEN="job-test-secret")
        self.responses = {}
        self.job_responses = {}

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.repo, check=True,
                              capture_output=True, text=True).stdout.strip()

    def success(self, sha=None, pipeline_id=100):
        sha = sha or self.head
        self.responses[sha] = [dict(id=pipeline_id, project_id=83, sha=sha, status="success")]
        self.job_responses[pipeline_id] = [dict(name=name, status="success")
                                            for name in REQUIRED_JOBS]

    def respond(self, path, headers):
        parsed = urlsplit(path)
        if parsed.path.endswith("/jobs"):
            pipeline = int(parsed.path.split("/")[-2])
            return 200, self.job_responses.get(pipeline, [])
        self.assertEqual("/api/v4/projects/83/pipelines", parsed.path)
        query = parse_qs(parsed.query)
        self.assertEqual(["success"], query["status"])
        return 200, self.responses.get(query["sha"][0], [])

    def run_check(self, responder=None, **overrides):
        with api_server(responder or self.respond) as (url, requests):
            env = dict(self.env, CI_API_V4_URL=url, **overrides)
            result = subprocess.run([sys.executable, str(CHECK)], cwd=self.repo,
                                    env=env, capture_output=True, text=True, timeout=10)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual("", result.stderr)
        self.assertNotIn("test-secret", result.stdout)
        report = dict(line.split("=", 1) for line in (self.repo / "tree-tested.env").read_text().splitlines())
        self.assertEqual("1", report["QORE_TREE_TESTED_CHECKED"])
        return report, requests

    def assert_full(self, report):
        self.assertEqual({"QORE_TREE_TESTED_CHECKED": "1"}, report)

    def merge(self, changed=False):
        self.git("checkout", "-qb", "feature")
        (self.repo / "source").write_text("feature\n")
        self.git("-c", "commit.gpgsign=false", "commit", "-qam", "feature")
        parent = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "develop")
        if changed:
            (self.repo / "another-source").write_text("develop\n")
            self.git("add", "another-source")
            self.git("-c", "commit.gpgsign=false", "commit", "-qm", "develop")
        self.git("-c", "commit.gpgsign=false", "merge", "--no-ff", "--no-edit", "feature")
        self.head = self.git("rev-parse", "HEAD")
        self.tree = self.git("rev-parse", "HEAD^{tree}")
        self.env["CI_COMMIT_SHA"] = self.head
        return parent

    def test_same_commit_success(self):
        self.success()
        report, requests = self.run_check()
        self.assertEqual("100", report["QORE_TREE_TESTED_IN"])
        self.assertEqual(self.head, report["QORE_TREE_TESTED_SHA"])
        self.assertEqual(self.tree, report["QORE_TREE_TESTED_TREE"])
        self.assertEqual(2, len(requests))
        self.assertEqual(["false"], parse_qs(urlsplit(requests[1][0]).query)["include_retried"])

    def test_merge_reuses_identical_parent(self):
        parent = self.merge()
        self.success(parent)
        report, requests = self.run_check()
        self.assertEqual(parent, report["QORE_TREE_TESTED_SHA"])
        self.assertEqual(self.tree, report["QORE_TREE_TESTED_TREE"])
        self.assertEqual(3, len(requests))

    def test_changed_merge_tree_runs_full(self):
        parent = self.merge(changed=True)
        self.success(parent)
        report, requests = self.run_check()
        self.assert_full(report)
        self.assertEqual(1, len(requests))

    def test_no_prior_success(self):
        report, _ = self.run_check()
        self.assert_full(report)

    def test_current_pipeline_is_excluded(self):
        self.success(pipeline_id=200)
        report, requests = self.run_check()
        self.assert_full(report)
        self.assertEqual(1, len(requests))

    def test_older_success_after_current_pipeline(self):
        self.success()
        self.responses[self.head].insert(0, dict(id=200, project_id=83, sha=self.head, status="success"))
        report, _ = self.run_check()
        self.assertEqual("100", report["QORE_TREE_TESTED_IN"])

    def test_ineligible_pipelines_and_force_override(self):
        self.success()
        for overrides in ({"CI_COMMIT_BRANCH": "feature"}, {"CI_COMMIT_BRANCH": ""},
                          {"CI_PIPELINE_SOURCE": "web"}, {"CI_PIPELINE_SOURCE": "schedule"},
                          {"CI_PIPELINE_SOURCE": "merge_request_event"}, {"QORE_CI_FORCE_FULL": "1"}):
            with self.subTest(overrides=overrides):
                report, requests = self.run_check(**overrides)
                self.assert_full(report)
                self.assertEqual([], requests)

    def test_checkout_mismatch(self):
        self.success()
        report, requests = self.run_check(CI_COMMIT_SHA="a" * 40)
        self.assert_full(report)
        self.assertEqual([], requests)

    def test_invalid_pipeline_identity(self):
        for overrides in ({"CI_PROJECT_ID": "bad"}, {"CI_PROJECT_ID": "0"},
                          {"CI_PIPELINE_ID": "-1"}, {"CI_PIPELINE_ID": ""}):
            with self.subTest(overrides=overrides):
                report, requests = self.run_check(**overrides)
                self.assert_full(report)
                self.assertEqual([], requests)

    def test_missing_credentials(self):
        self.success()
        report, requests = self.run_check(CI_JOB_TOKEN="")
        self.assert_full(report)
        self.assertEqual([], requests)

    def test_read_api_token_fallback(self):
        self.success()
        def respond(path, headers):
            if headers.get("JOB-TOKEN"):
                return 403, {"message": "denied"}
            self.assertEqual("read-test-secret", headers.get("PRIVATE-TOKEN"))
            return self.respond(path, headers)
        report, requests = self.run_check(respond, QORE_CI_READ_API_TOKEN="read-test-secret")
        self.assertEqual("100", report["QORE_TREE_TESTED_IN"])
        self.assertEqual(4, len(requests))

    def test_read_api_token_without_job_token(self):
        self.success()
        report, requests = self.run_check(CI_JOB_TOKEN="", QORE_CI_READ_API_TOKEN="read-test-secret")
        self.assertEqual("100", report["QORE_TREE_TESTED_IN"])
        self.assertTrue(all(headers.get("Private-Token") == "read-test-secret" for _, headers in requests))

    def test_api_errors_and_malformed_json_run_full(self):
        for response in ((403, {"message": "denied"}), (500, b"internal error"),
                         (200, b"truncated ["), (200, {"id": 100}), (200, None)):
            with self.subTest(response=response):
                report, _ = self.run_check(lambda path, headers: response)
                self.assert_full(report)

    def test_untrusted_pipeline_items_are_ignored(self):
        self.success()
        original = self.responses[self.head][0]
        for patch in ({"id": True}, {"id": "100"}, {"id": 0}, {"id": -1},
                      {"project_id": 84}, {"sha": "a" * 40}, {"status": "failed"},
                      {"status": "running"}, {"status": "canceled"}):
            with self.subTest(patch=patch):
                self.responses[self.head] = [dict(original, **patch), None, "bad"]
                report, requests = self.run_check()
                self.assert_full(report)
                self.assertEqual(1, len(requests))

    def test_required_jobs_match_ci_config(self):
        self.assertEqual(set(REQUIRED_JOBS), ci_test_jobs())
        self.assertIn("test-ubuntu", REQUIRED_JOBS)
        self.assertIn("test-alpine", REQUIRED_JOBS)

    def test_other_jobs_do_not_prevent_reuse(self):
        self.success()
        self.job_responses[100] = ([{"name": "tree-already-tested", "status": "success"},
                                    {"name": "github-status-pending", "status": "failed"}]
                                   + self.job_responses[100]
                                   + [{"name": "github-status-success", "status": "success"}])
        report, _ = self.run_check()
        self.assertEqual("100", report["QORE_TREE_TESTED_IN"])

    def test_incomplete_or_nonpassing_platform_jobs(self):
        self.success()
        passed = [{"name": name, "status": "success"} for name in REQUIRED_JOBS]

        def with_status(name, status):
            self.assertIn(name, REQUIRED_JOBS)
            return [dict(job, status=status) if job["name"] == name else job for job in passed]

        for jobs in ([], [{"name": "test-ubuntu", "status": "success"}],
                     [{"name": ["test-ubuntu"], "status": "success"}],
                     [{"name": "lint", "status": "success"}],
                     [{"name": "test-ubuntu", "status": "success"}] * len(REQUIRED_JOBS),
                     passed[1:], passed[:-1],
                     passed + [{"name": REQUIRED_JOBS[0], "status": "success"}],
                     with_status("test-alpine", "skipped"),
                     with_status("test-ubuntu-python3.16", "failed"),
                     with_status("test-ubuntu", "manual"),
                     {"name": "test-ubuntu", "status": "success"}):
            with self.subTest(jobs=jobs):
                self.job_responses[100] = jobs
                report, _ = self.run_check()
                self.assert_full(report)

    def test_job_api_failure_runs_full(self):
        self.success()
        def respond(path, headers):
            return (500, {}) if urlsplit(path).path.endswith("/jobs") else self.respond(path, headers)
        report, _ = self.run_check(respond)
        self.assert_full(report)

    def test_stale_artifact_is_replaced(self):
        (self.repo / "tree-tested.env").write_text("QORE_TREE_TESTED_IN=100\n")
        report, _ = self.run_check()
        self.assert_full(report)

    def test_shallow_checkout_does_not_guess_missing_parents(self):
        parent = self.merge()
        self.success(parent)
        clone = self.directory / "shallow"
        subprocess.run(["git", "clone", "-q", "--depth=1", self.repo.as_uri(), str(clone)], check=True)
        self.repo = clone
        self.assertEqual("true", self.git("rev-parse", "--is-shallow-repository"))
        report, requests = self.run_check()
        self.assert_full(report)
        self.assertEqual(1, len(requests))

    def skip_env(self):
        fake_bin = self.directory / "bin"
        fake_bin.mkdir(exist_ok=True)
        curl = fake_bin / "curl"
        curl.write_text(f"#!{sys.executable}\n"
                        "import json, os, pathlib, sys\n"
                        "pathlib.Path(os.environ['STATUS_CAPTURE']).write_text(json.dumps(sys.argv[1:]))\n"
                        "sys.exit(int(os.environ.get('STATUS_EXIT', '0')))\n")
        curl.chmod(0o755)
        return dict(self.env, PATH=str(fake_bin) + os.pathsep + self.env["PATH"],
                    QORE_TREE_TESTED_CHECKED="1", QORE_TREE_TESTED_IN="100",
                    QORE_TREE_TESTED_SHA=self.head, QORE_TREE_TESTED_TREE=self.tree,
                    GITHUB_ACCESS_TOKEN="github-test-secret", REPO_NAME="module-python",
                    CI_JOB_NAME="test-ubuntu-python3.15t",
                    CI_PROJECT_URL="https://git.example.invalid/mirror/module-python",
                    STATUS_CAPTURE=str(self.directory / "status.json"))

    def run_skip(self, env, shell="sh"):
        capture = self.directory / "status.json"
        if capture.exists():
            capture.unlink()
        # The sentinel is reached only if sourcing the helper did not exit.
        result = subprocess.run([shell, "-euxc", '. "$1"; echo FULL_JOB', "ci-job", str(SKIP)],
                                cwd=self.repo, env=env, text=True, capture_output=True, timeout=10)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertNotIn("github-test-secret", result.stdout + result.stderr)
        args = json.loads(capture.read_text()) if capture.exists() else None
        return result.stdout, args

    def test_skip_publishes_success_and_exits_before_work(self):
        for shell in ("sh", "bash"):
            with self.subTest(shell=shell):
                output, args = self.run_skip(self.skip_env(), shell)
                self.assertNotIn("FULL_JOB", output)
                self.assertIn(f"https://api.github.com/repos/qoretechnologies/module-python/statuses/{self.head}", args)
                body = json.loads(args[args.index("-d") + 1])
                self.assertEqual("success", body["state"])
                # the job's own context: the pipeline context is left to the github-status-* jobs
                self.assertEqual("module-python/test-ubuntu-python3.15t", body["context"])
                self.assertEqual("https://git.example.invalid/mirror/module-python/-/pipelines/100", body["target_url"])
                self.assertEqual("tree tested in pipeline 100", body["description"])

    def test_skip_requires_valid_matching_evidence(self):
        for patch in ({"QORE_TREE_TESTED_CHECKED": ""}, {"QORE_TREE_TESTED_IN": ""},
                      {"QORE_TREE_TESTED_IN": "0"}, {"QORE_TREE_TESTED_IN": "100\ninjected"},
                      {"QORE_TREE_TESTED_IN": "200"}, {"QORE_TREE_TESTED_SHA": "abc"},
                      {"QORE_TREE_TESTED_SHA": "g" * 40}, {"QORE_TREE_TESTED_TREE": ""},
                      {"QORE_TREE_TESTED_TREE": "a" * 40}, {"GITHUB_ACCESS_TOKEN": ""},
                      {"CI_JOB_NAME": ""}, {"REPO_NAME": ""}, {"CI_PROJECT_URL": ""},
                      {"QORE_TREE_TESTED_SHA": "a" * 39}, {"QORE_TREE_TESTED_IN": "-100"},
                      {"CI_COMMIT_BRANCH": "feature"}, {"CI_PIPELINE_SOURCE": "web"},
                      {"CI_PIPELINE_SOURCE": "schedule"}, {"QORE_CI_FORCE_FULL": "1"}):
            with self.subTest(patch=patch):
                output, args = self.run_skip(dict(self.skip_env(), **patch))
                self.assertIn("FULL_JOB", output)
                self.assertIsNone(args)

    def test_skip_without_checkout_runs_full(self):
        env = self.skip_env()
        self.repo = self.directory
        output, args = self.run_skip(env)
        self.assertIn("FULL_JOB", output)
        self.assertIsNone(args)

    def test_status_failure_runs_full(self):
        output, args = self.run_skip(dict(self.skip_env(), STATUS_EXIT="22"))
        self.assertIn("FULL_JOB", output)
        self.assertIsNotNone(args)


if __name__ == "__main__":
    unittest.main()
