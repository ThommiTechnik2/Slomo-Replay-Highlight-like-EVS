#!/bin/bash
# Doppelklick-Start: 6 REC-Wege per SDI (Blackmagic DeckLink, REC 1 = Anschluss 0 … REC 6 = Anschluss 5)
export EVS_INPUT=sdi
exec "$(dirname "$(readlink -f "$0")")/evs-start.sh" 6
