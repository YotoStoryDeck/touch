#!/usr/bin/env python3
"""
calculate_version.py
Automated Semantic Versioning and Tag Calculator for GitHub Actions.
Supports:
  - Base version discovery via git tags (highest semver)
  - Semantic bumping: current, patch, minor, major, custom
  - Pre-release (RC) formatting: v<MAJOR>.<MINOR>.<PATCH>-rc.<YYYYMMDD>.<ITERATION>
  - Daily iteration auto-incrementing
  - Stable release formatting: v<MAJOR>.<MINOR>.<PATCH>
  - Writing outputs to $GITHUB_OUTPUT
"""

import argparse
import os
import re
import subprocess
import sys
from datetime import datetime, timezone


def get_git_tags(repo_dir: str) -> list[str]:
    """Fetch and return all git tags from repository."""
    try:
        res = subprocess.run(
            ["git", "-C", repo_dir, "tag", "-l", "v*"],
            capture_output=True,
            text=True,
            check=True,
        )
        return [line.strip() for line in res.stdout.splitlines() if line.strip()]
    except Exception as e:
        print(f"Warning: Failed to query git tags: {e}", file=sys.stderr)
        return []


def discover_highest_base_version(tags: list[str]) -> tuple[int, int, int]:
    """
    Parses all tags matching v<M>.<m>.<p> (ignoring suffixes like -rc...)
    and returns the highest (M, m, p) tuple. Defaults to (0, 1, 0) if no tags found.
    """
    semver_pattern = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)")
    versions: list[tuple[int, int, int]] = []

    for tag in tags:
        m = semver_pattern.match(tag)
        if m:
            major, minor, patch = int(m.group(1)), int(m.group(2)), int(m.group(3))
            versions.append((major, minor, patch))

    if not versions:
        return (0, 1, 0)

    versions.sort()
    return versions[-1]


def apply_bump(base: tuple[int, int, int], bump: str) -> tuple[int, int, int]:
    """Applies semantic bump operation to base version tuple."""
    major, minor, patch = base
    bump_lower = bump.strip().lower()
    if bump_lower == "current":
        return (major, minor, patch)
    elif bump_lower == "patch":
        return (major, minor, patch + 1)
    elif bump_lower == "minor":
        return (major, minor + 1, 0)
    elif bump_lower == "major":
        return (major + 1, 0, 0)
    else:
        raise ValueError(f"Unknown bump action: {bump}. Expected: current, patch, minor, major, custom.")


def calculate_next_rc_iteration(tags: list[str], prefix: str) -> int:
    """
    Given tags and a prefix like 'v1.1.0-rc.20261106.', finds the highest existing
    iteration number and returns next = highest + 1.
    """
    highest = 0
    pattern = re.compile(r"^" + re.escape(prefix) + r"(\d+)$")
    for tag in tags:
        m = pattern.match(tag)
        if m:
            iteration = int(m.group(1))
            if iteration > highest:
                highest = iteration
    return highest + 1


def main() -> None:
    parser = argparse.ArgumentParser(description="Calculate release version tag and pre-release status.")
    parser.add_argument("--repo-dir", default=".", help="Path to repository root.")
    parser.add_argument("--event", default="workflow_dispatch", help="GitHub event name.")
    parser.add_argument("--ref-name", default="", help="GITHUB_REF_NAME (for tag push events).")
    parser.add_argument("--bump", default="current", help="Bump action: current, patch, minor, major, custom.")
    parser.add_argument("--release-type", default="pre-release (RC)", help="Release type: pre-release or stable.")
    parser.add_argument("--custom-tag", default="", help="Custom tag to use if bump is custom.")
    parser.add_argument("--output-file", default=os.environ.get("GITHUB_OUTPUT", ""), help="Path to GITHUB_OUTPUT.")

    args = parser.parse_args()

    tags = get_git_tags(args.repo_dir)

    is_prerelease = False
    final_tag = ""

    if args.event == "push" and args.ref_name:
        # Pushed directly via git tag
        raw_tag = args.ref_name.strip()
        clean = re.sub(r"^[vV]+", "", raw_tag)
        final_tag = f"v{clean}"
        is_prerelease = ("-" in final_tag)
    else:
        bump_action = args.bump.strip().lower()
        rel_type = args.release_type.strip().lower()
        wants_prerelease = "pre" in rel_type or "rc" in rel_type

        if bump_action == "custom":
            if not args.custom_tag.strip():
                raise ValueError("bump is set to 'custom' but --custom-tag was not provided.")
            clean = re.sub(r"^[vV]+", "", args.custom_tag.strip())
            final_tag = f"v{clean}"
            is_prerelease = wants_prerelease or ("-" in final_tag)
        else:
            base_version = discover_highest_base_version(tags)
            new_base = apply_bump(base_version, bump_action)
            major, minor, patch = new_base

            if wants_prerelease:
                today_utc = datetime.now(timezone.utc).strftime("%Y%m%d")
                rc_prefix = f"v{major}.{minor}.{patch}-rc.{today_utc}."
                next_iter = calculate_next_rc_iteration(tags, rc_prefix)
                final_tag = f"{rc_prefix}{next_iter}"
                is_prerelease = True
            else:
                final_tag = f"v{major}.{minor}.{patch}"
                is_prerelease = False

    print(f"Calculated Version Tag: {final_tag}")
    print(f"Is Pre-Release: {is_prerelease}")

    # Output to GITHUB_OUTPUT if specified
    if args.output_file:
        try:
            with open(args.output_file, "a", encoding="utf-8") as f:
                f.write(f"tag={final_tag}\n")
                f.write(f"prerelease={'true' if is_prerelease else 'false'}\n")
            print(f"Appended tag & prerelease outputs to {args.output_file}")
        except Exception as e:
            print(f"Failed to write to output file {args.output_file}: {e}", file=sys.stderr)


if __name__ == "__main__":
    main()
