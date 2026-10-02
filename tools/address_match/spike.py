"""Phase 0 spike: how many FCC/ISED licensees match an address point.

US:     l_amat.zip (EN.dat, HD.dat) against the NAD text zip.
Canada: amateur_delim.zip against the NAR zip (Address + Location CSVs).

Key = postal code + house number + normalized street. "strict" keeps the
street's word order; "loose" sorts its words (catches "RUE PRINCIPALE" vs
"PRINCIPALE RUE" and dropped/extra directionals are still a miss).

Usage:
  spike.py us  l_amat.zip nad.zip  [STATE ...]
  spike.py ca  amateur_delim.zip nar.zip [PROV ...]
"""

import csv
import io
import re
import subprocess
import sys
import unicodedata
import zipfile
from collections import defaultdict

csv.field_size_limit(1 << 24)

SUFFIXES = {
    "STREET": "ST", "STR": "ST", "AVENUE": "AVE", "AV": "AVE", "AVN": "AVE", "ROAD": "RD", "DRIVE": "DR",
    "DRV": "DR", "LANE": "LN", "COURT": "CT", "CIRCLE": "CIR", "CIRC": "CIR", "BOULEVARD": "BLVD",
    "BOUL": "BLVD", "BLV": "BLVD", "PLACE": "PL", "TERRACE": "TER", "TERR": "TER", "HIGHWAY": "HWY",
    "HIWAY": "HWY", "PARKWAY": "PKWY", "PKY": "PKWY", "TRAIL": "TRL", "TR": "TRL", "WAY": "WAY",
    "PIKE": "PIKE", "SQUARE": "SQ", "LOOP": "LOOP", "POINT": "PT", "CROSSING": "XING", "COVE": "CV",
    "RIDGE": "RDG", "RUN": "RUN", "PATH": "PATH", "HILL": "HL", "HOLLOW": "HOLW", "VIEW": "VW",
    "VALLEY": "VLY", "CREEK": "CRK", "ESTATES": "ESTS", "HEIGHTS": "HTS", "MOUNT": "MT", "MOUNTAIN": "MTN",
    "EXPRESSWAY": "EXPY", "FREEWAY": "FWY", "ROUTE": "RTE", "CRESCENT": "CRES", "CRES": "CRES",
    "GARDENS": "GDNS", "GROVE": "GRV", "MEADOWS": "MDWS", "CLOSE": "CLOSE", "GATE": "GATE",
    # French (Canada)
    "CHEMIN": "CH", "RANG": "RANG", "MONTEE": "MONTEE", "AVENUE.": "AVE", "BOULEVARD.": "BLVD",
}
DIRECTIONS = {
    "NORTH": "N", "SOUTH": "S", "EAST": "E", "WEST": "W", "NORTHEAST": "NE", "NORTHWEST": "NW",
    "SOUTHEAST": "SE", "SOUTHWEST": "SW", "NORD": "N", "SUD": "S", "EST": "E", "OUEST": "W", "O": "W",
}
ORDINALS = {"FIRST": "1ST", "SECOND": "2ND", "THIRD": "3RD", "FOURTH": "4TH", "FIFTH": "5TH", "SIXTH": "6TH",
            "SEVENTH": "7TH", "EIGHTH": "8TH", "NINTH": "9TH", "TENTH": "10TH"}
# Words after which the rest is a unit, not the street.
UNIT_WORDS = {"APT", "APARTMENT", "UNIT", "STE", "SUITE", "LOT", "TRLR", "SPC", "SPACE", "RM", "ROOM", "BLDG",
              "FL", "FLOOR", "BOX", "#"}
PO_BOX = re.compile(r"\b(P\s*O|POST\s+OFFICE|POSTAL)\s*BOX\b|^\s*BOX\b|\bPO\s*BOX\b|\bC\.?\s*P\.?\s+\d|\bCASE\s+POSTALE\b"
                    r"|\b(RR|R\.R\.|RURAL\s+ROUTE|HC|STAR\s+ROUTE|PSC|CMR|GENERAL\s+DELIVERY)\b")


def Clean(text):
    # Accents off (NAR and ISED disagree on them).
    text = unicodedata.normalize("NFKD", text)
    text = "".join(c for c in text if not unicodedata.combining(c))
    text = text.upper().replace("&", " AND ").replace("'", " ")
    text = re.sub(r"[.,]", "", text)
    text = re.sub(r"[^A-Z0-9# -]", " ", text)
    return text.split()


def NormalizeWords(words):
    out = []
    for w in words:
        if w in UNIT_WORDS or w.startswith("#"):
            break
        w = SUFFIXES.get(w, w)
        w = DIRECTIONS.get(w, w)
        w = ORDINALS.get(w, w)
        out.append(w)
    return out


def IsPoBox(line):
    return PO_BOX.search(re.sub(r"[.#]", "", line.upper())) is not None


def SplitStreetLine(line):
    """'123B N Main St Apt 4' -> ('123', ['N', 'MAIN', 'ST']); None if no house number.
    A Canadian unit first ('#310-33333 12th Ave', '402 - 7031 Blundell Rd') gives the civic number."""
    line = re.sub(r"^\s*#?\s*[A-Za-z0-9]+\s*-\s*(?=\d)", "", line)
    words = Clean(line)
    if not words:
        return None
    m = re.match(r"^(\d+)", words[0])
    if not m:
        return None
    number = m.group(1)
    rest = words[1:]
    # "123 1/2 Main": drop the fraction.
    if rest and re.match(r"^\d+/\d+$", rest[0]):
        rest = rest[1:]
    # A lone letter right after the number is its suffix (123 B MAIN), unless it's a direction.
    if rest and len(rest[0]) == 1 and rest[0] not in ("N", "S", "E", "W", "O") and len(rest) > 1:
        rest = rest[1:]
    street = NormalizeWords(rest)
    if not street:
        return None
    return number, street


# Street types as written before (French) or after (English) the name; any of these at either end of
# a street is dropped for the core key, as are particles anywhere and a direction at either end.
TYPES = set(SUFFIXES.values()) | {"RUE", "CH", "RANG", "MONTEE", "BOUL", "BLVD", "CRES", "TSSE", "PROM", "CRT",
                                  "IMP", "ALLEE", "COTE", "RTE", "AV", "TERR", "CIRCLE", "CIR", "RD", "ST"}
PARTICLES = {"DE", "DU", "DES", "LA", "LE", "LES", "D", "L", "AU", "AUX", "THE", "OF"}
DIRS = set(DIRECTIONS.values()) | {"N", "S", "E", "W", "NE", "NW", "SE", "SW"}
SAINTS = {"SAINT": "ST", "SAINTE": "STE"}


def Ordinal(word):
    """'112IEME', '112E', '112TH', '1ER' -> '112' / '1'."""
    m = re.match(r"^(\d+)(ST|ND|RD|TH|E|EME|IEME|ER|RE|IER|IERE)$", word)
    return m.group(1) if m else word


# Numbered roads, written many ways: "CR 466" is "COUNTY RD 466", "FM 1954" is "FARM TO MARKET 1954",
# "HWY 82" is "UNITED STATES HWY 82". The road's kind is dropped, keeping its number.
ROAD_ALIASES = [
    (re.compile(r"\b(COUNTY (RD|HWY|RTE)|CO (RD|HWY)|CNTY RD|CR)\b"), "CR"),
    (re.compile(r"\b(FARM TO MARKET( RD)?|FARM RD|FM RD|RANCH TO MARKET( RD)?|RANCH RD|RM)\b"), "FM"),
    (re.compile(r"\b(UNITED STATES|US|U S|STATE|ST|INTERSTATE)? ?(HWY|RTE)\b"), "HWY"),
    (re.compile(r"\b(STATE RD|SR|SH|STATE ROUTE)\b"), "HWY"),
    (re.compile(r"\b(PRIVATE RD|PVT RD|PR)\b"), "PR"),
]
ROAD_KINDS = {"CR", "FM", "HWY", "PR"}


def Core(street):
    joined = " ".join(street)
    for pattern, alias in ROAD_ALIASES:
        joined = pattern.sub(alias, joined)
    street = joined.split()
    if any(w.isdigit() for w in street) and any(w in ROAD_KINDS for w in street):
        street = [w for w in street if w not in ROAD_KINDS]
    words = []
    for w in street:
        words.extend(p for p in w.split("-") if p)
    words = [Ordinal(SAINTS.get(w, w)) for w in words]
    # Ends only: "ST" in front is a type in French ("RUE ...") but a saint in "ST LAURENT".
    while len(words) > 1 and (words[-1] in TYPES or words[-1] in DIRS):
        words.pop()
    while len(words) > 1 and (words[0] in TYPES - {"ST"} or words[0] in DIRS):
        words.pop(0)
    core = [w for w in words if w not in PARTICLES]
    return "".join(core) if core else "".join(words)


def Keys(postal, number, street):
    strict = postal + "|" + number + "|" + " ".join(street)
    loose = postal + "|" + number + "|" + " ".join(sorted(set(street)))
    return strict, loose


def CoreKey(postal, number, street):
    return postal + "|" + number + "|" + Core(street)


# ---- Licensees ---------------------------------------------------------------

def LoadUls(path, states):
    """Active amateur licensees with an address: {callsign: (state, zip5, street_line)}.

    EN.dat joins HD.dat on the unique system id (field 1), as the app does."""
    active = {}
    with zipfile.ZipFile(path) as z:
        with z.open("HD.dat") as f:
            for raw in io.TextIOWrapper(f, encoding="latin-1", newline=""):
                p = raw.rstrip("\r\n").split("|")
                if len(p) > 5 and p[0] == "HD" and p[5] == "A" and p[4]:
                    active[p[1]] = p[4].upper()
        out = {}
        with z.open("EN.dat") as f:
            for raw in io.TextIOWrapper(f, encoding="latin-1", newline=""):
                p = raw.rstrip("\r\n").split("|")
                if len(p) < 19 or p[0] != "EN" or p[1] not in active:
                    continue
                state, zip5 = p[17].strip().upper(), p[18].strip()[:5]
                if states and state not in states:
                    continue
                out[active[p[1]]] = (state, zip5, p[15].strip())
    return out


def LoadIsed(path, provinces):
    """Fields by position, ';'-separated, as uls_import.cpp reads them:
    0 callsign, 3 address, 5 province, 6 postal code."""
    out = {}
    with zipfile.ZipFile(path) as z:
        name = [n for n in z.namelist() if n.lower().startswith("amateur") and n.lower().endswith(".txt")][0]
        with z.open(name) as f:
            for raw in io.TextIOWrapper(f, encoding="utf-8", errors="replace", newline=""):
                row = raw.rstrip("\r\n").split(";")
                if len(row) < 7 or not row[0] or row[0].strip().lower() == "callsign":
                    continue
                prov = row[5].strip().upper()
                if provinces and prov not in provinces:
                    continue
                out[row[0].strip().upper()] = (prov, row[6].replace(" ", "").upper(), row[3].strip())
    return out


# ---- Address points ------------------------------------------------------------

def Pick(col, *names):
    for n in names:
        if n.lower() in col:
            return col[n.lower()]
    return None


# NAR's PROV_CODE is the SGC code.
PROVINCES = {"10": "NL", "11": "PE", "12": "NS", "13": "NB", "24": "QC", "35": "ON", "46": "MB", "47": "SK",
             "48": "AB", "59": "BC", "60": "YT", "61": "NT", "62": "NU"}


def StreamNad(path, wanted_postals):
    """Yields (state, zip5, None, ...) once per point, for counting, then for one in a wanted ZIP
    (state, zip5, number, street_words, lat, lon)."""
    # The NAD zip is Deflate64 (zipfile can't read it), so it's streamed through unzip.
    with zipfile.ZipFile(path) as z:
        name = max(z.infolist(), key=lambda i: i.file_size).filename
    unzip = subprocess.Popen(["unzip", "-p", path, name], stdout=subprocess.PIPE, bufsize=1 << 20)
    with unzip.stdout as f:
        if True:
            reader = csv.reader(io.TextIOWrapper(f, encoding="utf-8-sig", errors="replace", newline=""))
            header = [h.strip().lower() for h in next(reader)]
            col = {h: i for i, h in enumerate(header)}
            i_state = Pick(col, "State")
            i_zip = Pick(col, "Zip_Code", "Post_Code")
            i_num = Pick(col, "Add_Number")
            i_full = Pick(col, "StNam_Full")
            parts = [Pick(col, n) for n in ("St_PreMod", "St_PreDir", "St_PreTyp", "St_PreSep", "St_Name",
                                              "St_PosTyp", "St_PosDir", "St_PosMod")]
            i_lat, i_lon = Pick(col, "Latitude"), Pick(col, "Longitude")
            print("NAD file %s, columns: %s" % (name, header), file=sys.stderr)
            for row in reader:
                try:
                    zip5 = row[i_zip].strip()[:5]
                except IndexError:
                    continue
                yield row[i_state].strip().upper(), zip5, None, None, None, None
                if zip5 not in wanted_postals:
                    continue
                number = re.sub(r"\D.*$", "", row[i_num].strip())
                if i_full is not None and row[i_full].strip():
                    words = Clean(row[i_full])
                else:
                    words = Clean(" ".join(row[i] for i in parts if i is not None))
                yield (row[i_state].strip().upper(), zip5, number, NormalizeWords(words), row[i_lat], row[i_lon])


def StreamNar(path, wanted_postals):
    """Yields (prov, postal, number, street_words, lat, lon) from the NAR's Address/Location CSVs."""
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        locations = {}
        # Pass 1: addresses we care about, by location id.
        want = {}
        for name in names:
            if "address" not in name.lower() or not name.lower().endswith(".csv"):
                continue
            with z.open(name) as f:
                reader = csv.reader(io.TextIOWrapper(f, encoding="utf-8-sig", errors="replace", newline=""))
                col = {h.strip().lower(): i for i, h in enumerate(next(reader))}
                i_loc, i_num = col["loc_guid"], col["civic_no"]
                i_post = Pick(col, "MAIL_POSTAL_CODE", "POSTAL_CODE")
                i_prov = Pick(col, "PROV_CODE", "MAIL_PROV_ABVN")
                street_cols = [Pick(col, "OFFICIAL_STREET_NAME"), Pick(col, "OFFICIAL_STREET_TYPE"),
                               Pick(col, "OFFICIAL_STREET_DIR")]
                mail_cols = [Pick(col, "MAIL_STREET_NAME"), Pick(col, "MAIL_STREET_TYPE"), Pick(col, "MAIL_STREET_DIR")]
                for row in reader:
                    postal = row[i_post].replace(" ", "").upper()
                    prov = PROVINCES.get(row[i_prov].strip(), row[i_prov].strip()) if i_prov is not None else ""
                    yield prov, postal, None, None, None, None
                    if postal not in wanted_postals:
                        continue
                    for cols in (street_cols, mail_cols):
                        words = NormalizeWords(Clean(" ".join(row[i] for i in cols if i is not None)))
                        if words:
                            want.setdefault(row[i_loc], []).append((prov, postal, row[i_num].strip(), words))
        # Pass 2: their locations.
        for name in names:
            if "location" not in name.lower() or not name.lower().endswith(".csv"):
                continue
            with z.open(name) as f:
                reader = csv.reader(io.TextIOWrapper(f, encoding="utf-8-sig", errors="replace", newline=""))
                col = {h.strip().lower(): i for i, h in enumerate(next(reader))}
                i_loc = col["loc_guid"]
                # The building's point, else the blockface's.
                pairs = [(Pick(col, a), Pick(col, b)) for a, b in (("BG_LATITUDE", "BG_LONGITUDE"),
                                                                   ("BF_REPPOINT_LATITUDE", "BF_REPPOINT_LONGITUDE"),
                                                                   ("REPPOINT_LATITUDE", "REPPOINT_LONGITUDE"))]
                pairs = [(a, b) for a, b in pairs if a is not None and b is not None]
                for row in reader:
                    found = want.get(row[i_loc])
                    if not found:
                        continue
                    lat = lon = ""
                    for a, b in pairs:
                        if row[a].strip() and row[b].strip():
                            lat, lon = row[a], row[b]
                            break
                    for prov, postal, number, words in found:
                        yield prov, postal, number, words, lat, lon


# ---- The match -------------------------------------------------------------------

def Grid6(lat, lon):
    lon += 180.0
    lat += 90.0
    a = chr(ord("A") + int(lon / 20)) + chr(ord("A") + int(lat / 10))
    b = str(int(lon % 20 / 2)) + str(int(lat % 10))
    c = chr(ord("a") + int(lon % 2 * 12)) + chr(ord("a") + int(lat % 1 * 24))
    return a + b + c


def Km(lat1, lon1, lat2, lon2):
    import math
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 12742.0 * math.asin(min(1.0, math.sqrt(h)))


def Interpolate(n, low, high):
    lo, lat1, lon1 = low
    hi, lat2, lon2 = high
    t = (n - lo) / float(hi - lo)
    return lat1 + t * (lat2 - lat1), lon1 + t * (lon2 - lon1)


def Percentile(values, q):
    if not values:
        return 0.0
    values = sorted(values)
    return values[min(len(values) - 1, int(q * len(values)))]


def Run(licensees, stream_points, label):
    by_strict = defaultdict(list)
    by_loose = defaultdict(list)
    by_core = defaultdict(list)
    by_pn = defaultdict(list)
    # For interpolation: licensees by postal code + core street, with their house numbers.
    by_street = defaultdict(list)
    house = {}
    stats = defaultdict(lambda: defaultdict(int))
    postals = set()
    for callsign, (region, postal, line) in licensees.items():
        s = stats[region]
        s["licensees"] += 1
        if not line:
            s["blank"] += 1
            continue
        if IsPoBox(line):
            s["po_box"] += 1
            continue
        split = SplitStreetLine(line)
        if split is None or not postal:
            s["no_street"] += 1
            continue
        s["street"] += 1
        strict, loose = Keys(postal, split[0], split[1])
        by_strict[strict].append(callsign)
        by_loose[loose].append(callsign)
        by_core[CoreKey(postal, split[0], split[1])].append(callsign)
        by_pn[postal + "|" + split[0]].append(callsign)
        by_street[postal + "|" + Core(split[1])].append(callsign)
        house[callsign] = int(split[0])
        postals.add(postal)

    matched_strict, matched_loose, matched_core, no_coordinate = set(), set(), set(), set()
    streets_at_pn = defaultdict(set)
    exact_at = {}  # callsign -> (lat, lon) of its matched point
    # Nearest numbers below and above each licensee's, on its street: (number, lat, lon).
    # "any" takes either side of the street, "same" only the same parity (side).
    low = {"any": {}, "same": {}}
    high = {"any": {}, "same": {}}
    points_by_region = defaultdict(int)
    covered_postals = set()
    for region, postal, number, words, lat, lon in stream_points(postals):
        if words is None:
            points_by_region[region] += 1
            if postal in postals:
                covered_postals.add(postal)
            continue
        strict, loose = Keys(postal, number, words)
        if not (lat or "").strip() or not (lon or "").strip():
            no_coordinate.update(by_loose.get(loose, ()))
            continue
        matched_strict.update(by_strict.get(strict, ()))
        matched_loose.update(by_loose.get(loose, ()))
        core = CoreKey(postal, number, words)
        matched_core.update(by_core.get(core, ()))
        try:
            point = (int(number), float(lat), float(lon))
        except ValueError:
            continue
        for callsign in by_loose.get(loose, []) + by_core.get(core, []):
            exact_at.setdefault(callsign, point[1:])
        for callsign in by_street.get(postal + "|" + Core(words), ()):
            n = house[callsign]
            for side in ("any", "same"):
                if side == "same" and (n - point[0]) % 2:
                    continue
                if point[0] < n and (callsign not in low[side] or point[0] > low[side][callsign][0]):
                    low[side][callsign] = point
                elif point[0] > n and (callsign not in high[side] or point[0] < high[side][callsign][0]):
                    high[side][callsign] = point
        pn = postal + "|" + number
        if pn in by_pn:
            streets_at_pn[pn].add(core)

    # Only one street with that number at that postal code (a Canadian postal code is a block or so).
    matched_pn = set()
    for pn, streets in streets_at_pn.items():
        if len(streets) == 1:
            matched_pn.update(by_pn[pn])
    # Interpolated: no exact point, but numbers on both sides on the same street. Checked first on the
    # licensees whose exact point is known, ignoring it, to see how good a guess it is.
    exact_any = matched_strict | matched_loose | matched_core | matched_pn
    interpolated = set()
    for side in ("any", "same"):
        errors, gaps, same_grid, tested = [], [], 0, 0
        for callsign, at in exact_at.items():
            if callsign in low[side] and callsign in high[side]:
                lat, lon = Interpolate(house[callsign], low[side][callsign], high[side][callsign])
                errors.append(Km(lat, lon, at[0], at[1]))
                same_grid += Grid6(lat, lon) == Grid6(at[0], at[1])
                tested += 1
        for callsign in low[side]:
            if callsign in high[side] and callsign not in exact_any:
                l, h = low[side][callsign], high[side][callsign]
                gaps.append(Km(l[1], l[2], h[1], h[2]))
                if side == "same":
                    interpolated.add(callsign)
        if tested:
            print("Interpolation check (%s side): %d known points re-guessed from neighbors; error median %.2f km, "
                  "90%% under %.2f km; same 6-char grid %.1f%%" % (
                      side, tested, Percentile(errors, 0.5), Percentile(errors, 0.9), 100.0 * same_grid / tested))
        print("  licensees it would place (%s side): %d; neighbor gap median %.2f km, 90%% under %.2f km" % (
            side, len(gaps), Percentile(gaps, 0.5), Percentile(gaps, 0.9)))
    for callsign, (region, postal, line) in licensees.items():
        s = stats[region]
        if callsign in exact_any or callsign in interpolated:
            s["interp"] += 1
        if postal in covered_postals:
            s["in_covered_postal"] += 1
        if callsign in matched_strict:
            s["strict"] += 1
        if callsign in matched_loose or callsign in matched_strict:
            s["loose"] += 1
        if callsign in matched_loose or callsign in matched_strict or callsign in matched_core:
            s["core"] += 1
        if callsign in matched_loose or callsign in matched_strict or callsign in matched_core or \
                callsign in matched_pn:
            s["pn1"] += 1

    print("\n%s  (points: address points in the file for that region)" % label)
    print("%-6s %10s %9s %7s %8s %9s %9s %8s %8s %8s %8s %8s %8s" % (
        "region", "points", "licensees", "blank", "po_box", "street", "in_cov", "strict%", "loose%", "core%",
        "+pn1%", "+intp%", "cov%"))
    totals = defaultdict(int)
    for region in sorted(stats):
        s = stats[region]
        n = s["licensees"]
        cov = s["in_covered_postal"]
        print("%-6s %10d %9d %7d %8d %9d %9d %7.1f%% %7.1f%% %7.1f%% %7.1f%% %7.1f%% %7.1f%%" % (
            region, points_by_region.get(region, 0), n, s["blank"], s["po_box"], s["street"], cov,
            100.0 * s["strict"] / n if n else 0, 100.0 * s["loose"] / n if n else 0,
            100.0 * s["core"] / n if n else 0, 100.0 * s["pn1"] / n if n else 0,
            100.0 * s["interp"] / n if n else 0, 100.0 * s["interp"] / cov if cov else 0))
        if points_by_region.get(region, 0):
            for k in ("licensees", "blank", "po_box", "street", "strict", "loose", "core", "pn1", "interp",
                      "in_covered_postal"):
                totals[k] += s[k]
    print("Matched only an address point with no coordinate: %d" % len(no_coordinate - matched_loose))
    n = totals["licensees"]
    if n:
        st = totals["street"] or 1
        print("Regions with address points: %d licensees (%d blank, %d PO box/rural, %d street)" % (
            n, totals["blank"], totals["po_box"], totals["street"]))
        for k in ("strict", "loose", "core", "pn1", "interp"):
            print("  %-6s %5.1f%% of all, %5.1f%% of street addresses" % (
                k, 100.0 * totals[k] / n, 100.0 * totals[k] / st))
    return matched_strict, matched_loose


def main():
    kind, licensee_path, points_path = sys.argv[1:4]
    regions = set(a.upper() for a in sys.argv[4:])
    if kind == "us":
        licensees = LoadUls(licensee_path, regions)
        Run(licensees, lambda postals: StreamNad(points_path, postals), "US (NAD)")
    else:
        licensees = LoadIsed(licensee_path, regions)
        Run(licensees, lambda postals: StreamNar(points_path, postals), "Canada (NAR)")


if __name__ == "__main__":
    main()
