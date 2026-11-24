const fields = ["event_hash", "mode", "laps", "reversed", "time_ms", "profile", "event_id", "event_name", "car", "speed_milli_kmh"];
const modes = ["Sprint", "Circuit", "Drag", "Lap Knockout", "Tollbooth", "Speedtrap"];
const lapModes = new Set(["Circuit", "Lap Knockout"]);
const maxBytes = 4096;
const maxRows = 5000;
const maxPerEvent = 100;
const encoder = new TextEncoder();

const scoreSql = "CASE WHEN mode = 'Speedtrap' THEN -speed_milli_kmh ELSE time_ms END";
const rankingSql = `WITH ranked AS (
    SELECT event_hash, mode, laps, reversed, time_ms, profile, event_id, event_name, car,
           speed_milli_kmh, received_at, player_id,
           RANK() OVER course AS place,
           MIN(time_ms) OVER variant AS leader_time_ms,
           MAX(speed_milli_kmh) OVER variant AS leader_speed_milli_kmh,
           ROW_NUMBER() OVER (
               PARTITION BY event_hash, mode, laps, reversed ORDER BY ${scoreSql}, player_id
           ) AS row_number
    FROM personal_bests
    WHERE (? = 1 OR mode IN ('Sprint', 'Circuit'))
      AND (mode <> 'Speedtrap' OR (speed_milli_kmh IS NOT NULL AND ? = 1))
    WINDOW course AS (PARTITION BY event_hash, mode, laps, reversed ORDER BY ${scoreSql}),
           variant AS (PARTITION BY event_hash, mode, laps, reversed)
)
SELECT event_hash, mode, laps, reversed, time_ms, profile, event_id, event_name, car,
       speed_milli_kmh, received_at, place, leader_time_ms, leader_speed_milli_kmh,
       player_id = ? AS mine
FROM ranked WHERE row_number <= ? OR player_id = ?
ORDER BY mine DESC, row_number, event_hash, mode, laps, reversed LIMIT ?`;

async function playerId(request, db) {
    const credential = /^Bearer ([a-f0-9]{64})$/.exec(request.headers.get("Authorization") || "");
    if (!credential)
        return 0;
    const digest = await crypto.subtle.digest("SHA-256", encoder.encode(credential[1]));
    const hash = Array.from(new Uint8Array(digest), byte => byte.toString(16).padStart(2, "0")).join("");
    return (await db.prepare("SELECT id FROM players WHERE token_hash = ?").bind(hash).first())?.id ?? 0;
}

function response(body, status, type, extra = {}) {
    return new Response(body, {
        status,
        headers: {
            "Content-Type": type + "; charset=utf-8",
            "Cache-Control": "no-store",
            "X-Content-Type-Options": "nosniff",
            "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'",
            ...extra,
        },
    });
}

function json(value, status = 200) {
    return response(JSON.stringify(value), status, "application/json");
}

async function readBody(request) {
    if (!request.body)
        return null;
    const reader = request.body.getReader();
    const bytes = new Uint8Array(maxBytes);
    let length = 0;
    try {
        for (;;) {
            const { done, value } = await reader.read();
            if (done)
                break;
            if (length + value.length > bytes.length) {
                await reader.cancel();
                return null;
            }
            bytes.set(value, length);
            length += value.length;
        }
    } finally {
        reader.releaseLock();
    }
    return bytes.subarray(0, length);
}

function validate(run) {
    if (!run || typeof run !== "object" || Array.isArray(run))
        return false;
    if (Object.keys(run).some(key => key !== "id" && !fields.includes(key)))
        return false;
    if (!Object.hasOwn(run, "speed_milli_kmh"))
        run.speed_milli_kmh = null;
    if (run.speed_milli_kmh !== null && (run.mode !== "Speedtrap"
        || !Number.isInteger(run.speed_milli_kmh) || run.speed_milli_kmh < 0 || run.speed_milli_kmh > 1000000000))
        return false;
    if (!Object.hasOwn(run, "car"))
        run.car = "";
    if (typeof run.id !== "string" || !/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(run.id)
        || typeof run.event_hash !== "string" || !/^[0-9a-f]{8}$/.test(run.event_hash)
        || run.event_hash === "00000000" || !modes.includes(run.mode)
        || typeof run.reversed !== "boolean" || !Number.isInteger(run.laps)
        || run.laps < 1 || run.laps > 99 || (!lapModes.has(run.mode) && run.laps !== 1)
        || !Number.isInteger(run.time_ms) || run.time_ms < 1000 || run.time_ms > 21600000)
        return false;
    for (const [field, limit] of [["profile", 32], ["event_id", 48], ["event_name", 96], ["car", 64]]) {
        const value = run[field];
        if (typeof value !== "string" || [...value].length > limit || /[\p{Cc}\p{Cf}\p{Cs}]/u.test(value)
            || (["profile", "event_id"].includes(field) && !value.trim()))
            return false;
    }
    run.id = run.id.toLowerCase();
    return true;
}

async function submit(request, db, openRegistration) {
    const credential = /^Bearer ([a-f0-9]{64})$/.exec(request.headers.get("Authorization") || "");
    if (!credential)
        return json({ error: "Player token required." }, 401);
    if (!/^application\/json(?:\s*;|$)/i.test(request.headers.get("Content-Type") || ""))
        return json({ error: "Send a JSON result." }, 400);
    const digest = await crypto.subtle.digest("SHA-256", encoder.encode(credential[1]));
    const hash = Array.from(new Uint8Array(digest), byte => byte.toString(16).padStart(2, "0")).join("");
    const lookup = db.prepare("SELECT id FROM players WHERE token_hash = ?").bind(hash);
    let player = await lookup.first();
    const register = openRegistration && request.headers.get("X-MWLeaderboard-Register") === "1";
    if (!player && !register)
        return json({ error: "Unknown player token." }, 401);
    const bytes = await readBody(request);
    if (!bytes)
        return json({ error: "Invalid body size." }, 413);
    let run;
    try {
        run = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
    } catch {
        return json({ error: "Invalid JSON." }, 422);
    }
    if (!validate(run))
        return json({ error: "Invalid race result." }, 422);

    if (!player) {
        await db.prepare(
            "INSERT INTO players (label, token_hash) VALUES (?, ?) ON CONFLICT(token_hash) DO NOTHING"
        ).bind("guest:" + hash, hash).run();
        player = await lookup.first();
        if (!player)
            return json({ error: "Could not register player." }, 503);
    }

    if (run.mode === "Speedtrap" && run.speed_milli_kmh === null)
        return json({ id: run.id, ignored: "Speedtrap requires an accumulated-speed score." });

    const values = fields.map(field => field === "reversed" ? Number(run.reversed) : run[field]);
    const keys = [player.id, run.id];
    const keepBest = db.prepare(
        `INSERT INTO personal_bests (player_id,id,${fields.join(",")},received_at)
         SELECT ?,?,?,?,?,?,?,?,?,?,?,?,strftime('%Y-%m-%dT%H:%M:%SZ', 'now')
         WHERE NOT EXISTS (SELECT 1 FROM personal_bests WHERE player_id = ? AND id = ?)
         ON CONFLICT(event_hash, mode, laps, reversed, player_id) DO UPDATE SET
             id = excluded.id,
             time_ms = excluded.time_ms,
             speed_milli_kmh = excluded.speed_milli_kmh,
             profile = excluded.profile,
             event_id = excluded.event_id,
             event_name = excluded.event_name,
             car = excluded.car,
             received_at = excluded.received_at
         WHERE (excluded.mode = 'Speedtrap' AND (personal_bests.speed_milli_kmh IS NULL
                  OR excluded.speed_milli_kmh > personal_bests.speed_milli_kmh))
            OR (excluded.mode <> 'Speedtrap' AND excluded.time_ms < personal_bests.time_ms)
            OR ((CASE WHEN excluded.mode = 'Speedtrap'
                      THEN excluded.speed_milli_kmh = personal_bests.speed_milli_kmh
                      ELSE excluded.time_ms = personal_bests.time_ms END)
                AND personal_bests.car = '' AND excluded.car <> '')`
    ).bind(...keys, ...values, ...keys);
    const findSaved = db.prepare(
        `SELECT ${fields.join(",")} FROM personal_bests WHERE player_id = ? AND id = ?`
    ).bind(...keys);
    const [updated, lookupResult] = await db.batch([keepBest, findSaved]);
    const saved = lookupResult.results[0];
    if (saved && !fields.every((field, index) => saved[field] === values[index]))
        return json({ error: "Run ID already used." }, 409);
    return json({ id: run.id }, updated.meta.changes > 0 ? 201 : 200);
}

function escape(text) {
    return String(text).replace(/[&<>"']/g, char => (
        { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[char]
    ));
}

function eventKey(row) {
    return [row.event_hash, row.mode, row.laps, Number(row.reversed)].join(".");
}

function displayTime(ms) {
    const seconds = Math.floor(ms / 1000) % 60;
    return `${Math.floor(ms / 60000)}:${String(seconds).padStart(2, "0")}.${String(ms % 1000).padStart(3, "0")}`;
}

function browserPage(rows, selected, mode) {
    const available = rows;
    rows = mode ? rows.filter(row => row.mode === mode) : rows;
    const courses = new Map();
    for (const row of rows) {
        const title = row.event_name || row.event_id;
        const course = [title, row.mode, lapModes.has(row.mode) ? `${row.laps} ${row.laps === 1 ? "lap" : "laps"}` : "",
            row.reversed ? "Reverse" : "Forward"].filter(Boolean).join(" / ");
        courses.set(eventKey(row), course);
    }
    const options = [...courses].sort((left, right) => left[1].localeCompare(right[1]));
    const event = courses.has(selected) ? selected : options[0]?.[0];
    const selector = options.map(([key, name]) =>
        `<option value="${escape(key)}"${key === event ? " selected" : ""}>${escape(name)}</option>`).join("");
    const selectedRows = rows.filter(row => eventKey(row) === event);
    const lines = selectedRows.map(row => (
        `<tr><td>${row.place}</td><td>${escape(row.profile)}</td><td>${escape(row.car || "—")}</td><td>${row.mode === "Speedtrap" ? (row.speed_milli_kmh / 1000).toFixed(3) + " km/h" : displayTime(row.time_ms)}</td></tr>`
    )).join("");
    const modeOptions = ["", ...modes.filter(name => available.some(row => row.mode === name))]
        .map(name => `<option value="${escape(name)}"${name === mode ? " selected" : ""}>${escape(name || "All race types")}</option>`).join("");
    return `<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Most Wanted Leaderboard</title>
<style>:root{color-scheme:dark;font:16px/1.5 system-ui;background:#161713;color:#eee}body{max-width:900px;margin:3rem auto;padding:0 1rem}h1{color:#c9e75e}select,button{font:inherit;padding:.4rem;max-width:100%}table{border-collapse:collapse;width:100%;margin-top:1rem;text-align:left}td,th{border-bottom:1px solid #555;padding:.55rem}td:last-child{font-variant-numeric:tabular-nums}</style>
<main><h1>Most Wanted Leaderboard</h1>${event
        ? `<form><select name="mode" aria-label="Race type">${modeOptions}</select><select name="event" aria-label="Race event">${selector}</select> <button>View standings</button></form>
<table><thead><tr><th>Rank</th><th>Player</th><th>Car</th><th>${selectedRows.some(row => row.mode === "Speedtrap") ? "Total speed" : "Time"}</th></tr></thead><tbody>${lines}</tbody></table>`
        : "<p>No race results yet.</p>"}</main></html>`;
}

export default {
    async fetch(request, env) {
        const target = new URL(request.url);
        try {
            if (request.method === "POST" && target.pathname === "/api/runs")
                return await submit(request, env.DB, env.OPEN_REGISTRATION === "1");
            if (request.method !== "GET" || !["/", "/api/snapshot"].includes(target.pathname))
                return json({ error: "Unknown route." }, 404);
            const allModes = target.pathname === "/" || request.headers.get("X-MWLeaderboard-All-Modes") === "1";
            const version2 = target.pathname === "/" || request.headers.get("X-MWLeaderboard-Version") === "2";
            const identity = version2 ? await playerId(request, env.DB) : 0;
            const { results } = await env.DB.prepare(rankingSql)
                .bind(Number(allModes), Number(version2), identity, maxPerEvent, identity, maxRows).all();
            const rows = results.map(row => ({ ...row, reversed: Boolean(row.reversed), mine: Boolean(row.mine) }));
            if (target.pathname === "/api/snapshot") {
                const body = [version2 ? "MWLeaderboard/2" : "MWLeaderboard/1", ...rows.map(row => JSON.stringify(row))].join("\n") + "\n";
                return response(body, 200, "application/x-ndjson", { Vary: "Authorization, X-MWLeaderboard-All-Modes, X-MWLeaderboard-Version" });
            }
            return response(browserPage(rows, target.searchParams.get("event"), target.searchParams.get("mode") || ""), 200, "text/html");
        } catch (error) {
            console.error("Leaderboard request failed", error.message);
            return json({ error: "Leaderboard unavailable." }, 503);
        }
    },
};
