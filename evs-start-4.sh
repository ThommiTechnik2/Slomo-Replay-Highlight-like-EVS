#!/bin/bash
# Doppelklick-Start mit 4 REC-Wegen
exec "$(dirname "$(readlink -f "$0")")/evs-start.sh" 4
