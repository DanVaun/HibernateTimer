# HibernateTimer

A lightweight native Windows utility that exposes the hibernate timeout hidden by Windows on Modern Standby (S0) systems, along with screen/sleep timeouts and lid/power button controls.

\---

## Why does this exist?

Modern Windows laptops use **Modern Standby (S0)** instead of the older **S3 sleep** standard. On S0 systems, Windows deliberately hides the hibernate timer from both the modern Settings app and the legacy Control Panel — even though hibernate itself works perfectly and is already exposed in the lid and power button controls.

The only way to see or change the hibernate timer on an S0 system is through the command line:

```
powercfg /query SCHEME\_CURRENT SUB\_SLEEP HIBERNATEIDLE
```

This app exposes that timer in a clean, native Windows 11 style UI — nothing more, nothing less.

\---

## What it does

* **Screen, sleep \& hibernate timeouts** — plugged in and on battery, with the hibernate timer that Windows hides from the GUI
* **Lid \& power button controls** — all four combinations (plugged in / on battery × power button / lid close) with Do nothing / Sleep / Hibernate / Shut down options
* **Hibernate status check** — on launch the app checks whether hibernate is enabled. If it isn't, a warning banner appears with a one-click button to enable it via `powercfg /hibernate on`
* **Save only changed values** — the app tracks original values at load time and only writes settings that were actually changed
* **Conflict warning** — if you set the hibernate timer longer than or equal to the sleep timer, the app warns you that sleep will fire first and hibernate will never activate

\---

## How hibernate works on S0 (Modern Standby)

On S0 systems, the hibernate timer and sleep timer are **independent countdowns** from the last activity — they race each other. Whichever fires first wins:

* If hibernate is set **shorter** than sleep → hibernate fires first, sleep never happens
* If hibernate is set **longer** than or equal to sleep → sleep fires first, hibernate never happens

This is different from older S3 systems where sleep was a prerequisite step before hibernate. On S0 they are completely independent, so **set hibernate shorter than sleep** if you want it to actually activate.

The screen timeout, sleep timer, and hibernate timer are the only settings this app exposes. The broader power plan settings (processor state, PCI Express, etc.) are intentionally excluded — S0 power management is dynamic and managed cooperatively by Windows and the firmware. Changing those static S3-era settings on an S0 system can interfere with the dynamic power manager and produce worse results than leaving them alone.

\---

## Technical background

Finding the correct registry paths for the lid and power button controls required significant reverse engineering. These settings are **not** stored where `powercfg /query` reports them — the values shown there for the buttons subgroup never change regardless of what you set in Windows Settings.

The actual storage locations were discovered using [Process Monitor](https://learn.microsoft.com/en-us/sysinternals/downloads/procmon) from Microsoft Sysinternals by monitoring registry writes from `svchost.exe` while changing values in the legacy Control Panel.

Key findings:

* The physical power button GUID is `7648efa3-dd9c-4e3e-b566-50f929386280` — not `a7066653` which is the Start Menu power icon, a completely separate setting
* Both the power button and lid close action use the same index mapping: `0=Do nothing, 1=Sleep, 2=Hibernate, 3=Shut down`
* These settings must be **read** via `PowerReadACValueIndex` / `PowerReadDCValueIndex` (direct registry reads fail with access denied on this path even when elevated)
* These settings must be **written** via both the Power API and direct registry for compatibility with both legacy and modern Windows power management layers

\---

## Requirements

* Windows 10 or Windows 11
* Modern Standby (S0) system (the app works on S3 systems too but those systems already show the hibernate timer natively)
* Administrator rights (the app will prompt for elevation automatically on launch)

\---

## Installation

No installer required. Download `HibernateTimer.exe` from the [Releases](../../releases) page and run it. Windows will prompt for administrator permission on first launch.

\---

## Building from source

1. Install [Visual Studio Community](https://visualstudio.microsoft.com/vs/community/) (free) with the **Desktop development with C++** workload
2. Open `HibernateTimer.cpp` in a new Windows Desktop Application project
3. Build in Release x64 configuration

The app uses only Win32 API and standard Windows libraries — no external dependencies, no frameworks, no runtime requirements beyond what ships with Windows.

\---

## Code signing

This application is signed through [SignPath Foundation](https://signpath.org) which provides free code signing for open source projects. This means Windows will show a verified publisher name instead of "Unknown Publisher" in the UAC prompt.

\---

## License

MIT License — see [LICENSE](LICENSE) for details.

\---

## Frequently Asked Questions

**My hibernate timer is set but the laptop never hibernates**
The most common cause is that your sleep timer is set shorter than your hibernate timer. On S0 Modern Standby, sleep and hibernate race each other — whichever is shorter fires first. If sleep fires first the system goes to sleep and hibernate never activates. Set hibernate shorter than sleep.

**The app says hibernate is disabled**
Click the "Enable hibernate" button in the app. It runs `powercfg /hibernate on` automatically. The app will reload and the warning banner will disappear once hibernate is enabled.

**Will this break my power settings?**
No. The app only saves values you explicitly change — it tracks what was loaded at startup and only writes settings that differ. If you open the app and immediately hit Save without changing anything it will tell you there are no changes to save.

**Why does the app ask for administrator permission?**
Writing power settings requires administrator rights. The app requests elevation automatically on launch — you don't need to right-click and run as administrator manually.

**Does this work on desktop computers?**
The app is primarily designed for laptops with Modern Standby (S0). Desktop computers rarely use Modern Standby and typically don't have the hibernate timer hidden. The app will still run on a desktop but the lid close section won't be relevant.

**Why doesn't powercfg show the hibernate timer?**
It does — but only via command line. Run `powercfg /query SCHEME\_CURRENT SUB\_SLEEP HIBERNATEIDLE` in an admin command prompt to see the raw value. This app just exposes that value in a GUI so you don't have to use the command line.

**The UAC prompt says "Unknown Publisher"**
The app is not yet code signed. It is safe to run — the source code is fully available on this page for inspection. Code signing via SignPath is planned for an upcoming release which will replace "Unknown Publisher" with a verified publisher name.

\---



* [Microsoft Sysinternals Process Monitor](https://learn.microsoft.com/en-us/sysinternals/downloads/procmon) — essential tool for discovering the actual registry paths Windows uses for these settings
* [SignPath Foundation](https://signpath.org) — free code signing for open source projects
* Developed by DanVaun ([https://github.com/DanVaun](https://github.com/DanVaun))

