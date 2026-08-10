#!/usr/bin/env python3
"""Update the plugin version in tooth.json.

Usage: python scripts/bump_version.py [--dry-run] <version>
"""

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
TOOTH_PATH = ROOT / "tooth.json"
SEMVER_TOKEN = (
    r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)"
    r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
)
SEMVER = re.compile(rf"^{SEMVER_TOKEN}$")
VERSION_FIELD = re.compile(
    rf'(?m)^(\s*"version"\s*:\s*")(?P<version>{SEMVER_TOKEN})("\s*,?\s*)$'
)


class Style:
    BOLD = "\033[1m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    RED = "\033[91m"
    CYAN = "\033[96m"
    RESET = "\033[0m"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Update the hud-tryer plugin version.")
    parser.add_argument("new_version", help="new semantic version, for example 0.3.0-alpha.3")
    parser.add_argument(
        "-d",
        "--dryrun",
        "--dry-run",
        dest="dry_run",
        action="store_true",
        help="show the change without writing tooth.json",
    )
    return parser.parse_args()


def read_text(path: Path) -> str:
    with path.open("r", encoding="utf-8", newline="") as file:
        return file.read()


def write_text(path: Path, content: str) -> None:
    with path.open("w", encoding="utf-8", newline="") as file:
        file.write(content)


def replace_version(content: str, new_version: str) -> tuple[str, str]:
    try:
        metadata = json.loads(content)
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid JSON: {error}") from error

    if not isinstance(metadata, dict) or not isinstance(metadata.get("version"), str):
        raise ValueError("top-level version must be a string")
    previous_version = metadata["version"]
    if not SEMVER.fullmatch(previous_version):
        raise ValueError(f"top-level version is not valid semantic versioning: {previous_version}")

    matches = list(VERSION_FIELD.finditer(content))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one top-level version field, found {len(matches)}")
    if matches[0].group("version") != previous_version:
        raise ValueError("formatted version field does not match the parsed top-level version")

    def replace(match: re.Match[str]) -> str:
        return f"{match.group(1)}{new_version}{match.group(3)}"

    return VERSION_FIELD.sub(replace, content), previous_version


def main() -> int:
    args = parse_args()
    new_version = args.new_version.strip()
    if not SEMVER.fullmatch(new_version):
        print(f"{Style.RED}error:{Style.RESET} invalid semantic version: {new_version}", file=sys.stderr)
        return 2
    if not TOOTH_PATH.is_file():
        print(f"{Style.RED}error:{Style.RESET} missing {TOOTH_PATH}", file=sys.stderr)
        return 2

    try:
        content, previous_version = replace_version(read_text(TOOTH_PATH), new_version)
    except ValueError as error:
        print(f"{Style.RED}error:{Style.RESET} tooth.json: {error}", file=sys.stderr)
        return 2

    print(f"\n{Style.BOLD}Update hud-tryer version -> {new_version}{Style.RESET}")
    if args.dry_run:
        print(f"{Style.CYAN}Dry run: no files will be written.{Style.RESET}")

    if previous_version == new_version:
        print(f"{Style.YELLOW}Already at {new_version}; nothing to change.{Style.RESET}")
        return 0

    action = "would update" if args.dry_run else "update"
    print(f"  {Style.GREEN}{action}{Style.RESET} tooth.json ({previous_version} -> {new_version})")
    if args.dry_run:
        return 0

    write_text(TOOTH_PATH, content)
    print(f"\n{Style.GREEN}{Style.BOLD}Updated tooth.json.{Style.RESET}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
