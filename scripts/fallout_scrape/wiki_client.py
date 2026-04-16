"""MediaWiki API client with on-disk cache and polite rate limiting.

Used by the Fallout scrapers. The cache means re-running is free; parsing and
normalization iterate without re-hitting the wiki.
"""
import hashlib
import json
import time
from pathlib import Path
from typing import Any

import requests

CACHE_DIR = Path(__file__).parent / "cache"
CACHE_DIR.mkdir(exist_ok=True)

USER_AGENT = "MeshBBS-FalloutScraper/0.1 (private BBS project; github.com/goatsandmonkeys/mesh_bbs)"
RATE_LIMIT_SEC = 1.0

_last_request_at = 0.0


def _throttle():
    global _last_request_at
    delta = time.monotonic() - _last_request_at
    if delta < RATE_LIMIT_SEC:
        time.sleep(RATE_LIMIT_SEC - delta)
    _last_request_at = time.monotonic()


def _cache_key(site: str, params: dict) -> Path:
    # Stable hash of site + sorted params
    items = sorted(params.items())
    raw = site + "?" + "&".join(f"{k}={v}" for k, v in items)
    h = hashlib.sha256(raw.encode("utf-8")).hexdigest()[:24]
    return CACHE_DIR / f"{h}.json"


def api_get(site: str, params: dict, refresh: bool = False) -> dict:
    """GET a MediaWiki API endpoint with caching and automatic retry.

    site: full URL to api.php (e.g. https://fallout.fandom.com/api.php)
    params: query params; `format=json` is forced.
    refresh: if True, ignore cache and re-fetch.
    """
    params = {**params, "format": "json"}
    cache_path = _cache_key(site, params)
    if not refresh and cache_path.exists():
        return json.loads(cache_path.read_text(encoding="utf-8"))

    _throttle()
    last_exc = None
    for attempt in range(3):
        try:
            resp = requests.get(site, params=params, headers={"User-Agent": USER_AGENT}, timeout=45)
            resp.raise_for_status()
            data = resp.json()
            cache_path.write_text(json.dumps(data), encoding="utf-8")
            return data
        except Exception as exc:
            last_exc = exc
            if attempt < 2:
                wait = (attempt + 1) * 5  # 5s, 10s backoff
                print(f"  [wiki] retry {attempt+1} after {wait}s: {type(exc).__name__}",
                      file=__import__("sys").stderr)
                time.sleep(wait)
    raise last_exc


def iter_category_members(site: str, category: str, namespace: int = 0) -> list[str]:
    """Return all page titles in a category (pages only, not subcategories).

    Handles continuation automatically. Cached per-page.
    """
    titles: list[str] = []
    cont: dict = {}
    while True:
        params = {
            "action": "query",
            "list": "categorymembers",
            "cmtitle": f"Category:{category}",
            "cmlimit": 500,
            "cmtype": "page",
            "cmnamespace": str(namespace),
        }
        if cont:
            params.update(cont)
        data = api_get(site, params)
        members = data.get("query", {}).get("categorymembers", [])
        titles.extend(m["title"] for m in members)
        cont = data.get("continue") or {}
        if not cont:
            break
    return titles


def iter_subcategories(site: str, category: str) -> list[str]:
    """Return all direct subcategory titles for a category."""
    titles: list[str] = []
    cont: dict = {}
    while True:
        params = {
            "action": "query",
            "list": "categorymembers",
            "cmtitle": f"Category:{category}",
            "cmlimit": 500,
            "cmtype": "subcat",
        }
        if cont:
            params.update(cont)
        data = api_get(site, params)
        members = data.get("query", {}).get("categorymembers", [])
        # Returns "Category:Foo" titles — strip the "Category:" prefix
        titles.extend(m["title"].removeprefix("Category:") for m in members)
        cont = data.get("continue") or {}
        if not cont:
            break
    return titles


def iter_category_recursive(
    site: str,
    category: str,
    skip_patterns: tuple[str, ...] = (),
    namespace: int = 0,
    max_depth: int = 10,
    _depth: int = 0,
    _seen: set | None = None,
) -> list[str]:
    """Return page titles from a category AND all non-skipped subcategories.

    skip_patterns: substrings; a subcategory is excluded if its name (lowercased)
    contains any of them.  E.g. skip_patterns=("cut ", "images", "sounds").
    max_depth: maximum recursion depth (0 = root only, 1 = root + direct subcats, ...).
    """
    if _seen is None:
        _seen = set()
    if category.lower() in _seen:
        return []
    _seen.add(category.lower())

    titles = iter_category_members(site, category, namespace=namespace)

    if _depth < max_depth:
        for subcat in iter_subcategories(site, category):
            low = subcat.lower()
            if any(pat in low for pat in skip_patterns):
                continue
            titles.extend(
                iter_category_recursive(
                    site, subcat, skip_patterns, namespace,
                    max_depth, _depth + 1, _seen
                )
            )

    # Deduplicate while preserving order
    seen_titles: set[str] = set()
    result: list[str] = []
    for t in titles:
        if t not in seen_titles:
            seen_titles.add(t)
            result.append(t)
    return result


def get_wikitext(site: str, title: str) -> str:
    """Return raw wikitext for a page, or empty string on failure."""
    data = api_get(site, {"action": "parse", "page": title, "prop": "wikitext"})
    return data.get("parse", {}).get("wikitext", {}).get("*", "")


def get_page_plaintext(site: str, title: str) -> str:
    """Return extracted plain text intro for a page (first ~500 chars of the intro)."""
    data = api_get(
        site,
        {
            "action": "query",
            "titles": title,
            "prop": "extracts",
            "exintro": 1,
            "explaintext": 1,
            "redirects": 1,
        },
    )
    pages = data.get("query", {}).get("pages", {})
    for _, page in pages.items():
        return page.get("extract", "") or ""
    return ""
