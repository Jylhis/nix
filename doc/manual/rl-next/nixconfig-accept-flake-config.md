---
synopsis: "Accepting a flake's `accept-flake-config` no longer trusts its other settings"
---

When a flake's `nixConfig` set `accept-flake-config = true` and you allowed it at the prompt, the flake's remaining settings were applied without asking, because `accept-flake-config` sorts first and took effect immediately.
Nix now asks about every `nixConfig` setting before applying any of them, so each one is judged against the trust state from before the flake was loaded.
