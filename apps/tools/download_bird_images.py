#!/usr/bin/env python3
"""Download reusable bird photos and attribution data from Wikimedia Commons.

The script uses the public MediaWiki API rather than scraping HTML. It only
keeps images whose Commons metadata identifies a reusable license, and writes
all attribution fields to metadata.json next to the downloaded images.
"""

from __future__ import annotations

import argparse
import html
import json
import os
import random
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import deque
from pathlib import Path
from typing import Any, Iterable

API_URL = "https://commons.wikimedia.org/w/api.php"
USER_AGENT = os.environ.get(
    "WIKIMEDIA_USER_AGENT",
    "BirdAppImageCollector/1.0 (personal educational project; Wikimedia Commons API)",
)
DEFAULT_CATEGORY = "Quality images of birds"
ALLOWED_LICENSE_MARKERS = (
    "cc0",
    "public domain",
    "cc by",
    "creative commons attribution",
)
CONTENT_TYPE_EXTENSIONS = {
    "image/jpeg": ".jpg",
    "image/png": ".png",
    "image/webp": ".webp",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Download bird photos from a Wikimedia Commons category."
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("assets/birds"),
        help="destination directory (default: assets/birds)",
    )
    parser.add_argument("-n", "--count", type=int, default=50, help="total images wanted")
    parser.add_argument(
        "--category",
        default=DEFAULT_CATEGORY,
        help=f"Commons category, with or without 'Category:' (default: {DEFAULT_CATEGORY!r})",
    )
    parser.add_argument(
        "--width",
        type=int,
        default=1280,
        help="requested thumbnail width in pixels (default: 1280)",
    )
    parser.add_argument(
        "--depth",
        type=int,
        default=1,
        help="how many levels of subcategories to inspect (default: 1)",
    )
    parser.add_argument("--seed", type=int, default=2025, help="selection seed")
    parser.add_argument(
        "--delay",
        type=float,
        default=0.15,
        help="seconds to wait between downloads (default: 0.15)",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="download files again instead of resuming from metadata.json",
    )
    args = parser.parse_args()
    if args.count < 1:
        parser.error("--count must be at least 1")
    if args.width < 100:
        parser.error("--width must be at least 100")
    if args.depth < 0:
        parser.error("--depth cannot be negative")
    if args.delay < 0:
        parser.error("--delay cannot be negative")
    return args


def request(url: str, *, attempts: int = 4) -> urllib.response.addinfourl:
    """Open a URL with retries and a Wikimedia-friendly user agent."""
    last_error: Exception | None = None
    for attempt in range(attempts):
        try:
            return urllib.request.urlopen(
                urllib.request.Request(url, headers={"User-Agent": USER_AGENT}), timeout=45
            )
        except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as error:
            last_error = error
            if isinstance(error, urllib.error.HTTPError) and error.code < 500 and error.code != 429:
                raise
            retry_after = error.headers.get("Retry-After") if isinstance(error, urllib.error.HTTPError) else None
            try:
                pause = max(2**attempt, float(retry_after)) if retry_after else 2**attempt
            except ValueError:
                pause = 2**attempt
            time.sleep(min(pause, 60))
    assert last_error is not None
    raise last_error


def api(parameters: dict[str, Any]) -> dict[str, Any]:
    parameters = {
        "action": "query",
        "format": "json",
        "formatversion": "2",
        "maxlag": "5",
        **parameters,
    }
    url = API_URL + "?" + urllib.parse.urlencode(parameters)
    with request(url) as response:
        payload = json.load(response)
    if "error" in payload:
        raise RuntimeError(f"Commons API error: {payload['error']}")
    return payload


def category_title(value: str) -> str:
    return value if value.lower().startswith("category:") else f"Category:{value}"


def collect_titles(root: str, depth: int, limit: int) -> list[str]:
    """Breadth-first collection of file titles from a Commons category tree."""
    queue = deque([(category_title(root), 0)])
    visited: set[str] = set()
    files: list[str] = []

    while queue and len(files) < limit:
        category, level = queue.popleft()
        if category in visited:
            continue
        visited.add(category)
        continuation: str | None = None

        while len(files) < limit:
            parameters: dict[str, Any] = {
                "list": "categorymembers",
                "cmtitle": category,
                "cmtype": "file|subcat",
                "cmlimit": "500",
            }
            if continuation:
                parameters["cmcontinue"] = continuation
            payload = api(parameters)
            members = payload.get("query", {}).get("categorymembers", [])
            for member in members:
                if member["ns"] == 6:
                    files.append(member["title"])
                    if len(files) >= limit:
                        break
                elif member["ns"] == 14 and level < depth:
                    queue.append((member["title"], level + 1))
            continuation = payload.get("continue", {}).get("cmcontinue")
            if not continuation:
                break

    return files


def chunks(values: list[str], size: int) -> Iterable[list[str]]:
    for start in range(0, len(values), size):
        yield values[start : start + size]


def image_metadata(titles: list[str], width: int) -> Iterable[dict[str, Any]]:
    for batch in chunks(titles, 50):
        payload = api(
            {
                "prop": "imageinfo",
                "titles": "|".join(batch),
                "iiprop": "url|mime|size|extmetadata",
                "iiurlwidth": str(width),
            }
        )
        for page in payload.get("query", {}).get("pages", []):
            info = page.get("imageinfo")
            if info:
                yield {"title": page["title"], **info[0]}


def metadata_value(metadata: dict[str, Any], name: str) -> str:
    value = metadata.get(name, {})
    return str(value.get("value", "")).strip()


def plain_text(value: str) -> str:
    value = re.sub(r"<br\s*/?>", ", ", value, flags=re.IGNORECASE)
    value = re.sub(r"<[^>]+>", "", value)
    return " ".join(html.unescape(value).split())


def reusable_license(info: dict[str, Any]) -> bool:
    metadata = info.get("extmetadata", {})
    text = " ".join(
        [
            metadata_value(metadata, "LicenseShortName"),
            metadata_value(metadata, "UsageTerms"),
        ]
    ).lower()
    return any(marker in text for marker in ALLOWED_LICENSE_MARKERS)


def safe_stem(title: str) -> str:
    stem = title.removeprefix("File:").rsplit(".", 1)[0]
    stem = re.sub(r"[^a-zA-Z0-9]+", "-", stem).strip("-").lower()
    return stem[:80] or "bird"


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"source": "Wikimedia Commons", "images": []}
    with path.open(encoding="utf-8") as handle:
        manifest = json.load(handle)
    if not isinstance(manifest.get("images"), list):
        raise ValueError(f"Invalid manifest: {path}")
    return manifest


def save_manifest(path: Path, manifest: dict[str, Any]) -> None:
    temporary = path.with_suffix(".json.tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    temporary.replace(path)


def download(info: dict[str, Any], destination: Path, number: int) -> dict[str, Any]:
    url = info.get("thumburl") or info["url"]
    with request(url) as response:
        content_type = response.headers.get_content_type().lower()
        extension = CONTENT_TYPE_EXTENSIONS.get(content_type)
        if extension is None:
            raise ValueError(f"unsupported response type {content_type!r}")
        filename = f"{number:04d}-{safe_stem(info['title'])}{extension}"
        path = destination / filename
        temporary = path.with_suffix(path.suffix + ".part")
        with temporary.open("wb") as output:
            while block := response.read(1024 * 128):
                output.write(block)
        temporary.replace(path)

    extra = info.get("extmetadata", {})
    source_url = info.get("descriptionurl") or (
        "https://commons.wikimedia.org/wiki/" + urllib.parse.quote(info["title"].replace(" ", "_"))
    )
    return {
        "file": filename,
        "title": plain_text(metadata_value(extra, "ObjectName"))
        or info["title"].removeprefix("File:").rsplit(".", 1)[0],
        "source_title": info["title"],
        "source_url": source_url,
        "download_url": url,
        "artist": plain_text(metadata_value(extra, "Artist")) or "See source page",
        "credit": plain_text(metadata_value(extra, "Credit")),
        "license": plain_text(metadata_value(extra, "LicenseShortName")),
        "license_url": metadata_value(extra, "LicenseUrl"),
        "description": plain_text(metadata_value(extra, "ImageDescription")),
        "width": info.get("thumbwidth", info.get("width")),
        "height": info.get("thumbheight", info.get("height")),
    }


def main() -> int:
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest_path = args.output / "metadata.json"
    manifest = load_manifest(manifest_path)
    manifest.update(
        {
            "source": "Wikimedia Commons",
            "category": category_title(args.category),
            "requested_width": args.width,
        }
    )
    existing = {item["source_title"] for item in manifest["images"]}

    if len(manifest["images"]) >= args.count and not args.overwrite:
        print(f"Already have {len(manifest['images'])} images in {args.output}")
        return 0

    candidate_limit = max(250, args.count * 10)
    print(f"Finding up to {candidate_limit} candidates in {category_title(args.category)} ...")
    titles = collect_titles(args.category, args.depth, candidate_limit)
    random.Random(args.seed).shuffle(titles)

    if args.overwrite:
        for item in manifest["images"]:
            (args.output / item["file"]).unlink(missing_ok=True)
        manifest["images"] = []
        existing.clear()

    rejected = 0
    for info in image_metadata(titles, args.width):
        if len(manifest["images"]) >= args.count:
            break
        if info["title"] in existing:
            continue
        if info.get("mime") not in CONTENT_TYPE_EXTENSIONS or not reusable_license(info):
            rejected += 1
            continue
        try:
            item = download(info, args.output, len(manifest["images"]) + 1)
        except (OSError, ValueError, urllib.error.URLError) as error:
            print(f"Skipping {info['title']}: {error}", file=sys.stderr)
            continue
        manifest["images"].append(item)
        existing.add(info["title"])
        save_manifest(manifest_path, manifest)
        print(f"[{len(manifest['images'])}/{args.count}] {item['file']} — {item['license']}")
        time.sleep(args.delay)

    save_manifest(manifest_path, manifest)
    downloaded = len(manifest["images"])
    print(f"Saved {downloaded} images and attribution metadata to {args.output}")
    if downloaded < args.count:
        print(
            f"Only found {downloaded} suitable images ({rejected} candidates rejected). "
            "Try a larger --depth or another --category.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nStopped; completed downloads remain resumable.", file=sys.stderr)
        raise SystemExit(130)
    except (OSError, RuntimeError, urllib.error.URLError) as error:
        print(f"Wikimedia request failed: {error}", file=sys.stderr)
        raise SystemExit(1)
