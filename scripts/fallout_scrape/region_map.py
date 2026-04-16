"""Region normalisation and adjacency for Fallout location navigation.

Maps raw wiki "part of" strings → canonical region IDs (uint8, 0-62).
Defines the adjacency graph used for travel filtering on device.
Imported by gen_rpg_packed.py; also generates module-src/FRPGRegions.h.
"""
from __future__ import annotations

# ── Region ID constants ────────────────────────────────────────────────────────
REGION_UNKNOWN   = 0

# FO1 New California (1-7)
FO1_VAULT13      = 1
FO1_SHADY_SANDS  = 2
FO1_JUNKTOWN     = 3
FO1_HUB          = 4
FO1_NECROPOLIS   = 5
FO1_BONEYARD     = 6
FO1_GENERAL      = 7   # catch-all for unspecified New California

# FO2 Northern California (8-19)
FO2_ARROYO       = 8
FO2_KLAMATH      = 9
FO2_DEN          = 10
FO2_VAULT_CITY   = 11
FO2_BROKEN_HILLS = 12
FO2_NEW_RENO     = 13
FO2_REDDING      = 14
FO2_NCR          = 15
FO2_SF           = 16
FO2_MODOC        = 17
FO2_NAVARRO      = 18
FO2_GENERAL      = 19  # catch-all

# FO3 Capital Wasteland (20-32)
FO3_MEGATON      = 20
FO3_AREFU        = 21
FO3_BIG_TOWN     = 22
FO3_DC_RUINS     = 23
FO3_RIVET_CITY   = 24
FO3_TENPENNY     = 25
FO3_UNDERWORLD   = 26
FO3_GRAYDITCH    = 27
FO3_GENERAL      = 28  # catch-all Capital Wasteland
FO3_POINT_LOOKOUT = 29  # DLC, isolated
FO3_THE_PITT     = 30  # DLC, isolated
FO3_ANCHORAGE    = 31  # DLC simulation, isolated
FO3_MOTHERSHIP   = 32  # DLC in space, isolated

# NV Mojave Wasteland (33-45)
NV_GOODSPRINGS   = 33
NV_NOVAC         = 34
NV_NEW_VEGAS     = 35
NV_HOOVER_DAM    = 36
NV_RED_ROCK      = 37
NV_LAKE_MEAD     = 38
NV_BOULDER_CITY  = 39
NV_GENERAL       = 40  # catch-all Mojave
NV_ZION          = 41  # DLC Honest Hearts, isolated
NV_BIG_MT        = 42  # DLC Old World Blues, isolated
NV_DIVIDE        = 43  # DLC Lonesome Road, isolated
NV_DEAD_MONEY    = 44  # DLC Dead Money / Sierra Madre, isolated
NV_FORT          = 45  # Caesar's Fort / Arizona territory

# FO4 The Commonwealth (46-62)
FO4_CONCORD      = 46
FO4_LEXINGTON    = 47
FO4_CAMBRIDGE    = 48
FO4_FENS         = 49  # Diamond City area
FO4_SOUTH_BOSTON = 50
FO4_FINANCIAL    = 51  # Financial District / downtown
FO4_NORTH_END    = 52
FO4_THEATER      = 53  # Theater District
FO4_ESPLANADE    = 54
FO4_MALDEN       = 55
FO4_QUINCY       = 56
FO4_NATICK       = 57
FO4_NAHANT       = 58
FO4_GLOWING_SEA  = 59
FO4_INSTITUTE    = 60  # Underground, reachable from Commonwealth
FO4_FAR_HARBOR   = 61  # DLC The Island, isolated
FO4_NUKA_WORLD   = 62  # DLC, isolated
FO4_GENERAL      = 63  # catch-all Commonwealth

REGION_COUNT = 64

# ── Human-readable names (displayed in game) ──────────────────────────────────
REGION_NAMES: dict[int, str] = {
    REGION_UNKNOWN:   "Wasteland",
    FO1_VAULT13:      "Vault 13",
    FO1_SHADY_SANDS:  "Shady Sands",
    FO1_JUNKTOWN:     "Junktown",
    FO1_HUB:          "The Hub",
    FO1_NECROPOLIS:   "Necropolis",
    FO1_BONEYARD:     "Boneyard",
    FO1_GENERAL:      "New California",
    FO2_ARROYO:       "Arroyo",
    FO2_KLAMATH:      "Klamath",
    FO2_DEN:          "The Den",
    FO2_VAULT_CITY:   "Vault City",
    FO2_BROKEN_HILLS: "Broken Hills",
    FO2_NEW_RENO:     "New Reno",
    FO2_REDDING:      "Redding",
    FO2_NCR:          "NCR",
    FO2_SF:           "San Francisco",
    FO2_MODOC:        "Modoc",
    FO2_NAVARRO:      "Navarro",
    FO2_GENERAL:      "N. California",
    FO3_MEGATON:      "Megaton Area",
    FO3_AREFU:        "Arefu",
    FO3_BIG_TOWN:     "Big Town",
    FO3_DC_RUINS:     "DC Ruins",
    FO3_RIVET_CITY:   "Rivet City",
    FO3_TENPENNY:     "Tenpenny Area",
    FO3_UNDERWORLD:   "Underworld",
    FO3_GRAYDITCH:    "Grayditch",
    FO3_GENERAL:      "Capital Wstlnd",
    FO3_POINT_LOOKOUT:"Point Lookout",
    FO3_THE_PITT:     "The Pitt",
    FO3_ANCHORAGE:    "Anchorage",
    FO3_MOTHERSHIP:   "Mothership Zeta",
    NV_GOODSPRINGS:   "Goodsprings",
    NV_NOVAC:         "Novac",
    NV_NEW_VEGAS:     "New Vegas",
    NV_HOOVER_DAM:    "Hoover Dam",
    NV_RED_ROCK:      "Red Rock Cyn",
    NV_LAKE_MEAD:     "Lake Mead",
    NV_BOULDER_CITY:  "Boulder City",
    NV_GENERAL:       "Mojave Wstlnd",
    NV_ZION:          "Zion Canyon",
    NV_BIG_MT:        "Big MT",
    NV_DIVIDE:        "The Divide",
    NV_DEAD_MONEY:    "Sierra Madre",
    NV_FORT:          "The Fort",
    FO4_CONCORD:      "Concord",
    FO4_LEXINGTON:    "Lexington",
    FO4_CAMBRIDGE:    "Cambridge",
    FO4_FENS:         "The Fens",
    FO4_SOUTH_BOSTON: "South Boston",
    FO4_FINANCIAL:    "Financial Dist",
    FO4_NORTH_END:    "North End",
    FO4_THEATER:      "Theater Dist",
    FO4_ESPLANADE:    "Esplanade",
    FO4_MALDEN:       "Malden",
    FO4_QUINCY:       "Quincy",
    FO4_NATICK:       "Natick",
    FO4_NAHANT:       "Nahant",
    FO4_GLOWING_SEA:  "Glowing Sea",
    FO4_INSTITUTE:    "The Institute",
    FO4_FAR_HARBOR:   "Far Harbor",
    FO4_NUKA_WORLD:   "Nuka-World",
    FO4_GENERAL:      "Commonwealth",
}

# ── Adjacency graph ────────────────────────────────────────────────────────────
# Symmetric. DLC-isolated regions have empty sets.
# "General" catch-all for each game is adjacent to all named regions in that game.
ADJACENCY: dict[int, set[int]] = {
    # FO1
    FO1_VAULT13:      {FO1_GENERAL, FO1_SHADY_SANDS},
    FO1_SHADY_SANDS:  {FO1_VAULT13, FO1_GENERAL, FO1_HUB, FO1_JUNKTOWN},
    FO1_JUNKTOWN:     {FO1_SHADY_SANDS, FO1_HUB, FO1_GENERAL},
    FO1_HUB:          {FO1_JUNKTOWN, FO1_SHADY_SANDS, FO1_NECROPOLIS, FO1_BONEYARD, FO1_GENERAL},
    FO1_NECROPOLIS:   {FO1_HUB, FO1_GENERAL},
    FO1_BONEYARD:     {FO1_HUB, FO1_GENERAL},
    FO1_GENERAL:      {FO1_VAULT13, FO1_SHADY_SANDS, FO1_JUNKTOWN, FO1_HUB,
                       FO1_NECROPOLIS, FO1_BONEYARD},
    # FO2
    FO2_ARROYO:       {FO2_KLAMATH, FO2_GENERAL},
    FO2_KLAMATH:      {FO2_ARROYO, FO2_DEN, FO2_MODOC, FO2_GENERAL},
    FO2_DEN:          {FO2_KLAMATH, FO2_VAULT_CITY, FO2_GENERAL},
    FO2_VAULT_CITY:   {FO2_DEN, FO2_BROKEN_HILLS, FO2_NCR, FO2_GENERAL},
    FO2_BROKEN_HILLS: {FO2_VAULT_CITY, FO2_NEW_RENO, FO2_REDDING, FO2_GENERAL},
    FO2_NEW_RENO:     {FO2_BROKEN_HILLS, FO2_REDDING, FO2_NCR, FO2_GENERAL},
    FO2_REDDING:      {FO2_BROKEN_HILLS, FO2_NEW_RENO, FO2_GENERAL},
    FO2_NCR:          {FO2_VAULT_CITY, FO2_NEW_RENO, FO2_SF, FO2_GENERAL},
    FO2_SF:           {FO2_NCR, FO2_NAVARRO, FO2_GENERAL},
    FO2_MODOC:        {FO2_KLAMATH, FO2_GENERAL},
    FO2_NAVARRO:      {FO2_SF, FO2_GENERAL},
    FO2_GENERAL:      {FO2_ARROYO, FO2_KLAMATH, FO2_DEN, FO2_VAULT_CITY, FO2_BROKEN_HILLS,
                       FO2_NEW_RENO, FO2_REDDING, FO2_NCR, FO2_SF, FO2_MODOC, FO2_NAVARRO},
    # FO3 (DLC isolated)
    FO3_MEGATON:      {FO3_AREFU, FO3_BIG_TOWN, FO3_GENERAL, FO3_DC_RUINS},
    FO3_AREFU:        {FO3_MEGATON, FO3_GENERAL},
    FO3_BIG_TOWN:     {FO3_MEGATON, FO3_GENERAL, FO3_DC_RUINS},
    FO3_DC_RUINS:     {FO3_MEGATON, FO3_BIG_TOWN, FO3_RIVET_CITY, FO3_GRAYDITCH,
                       FO3_UNDERWORLD, FO3_GENERAL},
    FO3_RIVET_CITY:   {FO3_DC_RUINS, FO3_GENERAL, FO3_GRAYDITCH},
    FO3_TENPENNY:     {FO3_GENERAL},
    FO3_UNDERWORLD:   {FO3_DC_RUINS},
    FO3_GRAYDITCH:    {FO3_DC_RUINS, FO3_RIVET_CITY, FO3_GENERAL},
    FO3_GENERAL:      {FO3_MEGATON, FO3_TENPENNY, FO3_BIG_TOWN, FO3_DC_RUINS,
                       FO3_RIVET_CITY, FO3_GRAYDITCH, FO3_AREFU},
    FO3_POINT_LOOKOUT: set(),
    FO3_THE_PITT:     set(),
    FO3_ANCHORAGE:    set(),
    FO3_MOTHERSHIP:   set(),
    # NV (DLC isolated)
    NV_GOODSPRINGS:   {NV_NOVAC, NV_GENERAL},
    NV_NOVAC:         {NV_GOODSPRINGS, NV_BOULDER_CITY, NV_HOOVER_DAM, NV_GENERAL},
    NV_HOOVER_DAM:    {NV_NOVAC, NV_BOULDER_CITY, NV_FORT, NV_NEW_VEGAS, NV_LAKE_MEAD, NV_GENERAL},
    NV_BOULDER_CITY:  {NV_NOVAC, NV_HOOVER_DAM, NV_GENERAL},
    NV_NEW_VEGAS:     {NV_HOOVER_DAM, NV_RED_ROCK, NV_LAKE_MEAD, NV_GENERAL},
    NV_RED_ROCK:      {NV_NEW_VEGAS, NV_GENERAL},
    NV_LAKE_MEAD:     {NV_NEW_VEGAS, NV_HOOVER_DAM, NV_GENERAL},
    NV_FORT:          {NV_HOOVER_DAM, NV_GENERAL},
    NV_GENERAL:       {NV_GOODSPRINGS, NV_NOVAC, NV_HOOVER_DAM, NV_BOULDER_CITY,
                       NV_NEW_VEGAS, NV_RED_ROCK, NV_LAKE_MEAD, NV_FORT},
    NV_ZION:          set(),
    NV_BIG_MT:        set(),
    NV_DIVIDE:        set(),
    NV_DEAD_MONEY:    set(),
    # FO4 (DLC isolated)
    FO4_CONCORD:      {FO4_LEXINGTON, FO4_MALDEN, FO4_GENERAL},
    FO4_LEXINGTON:    {FO4_CONCORD, FO4_CAMBRIDGE, FO4_GENERAL},
    FO4_CAMBRIDGE:    {FO4_LEXINGTON, FO4_FENS, FO4_FINANCIAL, FO4_GENERAL},
    FO4_FENS:         {FO4_CAMBRIDGE, FO4_SOUTH_BOSTON, FO4_THEATER, FO4_GENERAL},
    FO4_SOUTH_BOSTON: {FO4_FENS, FO4_QUINCY, FO4_ESPLANADE, FO4_GENERAL},
    FO4_FINANCIAL:    {FO4_CAMBRIDGE, FO4_NORTH_END, FO4_THEATER, FO4_FENS, FO4_GENERAL},
    FO4_NORTH_END:    {FO4_FINANCIAL, FO4_ESPLANADE, FO4_GENERAL},
    FO4_THEATER:      {FO4_FINANCIAL, FO4_FENS, FO4_GENERAL},
    FO4_ESPLANADE:    {FO4_NORTH_END, FO4_SOUTH_BOSTON, FO4_NAHANT, FO4_GENERAL},
    FO4_MALDEN:       {FO4_CONCORD, FO4_GENERAL},
    FO4_QUINCY:       {FO4_SOUTH_BOSTON, FO4_NATICK, FO4_GENERAL},
    FO4_NATICK:       {FO4_QUINCY, FO4_GLOWING_SEA, FO4_GENERAL},
    FO4_NAHANT:       {FO4_ESPLANADE, FO4_GENERAL},
    FO4_GLOWING_SEA:  {FO4_NATICK, FO4_GENERAL},
    FO4_INSTITUTE:    {FO4_GENERAL},
    FO4_GENERAL:      {FO4_CONCORD, FO4_LEXINGTON, FO4_CAMBRIDGE, FO4_FENS,
                       FO4_SOUTH_BOSTON, FO4_FINANCIAL, FO4_NORTH_END, FO4_THEATER,
                       FO4_ESPLANADE, FO4_MALDEN, FO4_QUINCY, FO4_NATICK, FO4_NAHANT,
                       FO4_GLOWING_SEA, FO4_INSTITUTE},
    FO4_FAR_HARBOR:   set(),
    FO4_NUKA_WORLD:   set(),
}


def _adj_padded(region_id: int) -> list[int]:
    """Return adjacency list padded to 8 entries with 0xFF sentinel."""
    neighbors = sorted(ADJACENCY.get(region_id, set()))
    result = neighbors[:8]
    while len(result) < 8:
        result.append(0xFF)
    return result


# ── Normalisation: raw wiki string + game tag → region ID ─────────────────────
# Rules are checked in order; first match wins. Each entry is (substring, region_id).
# Game-specific rules listed per-game, most-specific first.

_FO1_RULES: list[tuple[str, int]] = [
    ("vault 13",     FO1_VAULT13),
    ("shady sands",  FO1_SHADY_SANDS),
    ("junktown",     FO1_JUNKTOWN),
    ("hub",          FO1_HUB),
    ("merchants",    FO1_HUB),
    ("necropolis",   FO1_NECROPOLIS),
    ("boneyard",     FO1_BONEYARD),
    ("cathedral",    FO1_BONEYARD),
    ("los angeles",  FO1_BONEYARD),
    ("hollywood",    FO1_BONEYARD),
]

_FO2_RULES: list[tuple[str, int]] = [
    ("arroyo",       FO2_ARROYO),
    ("klamath",      FO2_KLAMATH),
    ("the den",      FO2_DEN),
    ("den west",     FO2_DEN),
    ("vault city",   FO2_VAULT_CITY),
    ("gecko",        FO2_VAULT_CITY),   # adjacent to Vault City
    ("broken hills", FO2_BROKEN_HILLS),
    ("new reno",     FO2_NEW_RENO),
    ("second street",FO2_NEW_RENO),
    ("redding",      FO2_REDDING),
    ("ncr",          FO2_NCR),
    ("san francisco",FO2_SF),
    ("modoc",        FO2_MODOC),
    ("navarro",      FO2_NAVARRO),
]

_FO3_RULES: list[tuple[str, int]] = [
    ("point lookout",     FO3_POINT_LOOKOUT),
    ("the pitt",          FO3_THE_PITT),
    ("pitt",              FO3_THE_PITT),
    ("anchorage",         FO3_ANCHORAGE),
    ("alaska",            FO3_ANCHORAGE),
    ("mothership",        FO3_MOTHERSHIP),
    ("megaton",           FO3_MEGATON),
    ("springvale",        FO3_MEGATON),
    ("arefu",             FO3_AREFU),
    ("big town",          FO3_BIG_TOWN),
    ("minefield",         FO3_BIG_TOWN),
    ("galaxy news",       FO3_BIG_TOWN),
    ("washington",        FO3_DC_RUINS),
    ("the mall",          FO3_DC_RUINS),
    ("mall",              FO3_DC_RUINS),
    ("vernon square",     FO3_DC_RUINS),
    ("l'enfant",          FO3_DC_RUINS),
    ("georgetown",        FO3_DC_RUINS),
    ("museum",            FO3_DC_RUINS),
    ("rivet city",        FO3_RIVET_CITY),
    ("marketplace, rivet",FO3_RIVET_CITY),
    ("tenpenny",          FO3_TENPENNY),
    ("underworld",        FO3_UNDERWORLD),
    ("grayditch",         FO3_GRAYDITCH),
]

_NV_RULES: list[tuple[str, int]] = [
    ("zion",             NV_ZION),
    ("big mt",           NV_BIG_MT),
    ("the divide",       NV_DIVIDE),
    ("divide",           NV_DIVIDE),
    ("sierra madre",     NV_DEAD_MONEY),
    ("dead money",       NV_DEAD_MONEY),
    ("the fort",         NV_FORT),
    ("arizona",          NV_FORT),
    ("goodsprings",      NV_GOODSPRINGS),
    ("primm",            NV_GOODSPRINGS),
    ("novac",            NV_NOVAC),
    ("boulder city",     NV_BOULDER_CITY),
    ("hoover dam",       NV_HOOVER_DAM),
    ("nelson",           NV_HOOVER_DAM),
    ("new vegas strip",  NV_NEW_VEGAS),
    ("freeside",         NV_NEW_VEGAS),
    ("westside",         NV_NEW_VEGAS),
    ("new vegas sewers", NV_NEW_VEGAS),
    ("new vegas",        NV_NEW_VEGAS),
    ("aerotech",         NV_NEW_VEGAS),
    ("red rock",         NV_RED_ROCK),
    ("lake mead",        NV_LAKE_MEAD),
    ("camp golf",        NV_LAKE_MEAD),
    ("eastern virgin",   NV_ZION),
    ("the narrows",      NV_ZION),
]

_FO4_RULES: list[tuple[str, int]] = [
    ("the island",          FO4_FAR_HARBOR),
    ("far harbor",          FO4_FAR_HARBOR),
    ("nuka-world",          FO4_NUKA_WORLD),
    ("nuka world",          FO4_NUKA_WORLD),
    ("the institute",       FO4_INSTITUTE),
    ("glowing sea",         FO4_GLOWING_SEA),
    ("concord",             FO4_CONCORD),
    ("sanctuary",           FO4_CONCORD),
    ("lexington",           FO4_LEXINGTON),
    ("cambridge",           FO4_CAMBRIDGE),
    ("diamond city",        FO4_FENS),
    ("the fens",            FO4_FENS),
    ("fens",                FO4_FENS),
    ("south boston",        FO4_SOUTH_BOSTON),
    ("the castle",          FO4_SOUTH_BOSTON),
    ("castle",              FO4_SOUTH_BOSTON),
    ("quincy",              FO4_QUINCY),
    ("financial district",  FO4_FINANCIAL),
    ("north end",           FO4_NORTH_END),
    ("freedom trail",       FO4_NORTH_END),
    ("theater district",    FO4_THEATER),
    ("esplanade",           FO4_ESPLANADE),
    ("malden",              FO4_MALDEN),
    ("natick",              FO4_NATICK),
    ("nahant",              FO4_NAHANT),
]

_GAME_RULES: dict[str, list[tuple[str, int]]] = {
    "FO1": _FO1_RULES,
    "FO2": _FO2_RULES,
    "FO3": _FO3_RULES,
    "NV":  _NV_RULES,
    "FO4": _FO4_RULES,
}

_GAME_GENERAL: dict[str, int] = {
    "FO1": FO1_GENERAL,
    "FO2": FO2_GENERAL,
    "FO3": FO3_GENERAL,
    "NV":  NV_GENERAL,
    "FO4": FO4_GENERAL,  # = 63
}


def normalize_region(raw: str, game: str) -> int:
    """Map a raw wiki 'part of' string to a region ID.

    Falls back to the game's catch-all general region if no rule matches,
    or REGION_UNKNOWN if the game is unrecognised.
    """
    if not raw:
        return _GAME_GENERAL.get(game, REGION_UNKNOWN)
    low = raw.lower().strip()
    rules = _GAME_RULES.get(game, [])
    for keyword, region_id in rules:
        if keyword in low:
            return region_id
    return _GAME_GENERAL.get(game, REGION_UNKNOWN)


# ── C header generation ───────────────────────────────────────────────────────

def generate_frpg_regions_h() -> str:
    """Return the content of FRPGRegions.h as a string."""
    lines: list[str] = []
    lines.append("#pragma once")
    lines.append("// FRPGRegions.h — Region IDs and adjacency table for location navigation.")
    lines.append("// AUTO-GENERATED by gen_rpg_packed.py via region_map.py. Do NOT edit manually.")
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("")

    # Constants
    lines.append("// ── Region ID constants ──────────────────────────────────────────────────────")
    lines.append(f"#define REGION_COUNT  {REGION_COUNT}")
    lines.append(f"#define REGION_UNKNOWN  {REGION_UNKNOWN}")
    lines.append("")

    groups = [
        ("FO1", [(FO1_VAULT13,"FO1_VAULT13"), (FO1_SHADY_SANDS,"FO1_SHADY_SANDS"),
                 (FO1_JUNKTOWN,"FO1_JUNKTOWN"), (FO1_HUB,"FO1_HUB"),
                 (FO1_NECROPOLIS,"FO1_NECROPOLIS"), (FO1_BONEYARD,"FO1_BONEYARD"),
                 (FO1_GENERAL,"FO1_GENERAL")]),
        ("FO2", [(FO2_ARROYO,"FO2_ARROYO"), (FO2_KLAMATH,"FO2_KLAMATH"),
                 (FO2_DEN,"FO2_DEN"), (FO2_VAULT_CITY,"FO2_VAULT_CITY"),
                 (FO2_BROKEN_HILLS,"FO2_BROKEN_HILLS"), (FO2_NEW_RENO,"FO2_NEW_RENO"),
                 (FO2_REDDING,"FO2_REDDING"), (FO2_NCR,"FO2_NCR"),
                 (FO2_SF,"FO2_SF"), (FO2_MODOC,"FO2_MODOC"),
                 (FO2_NAVARRO,"FO2_NAVARRO"), (FO2_GENERAL,"FO2_GENERAL")]),
        ("FO3", [(FO3_MEGATON,"FO3_MEGATON"), (FO3_AREFU,"FO3_AREFU"),
                 (FO3_BIG_TOWN,"FO3_BIG_TOWN"), (FO3_DC_RUINS,"FO3_DC_RUINS"),
                 (FO3_RIVET_CITY,"FO3_RIVET_CITY"), (FO3_TENPENNY,"FO3_TENPENNY"),
                 (FO3_UNDERWORLD,"FO3_UNDERWORLD"), (FO3_GRAYDITCH,"FO3_GRAYDITCH"),
                 (FO3_GENERAL,"FO3_GENERAL"), (FO3_POINT_LOOKOUT,"FO3_POINT_LOOKOUT"),
                 (FO3_THE_PITT,"FO3_THE_PITT"), (FO3_ANCHORAGE,"FO3_ANCHORAGE"),
                 (FO3_MOTHERSHIP,"FO3_MOTHERSHIP")]),
        ("NV",  [(NV_GOODSPRINGS,"NV_GOODSPRINGS"), (NV_NOVAC,"NV_NOVAC"),
                 (NV_NEW_VEGAS,"NV_NEW_VEGAS"), (NV_HOOVER_DAM,"NV_HOOVER_DAM"),
                 (NV_RED_ROCK,"NV_RED_ROCK"), (NV_LAKE_MEAD,"NV_LAKE_MEAD"),
                 (NV_BOULDER_CITY,"NV_BOULDER_CITY"), (NV_GENERAL,"NV_GENERAL"),
                 (NV_ZION,"NV_ZION"), (NV_BIG_MT,"NV_BIG_MT"),
                 (NV_DIVIDE,"NV_DIVIDE"), (NV_DEAD_MONEY,"NV_DEAD_MONEY"),
                 (NV_FORT,"NV_FORT")]),
        ("FO4", [(FO4_CONCORD,"FO4_CONCORD"), (FO4_LEXINGTON,"FO4_LEXINGTON"),
                 (FO4_CAMBRIDGE,"FO4_CAMBRIDGE"), (FO4_FENS,"FO4_FENS"),
                 (FO4_SOUTH_BOSTON,"FO4_SOUTH_BOSTON"), (FO4_FINANCIAL,"FO4_FINANCIAL"),
                 (FO4_NORTH_END,"FO4_NORTH_END"), (FO4_THEATER,"FO4_THEATER"),
                 (FO4_ESPLANADE,"FO4_ESPLANADE"), (FO4_MALDEN,"FO4_MALDEN"),
                 (FO4_QUINCY,"FO4_QUINCY"), (FO4_NATICK,"FO4_NATICK"),
                 (FO4_NAHANT,"FO4_NAHANT"), (FO4_GLOWING_SEA,"FO4_GLOWING_SEA"),
                 (FO4_INSTITUTE,"FO4_INSTITUTE"), (FO4_FAR_HARBOR,"FO4_FAR_HARBOR"),
                 (FO4_NUKA_WORLD,"FO4_NUKA_WORLD"), (FO4_GENERAL,"FO4_GENERAL")]),
    ]

    for game, entries in groups:
        lines.append(f"// {game}")
        for val, name in entries:
            lines.append(f"#define {name:<22} {val}")
        lines.append("")

    # Region names table
    lines.append("// ── Region display names ─────────────────────────────────────────────────────")
    lines.append("static const char * const FRPG_REGION_NAMES[REGION_COUNT] = {")
    for i in range(REGION_COUNT):
        name = REGION_NAMES.get(i, "Wasteland")
        lines.append(f'    "{name}",  // {i}')
    lines.append("};")
    lines.append("")

    # Adjacency table
    lines.append("// ── Adjacency table (up to 8 neighbors, 0xFF = end sentinel) ─────────────────")
    lines.append("static const uint8_t FRPG_REGION_ADJ[REGION_COUNT][8] = {")
    for i in range(REGION_COUNT):
        adj = _adj_padded(i)
        name = REGION_NAMES.get(i, "?")
        adj_str = ", ".join(f"0x{v:02X}" for v in adj)
        lines.append(f"    {{{adj_str}}},  // {i} {name}")
    lines.append("};")
    lines.append("")

    # Helper functions
    lines.append("// ── Helpers ──────────────────────────────────────────────────────────────────")
    lines.append("inline bool frpgRegionsAdjacent(uint8_t a, uint8_t b) {")
    lines.append("    if (a == REGION_UNKNOWN || b == REGION_UNKNOWN) return false;")
    lines.append("    if (a == b) return true;")
    lines.append("    if (a >= REGION_COUNT || b >= REGION_COUNT) return false;")
    lines.append("    for (int i = 0; i < 8; i++) {")
    lines.append("        if (FRPG_REGION_ADJ[a][i] == 0xFF) break;")
    lines.append("        if (FRPG_REGION_ADJ[a][i] == b) return true;")
    lines.append("    }")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    lines.append("inline const char* frpgRegionName(uint8_t regionId) {")
    lines.append("    if (regionId >= REGION_COUNT) return \"Wasteland\";")
    lines.append("    return FRPG_REGION_NAMES[regionId];")
    lines.append("}")
    lines.append("")

    return "\n".join(lines)
