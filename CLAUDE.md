# warp-lite: agent notes

- `main` is production. Each task gets its own branch from `main` and enters through a PR after Diego's OK. Never push to `main` directly.
- **Release**: a push to `main` runs `.github/workflows/release.yml`. It builds with `tools/build.sh --local`, then publishes `vX.Y.Z` in this repo. The tag points at the pushed commit (`--target`). Assets: `warp-lite-setup.exe` and `warp-lite.exe`. Installed copies update themselves from `releases/latest`.
- **No release when nothing that ships changed** since the previous tag. Shipped paths: `src third_party assets tools/build.sh`. The run still ends green.
- **Is it in production?** Look at `releases/latest`, never at the green check. A merge is in production when that release's tag contains it, or when the merge changes none of the shipped paths. dcbot closes a task as «✅ En prod» only when its health check passes: `prod-check release justdiegocom/warp-lite <clone> warp-lite-setup.exe warp-lite.exe -- src third_party assets tools/build.sh`. If the shipped paths change in `release.yml`, change that check in dcbot's `projects.toml` too. By hand: `gh release view -R justdiegocom/warp-lite`, then `git merge-base --is-ancestor <merge> vX.Y.Z`.
- Pending: the release has no zip and no `.sha256` yet, so the check can only confirm that the assets are present. It cannot compare digests.
