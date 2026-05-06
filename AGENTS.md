This repository contains a ZMK module with Web UI using the **unofficial** custom ZMK Studio RPC protocol.

## Initialization (first time only)

This repo is created from template. Run the following to find all places that need to be replaced:

```
rg '(t|T)emplate'
```

Key things to replace:

- Rename `proto/zmk/template/template.proto` and `src/studio/template_handler.c` with your feature name, and update all references found by the search above.
- Update `zephyr/module.yml`: change the module name.
- Update `README.md`: replace template descriptions with your module's description.
- Update `web/vite.config.ts`: change `base` to your repository name.

Remove this "Initialization" section from AGENTS.md (CLAUDE.md is symlink) after completing all items.

## Dev Rules

- Commit changes at each milestone. Ensure pre-commit works and never bypass pre-commit check.
- Write simple and sufficient tests for new features.
  - Unit test: Test major functionalities as much as possible by adding case to `tests/<test case>`.
    You might have to add test only logic like executing logic at zephyr initialization to improve test coverage.
  - Build test: Enable feature in `tests/zmk-config/*`. It is to verify build works for real device and to easily test with real device.
    Ensure the feature and device is enabled as expected in the build by verifying output in `test.py`.
- Update README.md properly to guide how to use the module to unfamiliar ZMK keyboard users. Keep the guide simple but sufficient!
- Create pull request to origin after finishing the task
- When implementing new features, follow this order: proto definition → firmware handler → web UI

## Commands

Test command usually takes 1min.

```
# Run lint and test when required
pre-commit run
# Run unit test + build test and verify the results
python3 -m unittest
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
# Run web tests
cd web && npm test
```
