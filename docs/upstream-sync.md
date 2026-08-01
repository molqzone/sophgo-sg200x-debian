# Upstream synchronization

The repository keeps the upstream board configurations intact and layers
MaixCAM on top of LicheeRV Nano. Use an explicit upstream remote:

```sh
git remote add upstream https://github.com/Fishwaldo/sophgo-sg200x-debian.git
git fetch upstream
git merge upstream/master
```

Resolve conflicts with these ownership rules:

1. Preserve upstream changes in `configs/settings.mk`, `configs/duo256`,
   `configs/duos`, `configs/licheervnano`, and generic addons.
2. Reapply product differences only in `configs/maixcam` or `maixcam-*` addons.
3. Add a patch tombstone when MaixCAM must suppress a newly inherited patch.
4. Never add a board-name conditional to a workflow; update the matrix or
   settings instead.
5. Keep toolchain and SDK revisions in their owning pin files.

Run before merging:

```sh
bash scripts/ci/validate.sh
python3 scripts/ci/plan.py matrix
```

Then dispatch the full image workflow. Acceptance requires all original four
matrix entries and MaixCAM to build from the same checkout. Confirm DuoS eMMC
uploads a ZIP and every SD entry uploads an IMG-derived artifact.
