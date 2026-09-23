#!/usr/bin/env python3
"""Regenerate the "run it in your browser" Compiler Explorer link.

The permalink embeds a snapshot of include/lru/lru_cache.hpp, so it goes stale
whenever the header changes in a way the demo depends on. Run this to mint a
fresh one and paste the URL into README.md.

    python3 tools/make_online_demo.py

It checks that the demo compiles without diagnostics and prints exactly the
expected output before creating a link, and refuses to publish one otherwise.
"""

import json
import pathlib
import sys
import urllib.request

REPO = pathlib.Path(__file__).resolve().parent.parent
HEADER = REPO / "include" / "lru" / "lru_cache.hpp"
COMPILER = "g142"
OPTIONS = "-std=c++17 -O2 -pthread"

BANNER = """// LRUCache online demo. Press the Run button.
//
// main() is at the bottom of this file. The library header is pasted above it so
// the demo is one self-contained file. In a real project you write:
//
//     #include <lru/lru_cache.hpp>
//
// Source and install instructions: https://github.com/LinkaiQi/LRUCache

"""

DEMO = r"""
// ===========================================================================
//  The demo starts here. Edit anything below and press Run.
// ===========================================================================

#include <iostream>
#include <thread>

using namespace std::chrono_literals;

int main() {
    lru::LRUCache<std::string, std::string> cache(2);  // capacity 2

    cache.put("a", "apple");
    cache.put("b", "banana");
    std::cout << "get(a)   -> " << cache.get("a").value_or("<miss>") << "\n";

    cache.put("c", "cherry");  // evicts "b", the least recently used
    std::cout << "get(b)   -> " << cache.get("b").value_or("<miss>") << "\n";
    std::cout << "get(c)   -> " << cache.get("c").value_or("<miss>") << "\n";

    // Entries can expire. This one lives for 50ms.
    lru::LRUCache<std::string, std::string> session(100, 50ms);
    session.put("token", "abc123");
    std::this_thread::sleep_for(80ms);
    std::cout << "expired  -> " << session.get("token").value_or("<miss>") << "\n";

    // Compute on miss, atomically, without holding the lock during the factory.
    const std::string computed = cache.get_or_compute("d", [] { return std::string("durian"); });
    std::cout << "computed -> " << computed << "\n";

    const lru::CacheStats stats = cache.stats();
    std::cout << "stats    -> hits=" << stats.hits << " misses=" << stats.misses
              << " evictions=" << stats.evictions << "\n";
}
"""

EXPECTED_OUTPUT = [
    "get(a)   -> apple",
    "get(b)   -> <miss>",
    "get(c)   -> cherry",
    "expired  -> <miss>",
    "computed -> durian",
    "stats    -> hits=2 misses=2 evictions=2",
]


def post(url, payload):
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json", "Accept": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=120) as response:
        return json.load(response)


def build_source():
    # The include guard warns when the header is the main file, so drop it.
    header = HEADER.read_text().replace("#pragma once\n", "", 1)
    return BANNER + header + DEMO


def verify(source):
    result = post(
        "https://godbolt.org/api/compiler/%s/compile" % COMPILER,
        {
            "source": source,
            "options": {
                "userArguments": OPTIONS,
                "executeParameters": {"args": [], "stdin": ""},
                "compilerOptions": {"executorRequest": True},
                "filters": {"execute": True},
            },
            "lang": "c++",
        },
    )

    diagnostics = [line.get("text", "") for line in result.get("buildResult", {}).get("stderr", [])]
    output = [line.get("text", "") for line in result.get("stdout", [])]

    ok = True
    if result.get("buildResult", {}).get("code") != 0:
        print("build failed")
        ok = False
    if diagnostics:
        print("build produced diagnostics:")
        for line in diagnostics:
            print("  " + line)
        ok = False
    if output != EXPECTED_OUTPUT:
        print("unexpected program output:")
        for line in output:
            print("  got:  " + line)
        for line in EXPECTED_OUTPUT:
            print("  want: " + line)
        ok = False
    return ok, output


def main():
    source = build_source()
    print("demo is %d lines, building on %s ..." % (source.count("\n"), COMPILER))

    ok, output = verify(source)
    if not ok:
        print("refusing to publish a link for a demo that does not build cleanly")
        return 1

    print("clean build, output matches:")
    for line in output:
        print("  " + line)

    short = post(
        "https://godbolt.org/api/shortener",
        {
            "sessions": [
                {
                    "id": 1,
                    "language": "c++",
                    "source": source,
                    "compilers": [],
                    "executors": [
                        {
                            "compiler": {"id": COMPILER, "libs": [], "options": OPTIONS},
                            "wrap": True,
                        }
                    ],
                }
            ]
        },
    )
    print("\npermalink: %s" % short.get("url"))
    print("paste it into README.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
