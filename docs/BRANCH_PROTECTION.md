# Main branch protection

PulseForge should protect `main` with a GitHub repository ruleset. The connected automation used during repository setup can read rulesets but cannot create or modify them, so this configuration must be applied in the GitHub repository settings.

## Recommended ruleset

Create a branch ruleset named `Protect main` and set its enforcement status to **Active**.

Target only:

- `refs/heads/main`

Enable these branch rules:

- Restrict deletions.
- Block force pushes.
- Require a pull request before merging.
- Require conversation resolution before merging.
- Require status checks to pass before merging.
- Require branches to be up to date before merging.

Require these exact status-check names:

1. `PulseForge cross-platform build validation`
2. `PulseForge deterministic core tests`
3. `PulseForge OSS boundary validation`

## Review policy for the current repository

PulseForge currently has one maintainer. Keep the required approving-review count at **0** unless a second trusted reviewer is added. Do not enable **Require review from Code Owners** while the PR author is also the only available code owner, because that would make ordinary self-maintained pull requests impossible to approve.

`.github/CODEOWNERS` still documents ownership and highlights sensitive files even while code-owner review enforcement is disabled.

## Merge policy

Prefer squash merges for repository-maintenance, CI and dependency PRs so `main` stays compact and each merged PR is represented by one commit. Engine/source changes should still pass all three required checks on their exact PR head before merge.

Do not bypass failed or pending required checks for release-management changes.

## Rules intentionally not required yet

Do not enable the following until the corresponding repository process exists:

- **Require signed commits** — enable only after the maintainer signing workflow is established and verified.
- **Required approving reviews > 0** — enable after a second trusted reviewer exists.
- **Require review from Code Owners** — same reason as above.

## Release-specific boundary

Branch protection does not replace the release publisher's own integrity checks. Public `v0.9.7` publication must still use `.github/workflows/publish-release.yml`, which validates the approved release-artifact run and verifies the existing draft asset set before changing publication state.
