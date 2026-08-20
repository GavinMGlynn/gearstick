# Releases — what you are downloading, and why your computer will complain

**The short version: the releases are not code-signed, your operating system
will say so, and here is exactly what it will say and what to do about it.**

That is a deliberate choice rather than an oversight, and this document exists
because "just click through the warning" is terrible advice that people give
constantly. If you would rather not click through it, [build from
source](../README.md) — it takes one command and about five minutes.

---

## Why unsigned

Code signing means paying a certificate authority so that Microsoft and Apple
recognise a name attached to the binary.

- **Windows**: an Authenticode certificate, roughly a few hundred dollars a
  year. An OV certificate does not silence SmartScreen immediately either — the
  warning fades as the certificate builds "reputation", which for a small
  project can take a long time and many downloads.
- **macOS**: an Apple Developer Program membership at $99/year, plus
  notarisation, for Gatekeeper to open the app without ceremony.

Neither of those makes the software safer. They attach a paid-for identity to
it. What actually tells you a binary is what it claims to be is *where it was
built and from what*, and that is provided here instead, for free and in a form
you can check yourself:

**Every release is built by GitHub Actions from a public commit, and carries a
build provenance attestation.** That is a signed statement — by GitHub's own
infrastructure, using Sigstore — saying "this exact file was produced by this
workflow, from this commit, in this repository". It is a stronger claim than a
code-signing certificate makes, because a certificate says who paid for it and
says nothing at all about what went in.

You can verify it, and the next section says how.

---

## Verifying what you downloaded

You will need the [GitHub CLI](https://cli.github.com/).

```sh
gh attestation verify gearstick-1.0.0-linux-x86_64.tar.gz \
   --repo GavinMGlynn/gearstick
```

That checks the file against the signed provenance. If it passes, the file was
built by this project's CI from this project's source, and has not been altered
since.

There are also SHA-256 checksums with every release:

```sh
sha256sum -c SHA256SUMS          # Linux
shasum -a 256 -c SHA256SUMS      # macOS
```

```powershell
Get-FileHash gearstick-1.0.0-windows-x64.zip -Algorithm SHA256   # Windows
```

Checksums catch a corrupted download. **They do not catch a malicious one** — if
somebody could replace the file they could replace the checksum beside it. The
attestation is the one that means something.

---

## What your computer will say

### Windows

Running the extracted `gearstick.exe` gives:

> **Windows protected your PC**
> Microsoft Defender SmartScreen prevented an unrecognised app from starting.

Click **More info**, then **Run anyway**.

If you downloaded the zip with a browser, Windows may also have marked it as
coming from the internet. If the game will not start, right-click the zip →
**Properties** → tick **Unblock** → **OK**, then extract it again.

### macOS

Opening `Gearstick.app` gives:

> **"Gearstick" cannot be opened because Apple cannot check it for malicious
> software.**

Right-click (or Control-click) the app → **Open** → **Open** in the dialog that
follows. Double-clicking will keep refusing; the right-click route is the one
that offers to proceed.

On newer macOS you may need **System Settings → Privacy & Security**, then
**Open Anyway** next to the message about Gearstick.

If you get *"Gearstick is damaged and can't be opened"*, that is the quarantine
attribute rather than actual damage:

```sh
xattr -dr com.apple.quarantine /Applications/Gearstick.app
```

### Linux

Nothing will complain. Extract the tarball and run `./gearstick`.

You may need SDL's runtime dependencies if your distribution is minimal — a
desktop install will already have them:

```sh
# Debian/Ubuntu
sudo apt install libx11-6 libxext6 libwayland-client0 libasound2 libpulse0

# Fedora/RHEL
sudo dnf install libX11 libXext libwayland-client alsa-lib pulseaudio-libs
```

---

## What is in a release

**Linux needs glibc 2.38 or newer** — Ubuntu 24.04, Debian 13, Fedora 39 and
anything more recent. That is not an arbitrary floor: the simulation is written
in C23, and C23's headers redirect `sscanf`, `strtol` and their relatives to
symbols glibc only grew in 2.38. On an older distribution the game will start
and immediately say `GLIBC_2.38 not found`, which is what that means. Building
from source on such a system works if the compiler is new enough.

| Platform | File | Contents |
| --- | --- | --- |
| Linux x86_64 | `gearstick-VERSION-linux-x86_64.tar.gz` | `gearstick`, `gearstick_server`, `gearstick_cli`, `assets/` |
| macOS arm64 | `gearstick-VERSION-macos-arm64.dmg` | `Gearstick.app` |
| macOS arm64 | `gearstick-VERSION-macos-arm64.tar.gz` | the same, as a folder |
| Windows x64 | `gearstick-VERSION-windows-x64.msi` | an installer: the same three programs and `assets\`, plus a Start Menu entry |
| Windows x64 | `gearstick-VERSION-windows-x64.zip` | the same, as a folder to unpack |

**On Windows, take the `.msi` unless you have a reason not to.** It puts the
game in the Start Menu and uninstalls from Settings like anything else. The zip
is there for people who would rather not run an installer — it needs no
administrator, goes wherever you unpack it, and leaves nothing behind when you
delete the folder. Both contain exactly the same programs.

`gearstick` is the game.

`gearstick_server` is the meeting point for online play. You do not need it to
play — two people can reach each other directly with `--host` and `--join` — and
you do need it if you want to run a place people come back to. It has no window,
draws a live view in the terminal, and is happy on a machine with no screen.

`gearstick_cli` is the same simulation with no window attached. It is what
re-races a replay to check it, and running `gearstick_cli selftest --verify` is
the first thing to do if anything seems wrong. See
[the player's guide](GUIDE.md).

SDL is linked statically, so there is nothing to install alongside it.

---

## Checking a release actually runs

Every release is tested by unpacking it somewhere else entirely and running it
from there — not by checking that the packaging step exited zero, which says
nothing about whether the binary inside can find its own feet. The packaged
`gearstick_cli` re-races the golden replay and must land on the same state hash
CI got, and the packaged game draws a frame, which is what proves the installed
layout is one the asset probe can actually navigate.

The Linux package is additionally run inside a container that has **no compiler,
no CMake and no SDL installed** — a machine that has never had a toolchain on
it, which is what the release is for.

---

## If you would rather build it

```sh
git clone https://github.com/GavinMGlynn/gearstick
cd gearstick
git submodule update --init --depth 1 ext/sdl ext/imgui ext/sdl_net
cmake --preset linux-release
cmake --build --preset linux-release
./build/linux-release/gearstick
```

Then nothing is unsigned, because nothing was downloaded.
