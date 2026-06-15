# VersionChecker Expansion

## Patch Notes

- Expanded the built-in version checker to check both `mctde` and `mctde-link`.
- `latest.txt` now uses explicit key/value rows:

```text
mctde=0.88
mctde-link=0.1.1
```

- If mctde is out of date, the prompt says `mctde is out of date. Would you like to update?` and Yes opens:

```text
https://www.nexusmods.com/darksouls/mods/1926
```

- If only mctde-link is out of date, the prompt says `mctde-link is out of date. Would you like to update?` and Yes opens:

```text
https://github.com/McRoodyPoo/mctde-Link/releases
```

- If both are out of date, the mctde prompt takes priority and Yes opens the Nexus Mods mctde page.
- Pressing No on an update prompt closes Dark Souls.

## Included

The end-user release package contains only:

- `d3d9.dll`
- `mctde-link.ini`
- `README.md`
- `CONFIG_REFERENCE.md`
