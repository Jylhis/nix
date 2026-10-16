---
synopsis: "New experimental feature `deterministic-config-merge`"
issues: [9487]
---

With the new `deterministic-config-merge` experimental feature, all plain `<name> = ...` assignments in a configuration source are applied before its `extra-<name>` appends.
An `extra-<name>` line that appears before the base assignment (for example in a file whose settings were emitted alphabetically, or in a flake's `nixConfig`, which is applied alphabetically) now appends to the base value instead of being silently discarded.
