import argparse
from contextlib import closing
import hashlib
import html
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
import secrets
import sqlite3
from urllib.parse import parse_qs, urlsplit
from uuid import UUID

SCHEMA = """
CREATE TABLE IF NOT EXISTS players (
    id INTEGER PRIMARY KEY,
    label TEXT NOT NULL UNIQUE,
    token_hash BLOB NOT NULL UNIQUE
);
CREATE TABLE IF NOT EXISTS runs (
    player_id INTEGER NOT NULL REFERENCES players(id),
    id TEXT NOT NULL,
    event_hash TEXT NOT NULL,
    mode TEXT NOT NULL,
    laps INTEGER NOT NULL,
    reversed INTEGER NOT NULL,
    time_ms INTEGER NOT NULL,
    profile TEXT NOT NULL,
    event_id TEXT NOT NULL,
    event_name TEXT NOT NULL,
    car TEXT NOT NULL DEFAULT '',
    received_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    PRIMARY KEY (player_id, id)
);
CREATE INDEX IF NOT EXISTS run_event ON runs(event_hash, mode, laps, reversed, player_id, time_ms);
"""
REQUIRED_FIELDS = ("event_hash", "mode", "laps", "reversed", "time_ms", "profile", "event_id", "event_name")
FIELDS = REQUIRED_FIELDS + ("car", "speed_milli_kmh")
HASH = re.compile(r"[a-f0-9]{8}")
TOKEN = re.compile(r"Bearer ([a-f0-9]{64})")
MODES = ("Sprint", "Circuit", "Drag", "Lap Knockout", "Tollbooth", "Speedtrap")
LAP_MODES = ("Circuit", "Lap Knockout")
MAX_BODY = 4096
CSS = """
:root{color-scheme:dark;font:16px/1.55 'Segoe UI',system-ui,sans-serif;background:#151615;color:#f0f0e9}
*{box-sizing:border-box} body{max-width:1050px;margin:auto;padding:3rem 1.25rem}
h1{font-size:clamp(2.6rem,8vw,5rem);line-height:1;letter-spacing:-.06em;margin:.3rem 0 1.3rem}
h2{letter-spacing:-.03em;margin:2rem 0 .65rem} .game{color:#eaac53;font-weight:700;letter-spacing:.15em;text-transform:uppercase}
p{color:#bbbdb4} form{display:flex;align-items:start;gap:.65rem;flex-wrap:wrap;margin:1.5rem 0}
select,button{font:inherit;padding:.65rem;border-radius:.3rem;border:1px solid #55574c;background:#23251f;color:inherit;max-width:100%}
button{background:#ecae56;color:#181815;border-color:#ecae56;font-weight:700;cursor:pointer}
a{color:#f5c381} .results{overflow-x:auto} table{width:100%;border-collapse:collapse;text-align:left}
th,td{padding:.75rem;border-bottom:1px solid #3c3e35;white-space:nowrap} th{color:#d5d6ce}
td.time{font-weight:700;font-variant-numeric:tabular-nums;color:#ffbd64}
"""


def connect(path):
    db = sqlite3.connect(path, timeout=8)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys = ON")
    return db


def initialize(path):
    with closing(connect(path)) as db:
        db.execute("PRAGMA journal_mode = WAL")
        db.executescript(SCHEMA)
        db.execute("BEGIN IMMEDIATE")
        with db:
            columns = {row["name"] for row in db.execute("PRAGMA table_info(runs)")}
            if "car" not in columns:
                db.execute("ALTER TABLE runs ADD COLUMN car TEXT NOT NULL DEFAULT ''")
            if "speed_milli_kmh" not in columns:
                db.execute("ALTER TABLE runs ADD COLUMN speed_milli_kmh INTEGER")


def validate(data):
    if type(data) is not dict:
        raise ValueError("Expected a race result object.")
    keys = set(data)
    required = set(REQUIRED_FIELDS) | {"id"}
    if not required <= keys <= required | {"car", "speed_milli_kmh"}:
        raise ValueError("Unexpected result fields.")
    data.setdefault("car", "")
    data.setdefault("speed_milli_kmh", None)
    speed = data["speed_milli_kmh"]
    if speed is not None and (data.get("mode") != "Speedtrap"
                             or type(speed) is not int or not 0 <= speed <= 1000000000):
        raise ValueError("Invalid accumulated speed.")
    if type(data["id"]) is not str:
        raise ValueError("Invalid run ID.")
    data["id"] = str(UUID(data["id"]))
    if type(data["event_hash"]) is not str or not HASH.fullmatch(data["event_hash"]):
        raise ValueError("Invalid event hash.")
    if data["event_hash"] == "00000000" or data["mode"] not in MODES:
        raise ValueError("Unsupported event.")
    if type(data["reversed"]) is not bool:
        raise ValueError("Invalid direction.")
    laps = data["laps"]
    if type(laps) is not int or not 1 <= laps <= 99 or (data["mode"] not in LAP_MODES and laps != 1):
        raise ValueError("Invalid lap count.")
    if type(data["time_ms"]) is not int or not 1000 <= data["time_ms"] <= 21600000:
        raise ValueError("Invalid finish time.")
    for field, limit in (("profile", 32), ("event_id", 48), ("event_name", 96), ("car", 64)):
        value = data[field]
        if type(value) is not str or len(value) > limit:
            raise ValueError(f"Invalid {field}.")
        if any(not char.isprintable() for char in value):
            raise ValueError(f"Invalid characters in {field}.")
        if field in ("profile", "event_id") and not value.strip():
            raise ValueError(f"Missing {field}.")
    return data


def add_run(db, player_id, data):
    if data["mode"] == "Speedtrap" and data["speed_milli_kmh"] is None:
        return HTTPStatus.OK
    values = tuple(data[field] for field in FIELDS)
    keys = (player_id, data["id"])
    with db:
        inserted = db.execute(
            """INSERT INTO runs (player_id,id,event_hash,mode,laps,reversed,time_ms,profile,event_id,event_name,car,speed_milli_kmh)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(player_id,id) DO NOTHING""",
            keys + values,
        )
        if inserted.rowcount:
            return HTTPStatus.CREATED
        saved = db.execute(
            "SELECT event_hash,mode,laps,reversed,time_ms,profile,event_id,event_name,car,speed_milli_kmh "
            "FROM runs WHERE player_id=? AND id=?", keys
        ).fetchone()
        return HTTPStatus.OK if tuple(saved) == values else HTTPStatus.CONFLICT


def event_key(row):
    return f'{row["event_hash"]}.{row["mode"]}.{row["laps"]}.{row["reversed"]}'


def get_board(db, selection):
    events = db.execute("""
        SELECT event_hash, mode, laps, reversed,
               COALESCE(MIN(NULLIF(event_name, '')), MIN(event_id)) AS title
        FROM runs WHERE mode <> 'Speedtrap' OR speed_milli_kmh IS NOT NULL
        GROUP BY event_hash, mode, laps, reversed
        ORDER BY title, event_hash, mode, laps, reversed
    """).fetchall()
    chosen = next((row for row in events if event_key(row) == selection), None)
    if chosen is None and events and not selection:
        chosen = events[0]
    if chosen is None:
        return events, None, []
    rows = db.execute("""
        WITH best AS (
            SELECT profile, car, time_ms, received_at, player_id, speed_milli_kmh,
                   CASE WHEN mode = 'Speedtrap' THEN -speed_milli_kmh ELSE time_ms END AS score,
                   ROW_NUMBER() OVER (
                       PARTITION BY player_id
                       ORDER BY CASE WHEN mode = 'Speedtrap' THEN -speed_milli_kmh ELSE time_ms END,
                                (car = ''), received_at, id
                   ) AS personal_rank
            FROM runs WHERE event_hash=? AND mode=? AND laps=? AND reversed=?
                AND (mode <> 'Speedtrap' OR speed_milli_kmh IS NOT NULL)
        )
        SELECT profile, car, time_ms, received_at, speed_milli_kmh,
               RANK() OVER (ORDER BY score) AS place
        FROM best WHERE personal_rank=1
        ORDER BY score, received_at, player_id LIMIT 100
    """, tuple(chosen[column] for column in ("event_hash", "mode", "laps", "reversed"))).fetchall()
    return events, chosen, rows


def event_title(row):
    unit = "lap" if row["laps"] == 1 else "laps"
    laps = f' · {row["laps"]} {unit}' if row["mode"] in LAP_MODES else ""
    direction = " · Reverse" if row["reversed"] else ""
    return f'{row["title"]} · {row["mode"]}{laps}{direction} ({row["event_hash"]})'


def race_time(milliseconds):
    minutes, rest = divmod(milliseconds, 60000)
    seconds, fraction = divmod(rest, 1000)
    return f"{minutes}:{seconds:02}.{fraction:03}"


def result_value(row):
    speed = row["speed_milli_kmh"]
    return f"{speed / 1000:.3f} km/h" if speed is not None else race_time(row["time_ms"])


def page(events, chosen, rows):
    escape = html.escape
    options = "".join(
        f'<option value="{event_key(row)}"'
        + (" selected" if chosen and event_key(chosen) == event_key(row) else "")
        + f'>{escape(event_title(row))}</option>'
        for row in events
    )
    picker = (
        '<form method="get"><select name="event" aria-label="Race event">'
        + options + '</select><button>View standings</button></form>'
        if options else ""
    )
    if chosen:
        entries = "".join(
            f'<tr><td>{row["place"]}</td><td>{escape(row["profile"])}</td>'
            f'<td>{escape(row["car"] or "—")}</td>'
            f'<td class="time">{result_value(row)}</td>'
            f'<td>{row["received_at"][:10]}</td></tr>' for row in rows
        )
        board = ('<div class="results"><table><thead><tr><th scope="col">Rank</th>'
                 '<th scope="col">Player profile</th><th scope="col">Car</th><th scope="col">Result</th>'
                 '<th scope="col">Submitted (UTC)</th></tr></thead><tbody>'
                 + entries + '</tbody></table></div>')
    else:
        board = '<p>No times for this event yet.</p>' if options else '<p>No race results yet.</p>'
    return ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            '<title>Most Wanted Leaderboard</title><style>' + CSS + '</style></head>'
            '<body><header><div class="game">Need for Speed · Most Wanted (2005)</div>'
            '<h1>Leaderboard</h1><p>Race times and accumulated Speedtrap scores.</p></header>'
            '<main>' + picker + board + '</main></body></html>')


class LeaderboardHandler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def respond(self, status, content, mime):
        self.send_response(status)
        self.send_header("Content-Type", mime + "; charset=utf-8")
        self.send_header("Content-Length", str(len(content)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; "
                         "form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
        self.end_headers()
        self.wfile.write(content)

    def json(self, status, data):
        self.respond(status, json.dumps(data, ensure_ascii=False).encode("utf-8"), "application/json")

    def do_GET(self):
        target = urlsplit(self.path)
        if target.path not in ("/", "/api/leaderboard"):
            self.json(HTTPStatus.NOT_FOUND, {"error": "Unknown route."})
            return
        selections = parse_qs(target.query).get("event", [])
        selection = selections[0] if selections else ""
        with closing(connect(self.server.database)) as db:
            events, chosen, entries = get_board(db, selection)
        if target.path == "/api/leaderboard":
            self.json(HTTPStatus.OK, {
                "events": [dict(row, key=event_key(row)) for row in events],
                "event": dict(chosen) if chosen else None,
                "entries": [dict(row) for row in entries],
            })
            return
        self.respond(HTTPStatus.OK, page(events, chosen, entries).encode("utf-8"), "text/html")

    def do_POST(self):
        if self.path != "/api/runs":
            self.json(HTTPStatus.NOT_FOUND, {"error": "Unknown route."})
            return
        authorization = TOKEN.fullmatch(self.headers.get("Authorization", ""))
        if not authorization:
            self.json(HTTPStatus.UNAUTHORIZED, {"error": "Player token required."})
            return
        if self.headers.get("Transfer-Encoding") or self.headers.get_content_type() != "application/json":
            self.json(HTTPStatus.BAD_REQUEST, {"error": "Send JSON with Content-Length."})
            return
        content_lengths = self.headers.get_all("Content-Length", [])
        if len(content_lengths) != 1 or not re.fullmatch(r"[0-9]{1,10}", content_lengths[0]):
            self.json(HTTPStatus.BAD_REQUEST, {"error": "Invalid Content-Length."})
            return
        length = int(content_lengths[0])
        if not 1 <= length <= MAX_BODY:
            self.json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"error": "Invalid body size."})
            return
        digest = hashlib.sha256(authorization[1].encode("ascii")).digest()
        with closing(connect(self.server.database)) as db:
            player = db.execute("SELECT id FROM players WHERE token_hash=?", (digest,)).fetchone()
            if player is None:
                self.json(HTTPStatus.UNAUTHORIZED, {"error": "Unknown player token."})
                return
            try:
                data = validate(json.loads(self.rfile.read(length).decode("utf-8")))
            except (ValueError, RecursionError):
                self.json(HTTPStatus.UNPROCESSABLE_ENTITY, {"error": "Invalid race result."})
                return
            try:
                status = add_run(db, player["id"], data)
            except sqlite3.OperationalError:
                self.json(HTTPStatus.SERVICE_UNAVAILABLE, {"error": "Database unavailable."})
                return
        message = {"id": data["id"]} if status != HTTPStatus.CONFLICT else {"error": "Run ID already used."}
        self.json(status, message)


def main():
    parser = argparse.ArgumentParser(description="Host the Most Wanted leaderboard.")
    parser.add_argument("--db", type=Path, default=Path(__file__).with_name("leaderboard.sqlite3"))
    commands = parser.add_subparsers(dest="command", required=True)
    serve = commands.add_parser("serve")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8765)
    for command in ("add-player", "rotate-token"):
        commands.add_parser(command).add_argument("label")
    args = parser.parse_args()
    initialize(args.db)
    if args.command in ("add-player", "rotate-token"):
        token = secrets.token_hex(32)
        digest = hashlib.sha256(token.encode("ascii")).digest()
        with closing(connect(args.db)) as db, db:
            if args.command == "rotate-token":
                if not db.execute("UPDATE players SET token_hash=? WHERE label=?", (digest, args.label)).rowcount:
                    parser.error("Player label was not found.")
            else:
                try:
                    db.execute("INSERT INTO players(label,token_hash) VALUES (?,?)", (args.label, digest))
                except sqlite3.IntegrityError:
                    parser.error("Player label already exists.")
        print(token)
        return
    with ThreadingHTTPServer((args.host, args.port), LeaderboardHandler) as server:
        server.database = args.db
        print(f"Serving on http://{args.host}:{server.server_port}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
