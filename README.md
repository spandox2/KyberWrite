# KyberWrite — Flipper Zero FAP

Program EM4x05 RFID tubes to act as Disney Galaxy's Edge kyber crystals,
using a Flipper Zero running **Unleashed firmware**.

Converted from the original Proxmark3 Lua script by Spandox.

---

## Requirements

| Requirement | Notes |
|---|---|
| Flipper Zero | Any hardware revision |
| Unleashed firmware | ≥ 0.9x (needs `lfrfid_raw_write_em4x05_word`) |
| EM4x05 tube blank | FDX-B animal chips work; the script re-formats them |
| `fbt` build toolchain | From the Unleashed repo |

The stock Flipper firmware does **not** expose EM4x05 block-write capability.
You must be on Unleashed (or RogueMaster, which forks Unleashed).

---

## Building
The `kyberwrite.fap` in the repo root is built for Unleashed `unlshd-094`
(API 88.19). Copy it to `/ext/apps/RFID/kyberwrite.fap`, or compile it yourself.

```bash
# Clone Unleashed firmware
git clone https://github.com/DarkFlippers/unleashed-firmware
cd unleashed-firmware

# Drop this folder into applications_user/
cp -r /path/to/kyberwrite applications_user/kyberwrite

# Build 
./fbt fap_kyberwrite

# build will be located at
unleashed-firmware/build/f7-firmware-D/.extapps/kyberwrite.fap

# DEPLOY (install)
I use [Flipper Lab](https://lab.flipper.net/archive)
Copy the compiled `.fap` to your Flipper at
/ext/apps/RFID/kyberwrite.fap
```

---

## Usage

**Write Crystal** always writes the full kyber image, on a blank or a
crystal that already has data. The password block (address 2) is left
alone. Address 5, address 6, and address 9 come from the color and voice
you pick. Pick a color, then a voice. Series 2 voices and the series 1
voices a series 2 holocron speaks are in the same list. Picking a series 1
voice writes zeros to address 9, which is what makes that holocron fall
back to the series 1 voice.

The write screen stays on "Waiting for crystal" until a tag answers. It
then programs the chip and reads the EM4100 ID back. Crystal Ready is shown
only when that ID matches. Press Back to cancel. Address 9 is written in
the same pass. That check cannot see address 9, so use Read Crystal to
confirm the series 2 voice.

### Flow

```
Main Menu
  ├─ Read Crystal
  └─ Write Crystal ─▶ Color
                        │
                        ▼
                  Holocron voice
                        │
                        ▼
                Waits for a crystal
                        │
                        ▼
                Writes, then reads back
                     │            │
                   Match      No match
                     │            └──▶ stays and tries again
                     ▼
               Crystal Ready
                     │
                     ▼
                 Main Menu
```

### Physical placement

Hold the EM4x05 tube flat against the back of the Flipper, centered over
the RFID coil (the coil is the teardrop-shaped antenna trace near the top).
Keep it still during the write sequence — moving it mid-write will cause a
block-write failure.

---

## Crystal reference

| Code       | Color   | Sith Voice   | Jedi Voice      | Jedi (in Sith mode) |
|------------|---------|--------------|-----------------|---------------------|
| 18003000   | White   | Palpatine    | Ahsoka Tano     | Old Luke            |
| 5E003000   | Red     | Darth Vader  | Yoda            | Qui-Gon, Yoda       |
| 3D003000   | Orange  | —            | —               |                     |
| 7B003000   | Yellow  | Palpatine    | Temple Guard    | Old Luke            |
| 0C803000   | Green   | Palpatine    | Qui-Gon Jinn    | Old Luke, Mace      |
| 4A803000   | Cyan    | —            | —               |                     |
| 29803000   | Blue    | Palpatine    | Old Obi-Wan     | Old Luke            |
| 6F803000   | Purple  | Palpatine    | Mace Windu 1    | Old Luke            |
| 14403000   | White   | Palpatine    | Chirrut Imwe    | Old Luke            |
| 52403000   | Red     | Palpatine    | Yoda            | Young Obi-Wan       |
| 31403000   | Red     | Count Dooku  | Yoda            | Yoda, Old Obi-Wan   |
| 77403000   | Yellow  | Palpatine    | Maz Kanata      | Old Luke            |
| 00C03000   | Green   | Palpatine    | Yoda            | Old Luke            |
| 46C03000   | Red     | Darth Maul   | Yoda            | Bendu, sound fx     |
| 25C03000   | Blue    | Palpatine    | Old Luke        | Old Luke            |
| 63C03000   | Purple  | Palpatine    | Mace Windu 2    | Old Luke, Mace      |
| 3E183000   | Red     | Vader 8-Ball | Yoda            | Mace Windu          |
| 5D183000   | Green   | Palpatine    | Yoda 8-Ball     | Old Luke            |
| 1B183000   | Black   | Snoke        | Yoda            | Old Obi-Wan         |

### Series 2

Released May 4, 2024. The same address 6 value is the series 1 voice a
series 1 holocron plays. Address 9 is what a series 2 holocron, and a
wayfinder, use. Address 5 is `18C631FF` on every series 2 crystal, so the
tag broadcasts `1111000Cxx` instead of the series 1 `0000000Cxx`.

| Address 9 | Address 6 | Color | Series 2 voice | Series 1 voice | Wayfinder |
|-----------|-----------|-------|----------------|----------------|-----------|
| 010D0000 | 5E003000 | Black | Darth Sidious | Darth Vader | Exegol |
| 020D0000 | 29803000 | Blue, red core | Ben Solo | Old Ben Kenobi | Kef Bir |
| 030D0000 | 29803000 | Blue | Cal Kestis | Old Ben Kenobi | Bracca |
| 040D0000 | 29803000 | Blue | Plo Koon | Old Ben Kenobi | Florrum |
| 050D0000 | 0C803000 | Green | Luke Skywalker | Qui-Gon Jinn | Tatooine |
| 060D0000 | 6F803000 | Purple | General Grievous | Mace Windu | Vassek 3 |
| 070D0000 | 6F803000 | Purple | Mace Windu | Mace Windu | Bardotta |
| 080D0000 | 5E003000 | Red | Asajj Ventress | Darth Vader | Dathomir |
| 090D0000 | 5E003000 | Red, blue core | Kylo Ren | Darth Vader | Crait |
| 100D0000 | 5E003000 | Red | Darth Bane | Darth Vader | Moraband |
| 110D0000 | 5E003000 | Red | Maul | Darth Vader | Mandalore |
| 120D0000 | 5E003000 | Red | Second Sister | Darth Vader | Nur |
| 130D0000 | 5E003000 | Red | Grand Inquisitor | Darth Vader | Stygeon Prime |
| 140D0000 | 18003000 | White | Krin Dagbard | Ahsoka Tano | Batuu |
| 150D0000 | 18003000 | White | Orla Jareni | Ahsoka Tano | Umbara |
| 160D0000 | 7B003000 | Yellow | Indeera Stokes | Temple Guard | Elphrona |
| 170D0000 | 7B003000 | Yellow | Rey Skywalker | Temple Guard | Tatooine |
| 180D0000 | 0C803000 | Green | Luminara Unduli | Qui-Gon Jinn | Naboo |

Crystals that share a color share an EM4100 ID. Read Crystal shows the color,
the series 1 holocron voice, and the series 2 holocron voice. Address 9 is
not in the broadcast. As soon as that ID is recognized, and before the reader
turns the field off, the app does what a wayfinder does: Read Word for
address 6, then Read Word for address 9. Hold still the whole time. The word
read starts the moment the crystal is recognized and keeps going until Back.
Color and the series 1 voice appear from the broadcast. Address 9 is read
when that broadcast is audible, and the series 2 line fills in when the word
comes back. A different crystal replaces the picture. Silence returns the
screen to Hold still. Series 1 is shown on both lines only when address 9
reads as zero. A missed reply leaves the series 2 line as `...`. A short
spike does not end the frame, and the reply is accepted anywhere in the
listen. The backlight stays on for the whole read and returns to its normal timeout
when you leave. The splash and the main menu show the app version.

A valid address 9 value looks like `xx0D0000`. Anything in the last four
digits makes a series 2 holocron ignore the crystal. A non-zero middle digit
(`xxyD0000`) makes it fall back to address 6.

Data sourced from the community spreadsheet originally linked in the Proxmark script:
https://docs.google.com/spreadsheets/d/13P_GE6tNYpGvoVUTEQvA3SQzMqpZ-SoiWaTNoJoTV9Q

---

## Notes & caveats

- **Password block (addr 2) is intentionally skipped.** The Proxmark script
  skips it too. If you need to change the password you'll need to extend the
  write mask and add the password word.
- **The write does not report success by itself.** `em4305_write` never
  checks that a chip is in the field. The app waits for a read, writes, then
  reads the EM4100 ID back. A mismatch stays on the write screen.
- **Words are written in address order.** The firmware writer walks the mask
  from address 0 upward. The password word stays masked off.
- Orange (3D003000) and Cyan (4A803000) are listed as having no associated
  voice lines in the community data.
- **Series 2 needs address 9.** Handheld EM4100 writers only touch the
  broadcast ID. They leave address 9 alone, and often rewrite the config
  word so a series 2 holocron can no longer read it. Every write restores
  the whole image, including the `0001805F` config in address 4, and skips
  only the password.

---

## License

Do what you want with it. Based on public community research into Galaxy's
Edge lightsaber electronics. No affiliation with Disney.
