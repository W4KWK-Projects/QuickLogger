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
import sys
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
    text = text.upper().replace("&", " AND ")
    # Accents off (NAR and ISED disagree on them).
    for a, b in (("É", "E"), ("È", "E"), ("Ê", "E"), ("Ë", "E"), ("À", "A"), ("Â", "A"), ("Ç", "C"),
                 ("Î", "I"), ("Ï", "I"), ("Ô", "O"), ("Û", "U"), ("Ù", "U"), ("Ü", "U")):
        text = text.replace(a, b)
    text = re.sub(r"[.,']", "", text)
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


def SplitStreetLine(line):
    """'123B N Main St Apt 4' -> ('123', ['N', 'MAIN', 'ST']); None if no house number."""
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


def Keys(postal, number, street):
    strict = postal + "|" + number + "|" + " ".join(street)
    loose = postal + "|" + number + "|" + " ".join(sorted(set(street)))
    return strict, loose


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
        name = [n for n in z.namelist() if n.lower().endswith(".txt")][0]
        with z.open(name) as f:
            for raw in io.TextIOWrapper(f, encoding="latin-1", newline=""):
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
    with zipfile.ZipFile(path) as z:
        name = max(z.infolist(), key=lambda i: i.file_size).filename
        with z.open(name) as f:
            reader = csv.reader(io.TextIOWrapper(f, encoding="utf-8", errors="replace", newline=""))
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
                i_lat = Pick(col, "REPPOINT_LATITUDE", "BG_LATITUDE")
                i_lon = Pick(col, "REPPOINT_LONGITUDE", "BG_LONGITUDE")
                for row in reader:
                    for prov, postal, number, words in want.get(row[i_loc], ()):
                        yield prov, postal, number, words, row[i_lat], row[i_lon]


# ---- The match -------------------------------------------------------------------

def Run(licensees, stream_points, label):
    by_strict = defaultdict(list)
    by_loose = defaultdict(list)
    stats = defaultdict(lambda: defaultdict(int))
    postals = set()
    for callsign, (region, postal, line) in licensees.items():
        s = stats[region]
        s["licensees"] += 1
        if PO_BOX.search(line.upper()):
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
        postals.add(postal)

    matched_strict, matched_loose = set(), set()
    points_by_region = defaultdict(int)
    covered_postals = set()
    for region, postal, number, words, lat, lon in stream_points(postals):
        if words is None:
            points_by_region[region] += 1
            if postal in postals:
                covered_postals.add(postal)
            continue
        strict, loose = Keys(postal, number, words)
        matched_strict.update(by_strict.get(strict, ()))
        matched_loose.update(by_loose.get(loose, ()))

    for callsign, (region, postal, line) in licensees.items():
        s = stats[region]
        if postal in covered_postals:
            s["in_covered_postal"] += 1
        if callsign in matched_strict:
            s["strict"] += 1
        if callsign in matched_loose or callsign in matched_strict:
            s["loose"] += 1

    print("\n%s  (points: address points in the file for that region)" % label)
    print("%-6s %10s %9s %8s %9s %9s %8s %8s %8s" % ("region", "points", "licensees", "po_box", "street",
                                                      "in_cov", "strict%", "loose%", "cov%"))
    totals = defaultdict(int)
    for region in sorted(stats):
        s = stats[region]
        n = s["licensees"]
        cov = s["in_covered_postal"]
        print("%-6s %10d %9d %8d %9d %9d %7.1f%% %7.1f%% %7.1f%%" % (
            region, points_by_region.get(region, 0), n, s["po_box"], s["street"], cov,
            100.0 * s["strict"] / n if n else 0, 100.0 * s["loose"] / n if n else 0,
            100.0 * s["loose"] / cov if cov else 0))
        if points_by_region.get(region, 0):
            for k in ("licensees", "strict", "loose", "in_covered_postal"):
                totals[k] += s[k]
    n = totals["licensees"]
    if n:
        print("Regions with address points: %d licensees, strict %.1f%%, loose %.1f%%; "
              "%.1f%% of those in a postal code with points" % (
                  n, 100.0 * totals["strict"] / n, 100.0 * totals["loose"] / n,
                  100.0 * totals["loose"] / totals["in_covered_postal"] if totals["in_covered_postal"] else 0))
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
