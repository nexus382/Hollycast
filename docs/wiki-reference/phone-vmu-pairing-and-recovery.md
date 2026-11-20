<!-- Portions Copyright 2026 The Hollycast Authors -->

# Phone VMU Pairing and Card Management

Phone pairing links survive Hollycast restarts, with an option to reset the pairing address. The update adds saved mini-game settings, VMU backup restore, sync-conflict choices, and card-file import, export, and delete actions.

## Pair again, back up and manage a card

1. Reopen your saved Phone VMU address after restarting Hollycast and enabling **Connect Phone**. Use **New pairing address** in Phone VMU settings if you need a new QR code and want to disconnect the old pairing.
2. To manage card files, start the card's mini-game and open **Controls → Card files**. Check the free blocks, then choose the file you want to export or import.
3. If Hollycast reports a sync conflict, check both copies before choosing **Keep my phone copy** or **Use Hollycast copy**. Use **Download VMU copy** first if you want to keep a separate copy of browser progress.
4. To restore the most recent phone backup, stop emulation, then choose the applicable **Restore VMU … phone backup** action in the Phone VMU settings.

[Phone VMU guide](../Phone%20VMU.md)

[Source pull request #83](https://github.com/nexus382/Hollycast/pull/83)
