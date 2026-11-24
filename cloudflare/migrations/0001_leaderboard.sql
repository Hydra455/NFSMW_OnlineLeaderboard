CREATE TABLE players (
    id INTEGER PRIMARY KEY,
    label TEXT NOT NULL UNIQUE,
    token_hash TEXT NOT NULL UNIQUE
);

CREATE TABLE runs (
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

CREATE TABLE personal_bests (
    event_hash TEXT NOT NULL,
    mode TEXT NOT NULL,
    laps INTEGER NOT NULL,
    reversed INTEGER NOT NULL,
    player_id INTEGER NOT NULL REFERENCES players(id),
    id TEXT NOT NULL,
    time_ms INTEGER NOT NULL,
    profile TEXT NOT NULL,
    event_id TEXT NOT NULL,
    event_name TEXT NOT NULL,
    car TEXT NOT NULL,
    received_at TEXT NOT NULL,
    PRIMARY KEY (event_hash, mode, laps, reversed, player_id)
);

CREATE INDEX best_order
    ON personal_bests(event_hash, mode, laps, reversed, time_ms, player_id);

CREATE TRIGGER update_personal_best AFTER INSERT ON runs
BEGIN
    INSERT INTO personal_bests(event_hash, mode, laps, reversed, player_id, id,
                               time_ms, profile, event_id, event_name, car, received_at)
    VALUES (NEW.event_hash, NEW.mode, NEW.laps, NEW.reversed, NEW.player_id, NEW.id,
            NEW.time_ms, NEW.profile, NEW.event_id, NEW.event_name, NEW.car, NEW.received_at)
    ON CONFLICT(event_hash, mode, laps, reversed, player_id) DO UPDATE SET
        id = excluded.id, time_ms = excluded.time_ms,
        profile = excluded.profile, event_id = excluded.event_id,
        event_name = excluded.event_name, car = excluded.car, received_at = excluded.received_at
    WHERE excluded.time_ms < personal_bests.time_ms
        OR (excluded.time_ms = personal_bests.time_ms
            AND personal_bests.car = '' AND excluded.car <> '');
END;
