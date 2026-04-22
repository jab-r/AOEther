# third_party/

Out-of-tree dependencies that AOEther builds against but does not vendor.

Each entry is a git submodule pinned to a specific upstream commit. To initialize after cloning AOEther:

```sh
git submodule update --init --recursive
```

## la_avdecc

[L-Acoustics avdecc](https://github.com/L-Acoustics/avdecc) — the reference IEEE 1722.1 / Milan entity library. Used by AOEther starting at M7 Phase B to provide an AVDECC responder so Hive and other Milan controllers can discover AOEther receivers and talkers.

If this is a fresh checkout and you are the first person wiring up Phase B, add the submodule with:

```sh
git submodule add https://github.com/L-Acoustics/avdecc.git third_party/la_avdecc
cd third_party/la_avdecc
git checkout <stable tag>          # e.g. v4.1.0 — pin to a known-good release
cd -
git add third_party/la_avdecc .gitmodules
git commit -m "Pin la_avdecc submodule to <tag>"
```

la_avdecc is C++17 and builds via CMake. AOEther's `avdecc/Makefile` shells out to CMake to produce a static lib plus the C-binding headers; the main talker and receiver binaries stay C11 and link the resulting object through [`common/avdecc.c`](../common/avdecc.c).

### Build dependencies for la_avdecc (Linux)

```sh
sudo apt install cmake g++ libpcap-dev
```

la_avdecc's own submodules (it is CMake-based, depends on `la_networkInterfaceHelper`, `la_common`, json, etc.) are pulled recursively by the `--recursive` flag above.
