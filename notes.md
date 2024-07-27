tests:
  - shallow clone is actually shallow in terms of what it pulls from git
  - shallow clone with submodules has the submodules also cloned shallowly
  - test that using the same cache dir for all the configurations (submodule, shallow, allRefs) doesn't thrash or cause problems

  - test that using a local git source with submodule pointers that are "dirty" triggers a fetch of the submodules *without updating anything in the local copy of the repo*

can include the benchmarking script if appropriate


```
nix-slot-a on  feat/git-fetchers-tweaks:master [!?⇡] via C v11.1.0-clang via ❄️  impure (nix-env)
❯ ./bench-fetch.sh
Running: shallow benchmark
  - getting store dir for '{ url = "ssh://git@github.com/qemu/qemu.git"; ref = "master"; rev = "2946e1af2704bf6584f57d4e3aec49d1d5f3ecc0"; shallow = true; }'...
  - store dir: /nix/store/89qmiip4dfkw10dz875y487mrq1wlfcl-source
  - checking that cache dir '/home/rahul/.cache/nix/gitv3/041vqrjg9lj9nzy5y3k5scxh708ycaqmyzzi2n0j7xwdi3rs5cg3' is produced...

Benchmark 1: /run/current-system/sw/bin/nix '--experimental-features' 'nix-command' 'eval' '--expr' '(builtins.fetchGit { url = "ssh://git@github.com/qemu/qemu.git"; ref = "master"; rev = "2946e1af2704bf6584f57d4e3aec49d1d5f3ecc0"; shallow = true; }).outPath' '--raw' '--tarball-ttl' '0'
 ⠧ Current estimate: 44.796 s ██████████████████████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ ETA 00:00:52    ⠇ Current estimate: 44.796 s ███████████████████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  ⠏ Current estimate: 44.796 s █████████████████████████████░░░░░░░░░░░░░░ ⠋ Current estimate: 44.796 s ███████████████████████░░░░░░░░░░░░░░ ⠧ Current estim ⠇ Curre ⠏ Current estimate: ⠋ C ⠙ Curre ⠼ C ⠴ Current e ⠧ Curre ⠇ Current estimate: 44.796 s ██████████████████████████ ⠏ Current estimate: 44. ⠸ C ⠼ Current estimate: 44.796 s ██████████ ⠴ Current estim  Time (mean ± σ):     44.412 s ±  0.543 s    [User: 82.697 s, System: 5.041 s]
  Range (min … max):   44.029 s … 44.796 s    2 runs

Benchmark 2: outputs/out/bin/nix '--experimental-features' 'nix-command' 'eval' '--expr' '(builtins.fetchGit { url = "ssh://git@github.com/qemu/qemu.git"; ref = "master"; rev = "2946e1af2704bf6584f57d4e3aec49d1d5f3ecc0"; shallow = true; }).outPath' '--raw' '--tarball-ttl' '0'
  Time (mean ± σ):     10.189 s ±  2.222 s    [User: 1.496 s, System: 0.610 s]
  Range (min … max):    8.618 s … 11.760 s    2 runs

Summary
  'outputs/out/bin/nix '--experimental-features' 'nix-command' 'eval' '--expr' '(builtins.fetchGit { url = "ssh://git@github.com/qemu/qemu.git"; ref = "master"; rev = "2946e1af2704bf6584f57d4e3aec49d1d5f3ecc0"; shallow = true; }).outPath' '--raw' '--tarball-ttl' '0'' ran
    4.36 ± 0.95 times faster than '/run/current-system/sw/bin/nix '--experimental-features' 'nix-command' 'eval' '--expr' '(builtins.fetchGit { url = "ssh://git@github.com/qemu/qemu.git"; ref = "master"; rev = "2946e1af2704bf6584f57d4e3aec49d1d5f3ecc0"; shallow = true; }).outPath' '--raw' '--tarball-ttl' '0''

------------------------------------------------------------------------------------------------------------------------------------------
```
