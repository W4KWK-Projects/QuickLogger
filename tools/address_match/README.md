# Address-match spike

Measures how many FCC and ISED amateur licensees can be placed at an address point, for a 6-character grid. A standalone script, not part of the build. Python 3 and `unzip` (the NAD zip is Deflate64, which Python's zipfile can't read).

```
python3 spike.py us l_amat.zip nad.zip [STATE ...]
python3 spike.py ca amateur_delim.zip nar.zip [PROV ...]
```

Inputs: the FCC's `l_amat.zip`, ISED's `amateur_delim.zip`, the DOT National Address Database text zip, and the StatCan National Address Register zip (`https://www150.statcan.gc.ca/n1/pub/46-26-0002/2022001/202606.zip` for the June 2026 release).

A licensee is matched, in order, by:

| Method | Key |
|---|---|
| strict | postal code + house number + street, normalized (types, directions and ordinals abbreviated, unit dropped) |
| loose | the same, street words in any order |
| core | the street without its type, particles and end directions, hyphens joined, numbered roads by number ("CR 466" = "COUNTY RD 466") |
| pn1 | the only street with that house number in that postal code |
| interp | no point for the number, but the same street has numbers above and below it: placed in between, same side of the street |

PO boxes, rural routes and blank addresses aren't matched; they keep the ZIP or postal code centroid. Interpolation is checked on licensees whose point is known, by placing them from their neighbors and measuring the error.

## Results (2026-10-02)

FCC data of 2026-09-27, ISED of 2026-10-01, NAD release 24, NAR June 2026. The US run takes about 47 minutes, Canada's about 3.

| | Licensees | Matched or interpolated | Of those with a street address |
|---|---|---|---|
| US, the 24 states where NAD covers at least 95% of licensees' ZIPs | 315,366 | 85.7% | 91.8% |
| US, the 29 states at 90% or more | 448,011 | 81.1% | 87.1% |
| US, every state with any NAD points | 769,088 | 57.1% | 61.5% |
| Canada, all provinces and territories | 90,109 | 58.2% | 87.1% |

US states with no NAD points: HI, MI, MS, NH, NV, PR. Partly covered (share of licensees in a ZIP with points): AK 69%, CA 24%, CO 87%, FL 11%, GA 12%, ID 22%, KS 78%, LA 20%, MN 89%, MO 61%, NE 57%, OK 82%, PA 23%, SC 16%, SD 32%, WI 82%, WY 54%.

Canada's shortfall is ISED's own data: 21% of licensees have a blank address and 9% a PO box or rural route.

Interpolation, re-placing known points from their neighbors on the same side of the street: US median error 10 m, 90% within 120 m, 98.1% in the same 6-character square; Canada 99.3%. It places about 6,000 more US licensees and 400 Canadian ones.
