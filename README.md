# Barrier

Eliminate the barrier between your machines.
Find upstream [releases for Windows and macOS here](https://github.com/debauchee/barrier/releases).
For this macOS-maintained fork, download [Barrier 3.2.0 for macOS arm64](https://github.com/ibetterai/barrier/releases/tag/v3.2.0).
This fork's current packaged build is for Apple Silicon macOS only.
Your distro probably already has barrier packaged for it, see [distro specific packages](#distro-specific-packages)
below for a list. Alternatively, we also provide a [flatpak](https://github.com/flathub/com.github.debauchee.barrier)
and a [snap](https://snapcraft.io/barrier).

### Contact info:

Use the GitHub issue tracker for project support.

### iBetterAI macOS fork

The iBetterAI 3.x builds are a macOS Apple Silicon-focused fork of Barrier 2.4.0. They are currently packaged and tested for Apple Silicon macOS only; use the upstream Barrier releases or your platform package manager for Windows, Linux, Intel macOS, and BSD builds.

The signed release DMG and SHA-256 checksum are published on the [Barrier 3.2.0 release page](https://github.com/ibetterai/barrier/releases/tag/v3.2.0).

Changes since Barrier 2.4.0:

- Native Apple Silicon macOS build and release packaging.
- Dynamic macOS physical display geometry support for non-rectangular layouts, including L-shaped monitor arrangements.
- Freeform server configuration that follows the live macOS Displays layout when displays move and the configuration is re-saved.
- Cross-machine edge routing based on real display rectangles, while preserving normal local macOS transitions between displays on the same host.
- macOS display labels in the freeform canvas, using OS-provided display names when available.
- Client display wake-on-entry when the target macOS display system is asleep.
- Consistent perceived direction for Magic Mouse and trackpad two-finger vertical and horizontal scrolling across hosts.
- Menu bar actions that reliably foreground Barrier windows such as settings and logs on macOS.
- Magic Mouse two-finger left/right Spaces swipe forwarding in v3.2.0. When the pointer is on a macOS client, the server detects the swipe intent and the client injects the native `Control` + left/right Space-switching shortcut, allowing Spaces and full-screen apps to switch on the target.

### What is it?

Barrier is software that mimics the functionality of a KVM switch, which historically would allow you to use a single keyboard and mouse to control multiple computers by physically turning a dial on the box to switch the machine you're controlling at any given moment. Barrier does this in software, allowing you to tell it which machine to control by moving your mouse to the edge of the screen, or by using a keypress to switch focus to a different system.

Barrier was forked from Symless's Synergy 1.9 codebase. Synergy was a commercialized reimplementation of the original CosmoSynergy written by Chris Schoeneman.

At the moment, barrier is not compatible with synergy. Barrier needs to be installed on all machines that will share keyboard and mouse.

### What's different?

Whereas Synergy has moved beyond its goals from the 1.x era, Barrier aims to maintain that simplicity.
Barrier will let you use your keyboard and mouse from one computer to control one or more other computers.
Clipboard sharing is supported.
That's it.

### Project goals

Hassle-free reliability. We are users, too. Barrier was created so that we could solve the issues we had with synergy and then share these fixes with other users.

Compatibility. We use more than one operating system and you probably do, too. Windows, OSX, Linux, FreeBSD... Barrier should "just work". We will also have our eye on Wayland when the time comes.

Communication. Everything we do is in the open. Our issue tracker will let you see if others are having the same problem you're having and will allow you to add additional information. You will also be able to see when progress is made and how the issue gets resolved.

### Usage

Install and run barrier on each machine that will be sharing.
On the machine with the keyboard and mouse, make it the server.

Click the "Configure server" button and drag a new screen onto the grid for each client machine.
Ensure the "screen name" matches exactly (case-sensitive) for each configured screen -- the clients' barrier windows will tell you their screen names (just above the server IP).

On the client(s), put in the server machine's IP address (or use Bonjour/auto configuration when prompted) and "start" them.
You should see `Barrier is running` on both server and clients.
You should now be able to move the mouse between all the screens as if they were the same machine.
On Apple Silicon macOS, the iBetterAI 3.x fork adds dynamic physical-display routing, L-shaped layout support, target display wake-on-entry, consistent two-finger scrolling, and v3.2.0 Magic Mouse two-finger Spaces swipe forwarding. See [iBetterAI macOS fork](#ibetterai-macos-fork) above for details.


Note that if the keyboard's Scroll Lock is active then this will prevent the mouse from switching screens.

### Contact & support

Use the GitHub issue tracker for project support.

### Contributions

At this time we are looking for developers to help fix the issues found in the issue tracker.
Submit pull requests once you've polished up your patch and we'll review and possibly merge it.

Most pull requests will need to include a release note.
See docs/newsfragments/README.md for documentation of how to do that.

## Distro specific packages

While not a comprehensive list, repology provides a decent list of distro
specific packages.

[![Packaging status](https://repology.org/badge/vertical-allrepos/barrier.svg)](https://repology.org/project/barrier/versions)

## FAQ - Frequently Asked Questions

**Q: Does drag and drop work on linux?**

> A: No *(see [#855](https://github.com/debauchee/barrier/issues/855) if you'd like to change that)*


**Q: What OSes are supported?**

> A: The [most recent release](https://github.com/debauchee/barrier/releases/latest) of Barrier is known to work on:
>  - Windows 7, 8, 8.1, 10, and 11
>  - macOS *(previously known as OS X or Mac OS X)*  
>    - _The current GUI does **not** work on OS versions prior to macOS 10.12 Sierra (but see the related answer below)_
>  - Linux
>  - FreeBSD
>  - OpenBSD


**Q: Are 32-bit versions of Windows supported?**

> A: No


__Q: Is it possible to use Barrier on Mac OS X / OS X versions prior to 10.12?__

> A: Not officially.
>   - For OS X 10.10 Yosemite and later:
>     - [Barrier v2.1.0](https://github.com/debauchee/barrier/releases/tag/v2.1.0) or earlier _may_ work.
>   - For Mac OS X 10.9 Mavericks _(and perhaps earlier)_:
>     1. the command-line portions of the [current release](https://github.com/debauchee/barrier/releases/latest) _should_ run fine.
>     2. The GUI will _not_ run, as that OS version does not include Apple's *Metal* framework.
>         - _(For a GUI workaround for Mac OS X 10.9, see the [discussion at issue #544](https://github.com/debauchee/barrier/issues/544))_

> Note: Only versions [v2.3.4](https://github.com/debauchee/barrier/releases/tag/v2.3.4) and [later](https://github.com/debauchee/barrier/releases/latest) of Barrier can be supported by this project.
>  - Anyone using an earlier version is advised to upgrade due to recently-addressed security vulnerabilities *(and other bug fixes)*. 
>    - This is especially important for computers accessible from the public Internet *(or from other shared/untrusted networks, such as when using shared WiFi)*.


**Q: How do I load my configuration on startup?**

> A: Start the binary with the argument `--config <path_to_saved_configuration>`


**Q: After loading my configuration on the client the field 'Server IP' is still empty!**

> A: Edit your configuration to include the server's ip address manually with
> 
>```
>(...)
>
>section: options
>    serverhostname=<AAA.BBB.CCC.DDD>
>```

**Q: Are there any other significant limitations with the current version of Barrier?**

> A: Currently:
>    - Barrier currently has limited UTF-8 support; issues have been reported with processing various languages.
>      - *(see [#860](https://github.com/debauchee/barrier/issues/860))*
>    - There is interest in future support for the Wayland compositor/display server protocol *([official site](https://wayland.freedesktop.org/) | [Wikipedia article](https://en.wikipedia.org/wiki/Wayland_(display_server_protocol)))* on Linux.
>      - As of late 2021, there is no expected completion date for *Wayland* support.
>      - *(see [#109](https://github.com/debauchee/barrier/issues/109) and [#1251](https://github.com/debauchee/barrier/issues/1251) for status or to volunteer your talents)*
>
> The complete list of open issues can be found in the ['Issues' tab on GitHub](https://github.com/debauchee/barrier/issues?q=is%3Aissue+is%3Aopen). Help is always appreciated.
