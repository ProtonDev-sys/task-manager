"""Validate the verified build artifact and optionally publish its GitHub release."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate_artifact(directory):
    metadata = json.loads((directory / "release.json").read_text(encoding="utf-8-sig"))
    require(isinstance(metadata, dict), "release.json must contain an object.")
    version, sha = metadata.get("version"), metadata.get("sha")
    require(isinstance(version, str) and re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version),
            "Release version must be numeric x.y.z.")
    require(isinstance(sha, str) and re.fullmatch(r"[0-9a-fA-F]{40}", sha)
            and sha == os.environ.get("GITHUB_SHA"),
            "Artifact commit must equal the full GITHUB_SHA.")
    executable = directory / "TaskManager.exe"
    require(executable.is_file() and executable.stat().st_size > 0,
            "TaskManager.exe must be a nonempty file.")
    require((directory / "RELEASE_NOTES.md").read_text(encoding="utf-8-sig").strip(),
            "RELEASE_NOTES.md must contain release notes.")
    checksum = (directory / "SHA256SUMS.txt").read_text(encoding="utf-8-sig")
    match = re.fullmatch(r"([0-9a-fA-F]{64}) [ *]TaskManager\.exe\n?", checksum)
    require(match is not None, "SHA256SUMS.txt must contain exactly one TaskManager.exe entry.")
    digest = hashlib.sha256()
    with executable.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    require(match.group(1).lower() == digest.hexdigest(), "Executable checksum mismatch.")
    return version, sha


def gh(*arguments, payload=None, read_json=False):
    result = subprocess.run(
        ["gh", *arguments], input=None if payload is None else json.dumps(payload),
        capture_output=True, text=True, encoding="utf-8", check=False,
    )
    if result.returncode:
        # Never relay CLI output: authentication failures may contain credentials.
        raise RuntimeError(f"GitHub CLI {arguments[0]} failed (exit {result.returncode}).")
    if read_json:
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError("GitHub CLI returned invalid JSON.") from error
    return None


def verify_tag(repository, tag, sha):
    # Matching refs returns an empty list for an absent tag, without masking errors.
    references = gh("api", f"repos/{repository}/git/matching-refs/tags/{tag}", read_json=True)
    require(isinstance(references, list) and all(isinstance(item, dict) for item in references),
            "Unexpected Git tag response.")
    exact = [item for item in references if item.get("ref") == f"refs/tags/{tag}"]
    require(len(exact) <= 1, "Multiple references match the release tag.")
    if not exact:
        return
    target = exact[0].get("object")
    seen = set()
    while isinstance(target, dict) and target.get("type") == "tag":
        object_sha = target.get("sha")
        require(isinstance(object_sha, str) and re.fullmatch(r"[0-9a-fA-F]{40}", object_sha)
                and object_sha not in seen and len(seen) < 32,
                "Invalid or cyclic annotated release tag.")
        seen.add(object_sha)
        annotation = gh("api", f"repos/{repository}/git/tags/{object_sha}", read_json=True)
        require(isinstance(annotation, dict), "Unexpected annotated tag response.")
        target = annotation.get("object")
    require(isinstance(target, dict) and target.get("type") == "commit" and target.get("sha") == sha,
            "Existing release tag points to a different commit.")


def verify_draft(release, tag, sha):
    require(isinstance(release, dict) and release.get("draft") is True
            and release.get("tag_name") == tag and release.get("target_commitish") == sha
            and type(release.get("id")) is int and release["id"] > 0,
            "Existing draft does not match this exact release commit.")
    return release["id"]


def publish(directory, version, sha):
    require(os.environ.get("GITHUB_EVENT_NAME") == "push"
            and os.environ.get("GITHUB_REF") == "refs/heads/main",
            "Publishing requires a push to refs/heads/main.")
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository),
            "GITHUB_REPOSITORY must identify the release repository.")
    tag = f"v{version}"
    endpoint = f"repos/{repository}/releases"
    pages = gh("api", endpoint + "?per_page=100", "--paginate", "--slurp", read_json=True)
    require(isinstance(pages, list) and all(isinstance(page, list) for page in pages)
            and all(isinstance(item, dict) for page in pages for item in page),
            "Unexpected paginated release response.")
    matches = [item for page in pages for item in page if item.get("tag_name") == tag]
    if any(item.get("draft") is False for item in matches):
        print(f"{tag} is already published; no changes made.")
        return
    require(len(matches) <= 1, "Multiple drafts match the release version.")
    existing = bool(matches)
    release_id = verify_draft(matches[0], tag, sha) if existing else None
    verify_tag(repository, tag, sha)
    if not existing:
        release = gh("api", endpoint, "--method", "POST", "--input", "-", read_json=True,
                     payload={"tag_name": tag, "target_commitish": sha, "draft": True,
                              "name": f"Task Manager {tag}",
                              "body": (directory / "RELEASE_NOTES.md").read_text(encoding="utf-8-sig")})
        release_id = verify_draft(release, tag, sha)
    release_endpoint = f"{endpoint}/{release_id}"
    # Recheck the exact draft before a retry is allowed to replace its assets.
    current = gh("api", release_endpoint, read_json=True)
    require(verify_draft(current, tag, sha) == release_id, "Release identity changed.")
    arguments = ["release", "upload", tag, str(directory / "TaskManager.exe"),
                 str(directory / "SHA256SUMS.txt"), "--repo", repository]
    if existing:
        arguments.append("--clobber")
    gh(*arguments)
    current = gh("api", release_endpoint, read_json=True)
    require(verify_draft(current, tag, sha) == release_id, "Release identity changed during upload.")
    verify_tag(repository, tag, sha)
    published = gh("api", release_endpoint, "--method", "PATCH", "--input", "-",
                   payload={"draft": False}, read_json=True)
    require(isinstance(published, dict) and published.get("id") == release_id
            and published.get("draft") is False and published.get("tag_name") == tag,
            "GitHub did not confirm release publication.")
    print(f"Published {tag} from {sha} with TaskManager.exe and SHA256SUMS.txt.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--publish", action="store_true")
    arguments = parser.parse_args(argv)
    try:
        directory = arguments.artifact_dir.resolve()
        version, sha = validate_artifact(directory)
        if arguments.publish:
            publish(directory, version, sha)
        else:
            print(f"Validated release artifact v{version} for {sha}.")
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Release failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
