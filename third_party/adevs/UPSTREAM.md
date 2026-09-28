# Pinned adevs reference

Source: https://github.com/smiz/adevs

Commit: `ee9fed91224ed412088a3795ea8e67ffe2ee9b59`

This directory is a source snapshot of the upstream repository at that commit. Its source and `copyright.txt` are preserved; no upstream files have been changed. `UPSTREAM.md` is the only AnkuraFathom addition. The repository is used as a buildable reference oracle and source to study for the forthcoming DEVS kernel. AnkuraFathom's current economics simulator does not link to adevs; the eventual runtime will have its own implementation and will compare event traces against this pinned version.

The source was obtained from the upstream Git repository with `git archive`. To update it, record a new upstream commit, preserve the copyright notice, and run the reference and conformance tests before changing this pin.
