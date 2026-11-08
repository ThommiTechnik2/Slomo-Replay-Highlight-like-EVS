#!/bin/bash
# Doppelklick-Start mit 6 REC-Wegen
exec "$(dirname "$(readlink -f "$0")")/evs-start.sh" 6
