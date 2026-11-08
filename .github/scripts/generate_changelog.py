#!/usr/bin/env python3
"""
generate_changelog.py
Automated changelog and release notes generator for GitHub Releases.
- Pre-releases: Prepends warning callout and generates commit changelog with GitHub logins (@<user>).
- Full releases: Injects versioned release notes from releases/release-v[version].md,
  or falls back to the commit changelog if no curated notes file is found.
- Contributor resolution: Queries GitHub API for author logins, falling back to git commit metadata.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.request

KNOWN_USERS = {
    "alicia86@gmail.com": "alicia86",
    "alicia86": "alicia86",
    "alicia g": "alicia86",
    "alicia": "alicia86",
}


def get_git_tags(repo_dir: str) -> list[str]:
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


def resolve_previous_tag(tags: list[str], current_tag: str, is_prerelease: bool) -> str:
    clean_current = current_tag.strip()
    semver_pattern = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)(?:-rc\.(\d+)(?:\.(\d+))?)?")

    def tag_key(t: str):
        m = semver_pattern.match(t)
        if not m:
            return (-1, -1, -1, -1, -1, -1)
        major, minor, patch = int(m.group(1)), int(m.group(2)), int(m.group(3))
        is_stable = 1 if ("-" not in t) else 0
        rc_date = int(m.group(4)) if m.group(4) else 99999999
        rc_iter = int(m.group(5)) if m.group(5) else 9999
        return (major, minor, patch, is_stable, rc_date, rc_iter)

    sorted_tags = sorted(tags, key=tag_key, reverse=True)

    for t in sorted_tags:
        if t == clean_current:
            continue
        if is_prerelease:
            return t
        else:
            if "-" not in t:
                return t
    return ""


def get_commits_from_github(repo: str, base: str, head: str, token: str) -> list[dict]:
    url = f"https://api.github.com/repos/{repo}/compare/{base}...{head}"
    headers = {"User-Agent": "StoryDeck-Changelog-Generator"}
    if token:
        headers["Authorization"] = f"Bearer {token}"

    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode())
            return data.get("commits", [])
    except Exception as e:
        print(f"GitHub Compare API failed ({url}): {e}", file=sys.stderr)
        return []


def resolve_author_login(commit_dict: dict) -> str:
    # 1. GitHub API author login
    author = commit_dict.get("author") or {}
    if author.get("login"):
        return author["login"]

    # 2. GitHub API committer login
    committer = commit_dict.get("committer") or {}
    if committer.get("login"):
        return committer["login"]

    # 3. Fallback to commit metadata & known user map
    commit_meta = commit_dict.get("commit", {})
    author_meta = commit_meta.get("author", {})
    name = author_meta.get("name", "").strip()
    email = author_meta.get("email", "").strip().lower()

    if email in KNOWN_USERS:
        return KNOWN_USERS[email]
    if name.lower() in KNOWN_USERS:
        return KNOWN_USERS[name.lower()]

    # GitHub noreply format: 12345+username@users.noreply.github.com
    if "@users.noreply.github.com" in email:
        user_part = email.split("@")[0]
        if "+" in user_part:
            return user_part.split("+")[1]
        return user_part

    if email:
        user_part = email.split("@")[0]
        if user_part in KNOWN_USERS:
            return KNOWN_USERS[user_part]
        return user_part

    return name or "contributor"


def get_commits_from_git_log(repo_dir: str, prev_tag: str) -> list[dict]:
    range_arg = f"{prev_tag}..HEAD" if prev_tag else "HEAD"
    cmd = ["git", "-C", repo_dir, "log", range_arg, "--pretty=format:%H%x00%an%x00%ae%x00%s"]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        entries = []
        for line in res.stdout.splitlines():
            parts = line.split("\x00")
            if len(parts) >= 4:
                sha, name, email, subject = parts[0], parts[1], parts[2], parts[3]
                entries.append({
                    "sha": sha,
                    "commit": {
                        "message": subject,
                        "author": {"name": name, "email": email}
                    }
                })
        return entries
    except Exception as e:
        print(f"Git log failed: {e}", file=sys.stderr)
        return []


def find_versioned_release_file(repo_dir: str, tag: str) -> str:
    clean_ver = re.sub(r"^[vV]+", "", tag.strip())
    candidates = [
        os.path.join(repo_dir, "releases", f"release-{tag}.md"),
        os.path.join(repo_dir, "releases", f"release-v{clean_ver}.md"),
        os.path.join(repo_dir, "release", f"release-{tag}.md"),
        os.path.join(repo_dir, "release", f"release-v{clean_ver}.md"),
        os.path.join(repo_dir, f"release-{tag}.md"),
        os.path.join(repo_dir, f"release-v{clean_ver}.md"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    return ""


def main():
    parser = argparse.ArgumentParser(description="Generate release notes and changelog.")
    parser.add_argument("--repo-dir", default=".", help="Repository root directory")
    parser.add_argument("--repo", default="", help="GitHub repository name (owner/repo)")
    parser.add_argument("--tag", required=True, help="Release tag being created")
    parser.add_argument("--prerelease", default="false", help="Whether this is a pre-release")
    parser.add_argument("--output-file", default="release_notes.md", help="Path to write release notes")
    parser.add_argument("--token", default=os.environ.get("GITHUB_TOKEN", ""), help="GitHub Token")

    args = parser.parse_args()

    is_prerelease = args.prerelease.strip().lower() in ("true", "1", "yes")
    tag = args.tag.strip()
    repo_dir = os.path.abspath(args.repo_dir)

    tags = get_git_tags(repo_dir)
    prev_tag = resolve_previous_tag(tags, tag, is_prerelease)
    print(f"Changelog Resolution: current={tag}, prev={prev_tag or 'none'}, prerelease={is_prerelease}")

    content_parts = []

    if is_prerelease:
        # Pre-release warning banner
        content_parts.append(
            "> [!WARNING]\n"
            "> This release is intended for testing purposes only. Please do not use it with production data, as frequent updates and architectural changes may occur between pre-release versions.\n"
        )

    # Check for curated release notes file (for full releases only)
    curated_file = "" if is_prerelease else find_versioned_release_file(repo_dir, tag)

    if curated_file:
        print(f"Found curated release notes: {curated_file}")
        with open(curated_file, "r", encoding="utf-8") as f:
            content_parts.append(f.read().strip())
    else:
        # Generate commit changelog
        print("Generating commit changelog...")
        commits = []
        if args.repo and prev_tag:
            commits = get_commits_from_github(args.repo, prev_tag, tag, args.token)

        if not commits:
            print("Fetching commits via local git log fallback...")
            commits = get_commits_from_git_log(repo_dir, prev_tag)

        content_parts.append("## Changelog")
        if commits:
            for c in commits:
                sha = c.get("sha", "")
                msg = c.get("commit", {}).get("message", "").splitlines()[0].strip()
                login = resolve_author_login(c)
                content_parts.append(f"* {sha}: {msg} (@{login})")
        else:
            if prev_tag:
                content_parts.append("* Maintenance release / no commit delta")
            else:
                content_parts.append("* Initial release")

        if args.repo and prev_tag:
            content_parts.append(f"\n**Full Changelog**: https://github.com/{args.repo}/compare/{prev_tag}...{tag}")

    final_body = "\n\n".join(part.strip() for part in content_parts if part.strip()) + "\n"

    output_path = os.path.abspath(args.output_file)
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(final_body)

    print(f"Successfully generated release notes at: {output_path}")


if __name__ == "__main__":
    main()
