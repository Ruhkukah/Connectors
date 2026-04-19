#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable
from urllib.error import HTTPError, URLError

import yaml


PROJECT_ROOT = Path(__file__).resolve().parents[1]
USER_AGENT = "MoexConnectorPhase0/0.1"
SENSITIVE_KEYS = {
    "credentials",
    "password",
    "refresh_token",
    "session_secret",
    "private_key",
    "authorization",
    "auth_header",
    "token",
}


def ensure_dir(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    return path


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def load_json_yaml(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    if path.suffix.lower() == ".json":
        return json.loads(text)
    return yaml.safe_load(text)


def dump_json(data: object, path: Path) -> None:
    ensure_dir(path.parent)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def dump_yaml(data: object, path: Path) -> None:
    ensure_dir(path.parent)
    path.write_text(
        yaml.safe_dump(data, sort_keys=False, allow_unicode=True),
        encoding="utf-8",
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stable_id(*parts: str) -> str:
    raw = "__".join(part for part in parts if part)
    lowered = raw.lower()
    return re.sub(r"[^a-z0-9]+", "_", lowered).strip("_")


def path_matches_any_glob(path: str, patterns: list[str]) -> bool:
    if not patterns:
        return True
    import fnmatch

    return any(fnmatch.fnmatch(path, pattern) for pattern in patterns)


def redact_secrets(value: object) -> object:
    if isinstance(value, dict):
        redacted = {}
        for key, item in value.items():
            if key.lower() in SENSITIVE_KEYS:
                redacted[key] = "[REDACTED]"
            else:
                redacted[key] = redact_secrets(item)
        return redacted
    if isinstance(value, list):
        return [redact_secrets(item) for item in value]
    return value


def fetch_bytes(url: str) -> bytes:
    parsed = urllib.parse.urlsplit(url)
    normalized_path = urllib.parse.quote(urllib.parse.unquote(parsed.path), safe="/%")
    normalized_url = urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, normalized_path, parsed.query, parsed.fragment))
    last_error: Exception | None = None
    for attempt in range(4):
        request = urllib.request.Request(normalized_url, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return response.read()
        except HTTPError:
            raise
        except (URLError, TimeoutError, ConnectionError, OSError) as error:
            last_error = error
            if attempt == 3:
                break
            time.sleep(1.0 + attempt)
    assert last_error is not None
    raise last_error


def fetch_text(url: str) -> str:
    return fetch_bytes(url).decode("utf-8", errors="replace")


DIRECTORY_ENTRY_RE = re.compile(
    r"(?P<modified>\d{1,2}/\d{1,2}/\d{4}\s+\d{1,2}:\d{2}\s+[AP]M)\s+"
    r"(?P<size>&lt;dir&gt;|\d+)\s+<A HREF=\"(?P<href>[^\"]+)\">(?P<name>[^<]+)</A><br>",
    re.IGNORECASE,
)


def parse_moex_directory_listing(base_url: str, html: str) -> list[dict]:
    entries: list[dict] = []
    for match in DIRECTORY_ENTRY_RE.finditer(html):
        href = match.group("href")
        name = match.group("name")
        if name == "[To Parent Directory]":
            continue
        size_token = match.group("size").lower()
        entries.append(
            {
                "name": name,
                "href": urllib.parse.urljoin(base_url, href),
                "is_dir": size_token == "&lt;dir&gt;",
                "size": 0 if size_token == "&lt;dir&gt;" else int(size_token),
                "upstream_modified": match.group("modified"),
            }
        )
    return entries


def file_url_from_path(path: Path) -> str:
    return path.resolve().as_uri()


def relative_posix_path(path: Path) -> str:
    return path.as_posix().lstrip("./")


def copy_file(src: Path, dst: Path) -> None:
    ensure_dir(dst.parent)
    shutil.copy2(src, dst)


def is_remote_source(source: str) -> bool:
    return source.startswith("http://") or source.startswith("https://")


def parse_bool(value: str | None) -> bool:
    if value is None:
        return False
    return value.lower() in {"1", "true", "yes", "on"}


def print_json(data: object) -> None:
    json.dump(data, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


def iter_files(root: Path) -> Iterable[Path]:
    for path in sorted(root.rglob("*")):
        if path.is_file():
            yield path
