# Check A Download

Every file in the release has a line in `SHA256SUMS`, the VPK included.

## Optional Offline Checker

Extract `RetroAdhoc-install.zip` into a new folder. If you have Python 3, open a
terminal in that folder and run:

```sh
python3 tools/check-files.py        # macOS or Linux
py -3 tools/check-files.py          # Windows
```

It reads local files only: no network, no Vita, and nothing is changed. `PASS`
means every listed file matches. If a file is missing or changed, extract a
fresh copy before installing.

## Without Python

Check one file with your computer's own command and compare the result with
its line in `SHA256SUMS`:

| Computer | Command |
| --- | --- |
| Windows PowerShell | `Get-FileHash -Algorithm SHA256 RetroAdhoc.vpk` |
| macOS Terminal | `shasum -a 256 RetroAdhoc.vpk` |
| Linux Terminal | `sha256sum RetroAdhoc.vpk` |

A matching checksum means the file arrived intact. It says nothing about game
compatibility. Download Retro Adhoc only from
[its releases page](https://github.com/CBOC-Vita-Lab/vitadhoc/releases).
