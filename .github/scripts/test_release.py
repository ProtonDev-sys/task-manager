"""Release tests use a recording fake gh; no GitHub requests or mutations occur."""

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import release


SHA = "a" * 40
OTHER_SHA = "b" * 40
REPOSITORY = "ProtonDev-sys/task-manager"
ENDPOINT = f"repos/{REPOSITORY}/releases"
TAG = "v1.2.1"


class FakeGh:
    def __init__(self, pages=None, references=None, annotations=None):
        self.pages = [[]] if pages is None else pages
        self.references = [] if references is None else references
        self.annotations = annotations or {}
        self.calls = []
        self.mutations = []
        self.upload_fails = False
        self.current = next((dict(item) for page in self.pages for item in page
                             if item.get("tag_name") == TAG), None)

    def __call__(self, command, **options):
        assert command[0] == "gh"
        assert options["capture_output"] and options["encoding"] == "utf-8"
        assert options["check"] is False
        args = command[1:]
        payload = json.loads(options["input"]) if options.get("input") else None
        self.calls.append((args, payload))
        data = None
        if args[0] == "api":
            method = args[args.index("--method") + 1] if "--method" in args else "GET"
            endpoint = args[1]
            if method == "POST":
                assert endpoint == ENDPOINT and payload["draft"] is True
                self.mutations.append("create-draft")
                self.current = {"id": 17, **payload}
                data = self.current
            elif method == "PATCH":
                assert endpoint == ENDPOINT + "/17" and payload == {"draft": False}
                self.mutations.append("publish")
                self.current["draft"] = False
                data = self.current
            elif endpoint == ENDPOINT + "?per_page=100":
                assert "--paginate" in args and "--slurp" in args
                data = self.pages
            elif endpoint == f"repos/{REPOSITORY}/git/matching-refs/tags/{TAG}":
                data = self.references
            elif endpoint.startswith(f"repos/{REPOSITORY}/git/tags/"):
                data = self.annotations[endpoint.rsplit("/", 1)[1]]
            elif endpoint == ENDPOINT + "/17":
                data = self.current
            else:
                raise AssertionError(f"Unexpected gh API call: {args}")
        else:
            assert args[:3] == ["release", "upload", TAG]
            assert args[args.index("--repo") + 1] == REPOSITORY
            self.mutations.append("upload")
            if self.upload_fails:
                return subprocess.CompletedProcess(command, 1, "", "never-print-this-token")
        return subprocess.CompletedProcess(command, 0, json.dumps(data), "")


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.executable = b"owned test executable fixture"
        (self.directory / "TaskManager.exe").write_bytes(self.executable)
        (self.directory / "RELEASE_NOTES.md").write_text("Release fixture notes.\n", encoding="utf-8")
        self.write_metadata()
        self.write_checksum()
        environment = {"GITHUB_SHA": SHA, "GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/main",
                       "GITHUB_REPOSITORY": REPOSITORY, "GH_TOKEN": "never-print-this-token"}
        self.environment = patch.dict(os.environ, environment)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def write_metadata(self, version="1.2.1", sha=SHA):
        (self.directory / "release.json").write_text(json.dumps({"version": version, "sha": sha}), encoding="utf-8")

    def write_checksum(self):
        checksum = hashlib.sha256(self.executable).hexdigest()
        (self.directory / "SHA256SUMS.txt").write_text(f"{checksum}  TaskManager.exe\n", encoding="utf-8")

    def run_release(self, fake=None, publish=False):
        fake = fake or FakeGh()
        output, errors = io.StringIO(), io.StringIO()
        arguments = ["--artifact-dir", str(self.directory)] + (["--publish"] if publish else [])
        with patch.object(release.subprocess, "run", side_effect=fake), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            status = release.main(arguments)
        return status, fake, output.getvalue(), errors.getvalue()

    def draft(self, sha=SHA):
        return {"id": 17, "tag_name": TAG, "draft": True, "target_commitish": sha}

    def reference(self, kind="commit", sha=SHA, tag=TAG):
        return {"ref": f"refs/tags/{tag}", "object": {"type": kind, "sha": sha}}

    def test_validation_never_invokes_gh(self):
        status, fake, output, _ = self.run_release()
        self.assertEqual(status, 0)
        self.assertEqual(fake.calls, [])
        self.assertIn("Validated release artifact v1.2.1", output)

    def test_invalid_metadata_fails_before_gh(self):
        for version, sha in [("v1.2.1", SHA), ("1.2", SHA), ("1.2.1-rc1", SHA),
                             (None, SHA), ("1.2.1", OTHER_SHA), ("1.2.1", "main")]:
            with self.subTest(version=version, sha=sha):
                self.write_metadata(version, sha)
                status, fake, _, _ = self.run_release(publish=True)
                self.assertEqual(status, 1)
                self.assertEqual(fake.calls, [])

    def test_checksum_mismatch_or_extra_entry_fails_before_gh(self):
        checksum = hashlib.sha256(self.executable).hexdigest()
        for contents in ["0" * 64 + "  TaskManager.exe\n",
                         f"{checksum}  TaskManager.exe\n{checksum}  second.exe\n",
                         f"{checksum}  wrong.exe\n"]:
            with self.subTest(contents=contents):
                (self.directory / "SHA256SUMS.txt").write_text(contents, encoding="utf-8")
                status, fake, _, _ = self.run_release(publish=True)
                self.assertEqual(status, 1)
                self.assertEqual(fake.calls, [])

    def test_empty_executable_and_notes_are_rejected(self):
        for filename in ["TaskManager.exe", "RELEASE_NOTES.md"]:
            with self.subTest(filename=filename):
                path = self.directory / filename
                saved = path.read_bytes()
                path.write_bytes(b"")
                self.assertEqual(self.run_release()[0], 1)
                path.write_bytes(saved)

    def test_publish_rejects_wrong_event_or_ref_before_gh(self):
        for event, ref in [("pull_request", "refs/heads/main"), ("push", "refs/heads/topic"),
                           ("workflow_dispatch", "refs/heads/main")]:
            with self.subTest(event=event, ref=ref), patch.dict(os.environ, GITHUB_EVENT_NAME=event, GITHUB_REF=ref):
                status, fake, _, _ = self.run_release(publish=True)
                self.assertEqual(status, 1)
                self.assertEqual(fake.calls, [])

    def test_published_version_on_later_page_is_untouched(self):
        published = {**self.draft(OTHER_SHA), "draft": False}
        fake = FakeGh(pages=[[{"tag_name": "v1.2.0", "draft": False}], [published]])
        status, fake, output, _ = self.run_release(fake, publish=True)
        self.assertEqual(status, 0)
        self.assertEqual(fake.mutations, [])
        self.assertEqual(len(fake.calls), 1)
        self.assertIn("already published", output)

    def test_conflicting_draft_is_rejected_without_mutation(self):
        for target in [OTHER_SHA, "main"]:
            with self.subTest(target=target):
                status, fake, _, _ = self.run_release(FakeGh(pages=[[self.draft(target)]]), publish=True)
                self.assertEqual(status, 1)
                self.assertEqual(fake.mutations, [])

    def test_conflicting_lightweight_or_annotated_tag_is_rejected(self):
        for annotated in [False, True]:
            with self.subTest(annotated=annotated):
                reference = self.reference("tag" if annotated else "commit", OTHER_SHA)
                fake = FakeGh(references=[reference], annotations={OTHER_SHA: {"object": {"type": "commit", "sha": OTHER_SHA}}})
                status, fake, _, _ = self.run_release(fake, publish=True)
                self.assertEqual(status, 1)
                self.assertEqual(fake.mutations, [])

    def test_new_release_creates_draft_uploads_only_two_assets_then_publishes(self):
        # Prefix matches are not the exact release tag.
        fake = FakeGh(references=[self.reference(sha=OTHER_SHA, tag="v1.2.10")])
        status, fake, _, errors = self.run_release(fake, publish=True)
        self.assertEqual(status, 0, errors)
        self.assertEqual(fake.mutations, ["create-draft", "upload", "publish"])
        create = next(payload for args, payload in fake.calls if "POST" in args)
        self.assertEqual(create["target_commitish"], SHA)
        self.assertTrue(create["draft"])
        upload = next(args for args, _ in fake.calls if args[:2] == ["release", "upload"])
        self.assertEqual(upload[3:5], [str(self.directory / "TaskManager.exe"), str(self.directory / "SHA256SUMS.txt")])
        self.assertNotIn("--clobber", upload)

    def test_existing_verified_draft_resumes_with_clobber(self):
        fake = FakeGh(pages=[[self.draft()]], references=[self.reference()])
        status, fake, _, errors = self.run_release(fake, publish=True)
        self.assertEqual(status, 0, errors)
        self.assertEqual(fake.mutations, ["upload", "publish"])
        upload = next(args for args, _ in fake.calls if args[:2] == ["release", "upload"])
        self.assertIn("--clobber", upload)

    def test_nested_annotated_tag_is_peeled_to_the_commit(self):
        outer, inner = "c" * 40, "d" * 40
        fake = FakeGh(references=[self.reference("tag", outer)], annotations={
            outer: {"object": {"type": "tag", "sha": inner}},
            inner: {"object": {"type": "commit", "sha": SHA}},
        })
        status, fake, _, errors = self.run_release(fake, publish=True)
        self.assertEqual(status, 0, errors)
        self.assertEqual(fake.mutations, ["create-draft", "upload", "publish"])

    def test_failed_upload_keeps_draft_and_does_not_print_cli_secrets(self):
        fake = FakeGh()
        fake.upload_fails = True
        status, fake, output, errors = self.run_release(fake, publish=True)
        self.assertEqual(status, 1)
        self.assertEqual(fake.mutations, ["create-draft", "upload"])
        self.assertTrue(fake.current["draft"])
        self.assertNotIn("never-print-this-token", output + errors)


if __name__ == "__main__":
    unittest.main()
