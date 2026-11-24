import { configureServer } from "./build-config.mjs";
import { spawn } from "node:child_process";
import { createHash, randomBytes } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, renameSync, unlinkSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { stdin, stdout } from "node:process";
import { createInterface } from "node:readline/promises";
import { setTimeout as delay } from "node:timers/promises";
import { fileURLToPath } from "node:url";

const directory = dirname(fileURLToPath(import.meta.url));
const configPath = join(directory, "wrangler.jsonc");
const windows = process.platform === "win32";
const zeroId = "00000000-0000-0000-0000-000000000000";
const uuid = /^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/i;
const resourceName = /^[a-z0-9][a-z0-9-]{0,61}[a-z0-9]$/;
const playerLabel = /^[a-zA-Z0-9][a-zA-Z0-9 _-]{0,39}$/;
const iniSection = /^\s*\[Leaderboard\]\s*$/i;

function run(args, { capture = false, interactive = false } = {}) {
    const npxCli = windows ? [
        dirname(process.execPath),
        ...(process.env.PATH ?? "").split(";").filter(Boolean).map(path => path.replace(/^"|"$/g, "")),
    ].map(path => join(path, "node_modules", "npm", "bin", "npx-cli.js")).find(existsSync) : undefined;
    if (windows && !npxCli)
        throw new Error("npm was not found. Install Node.js with npm from https://nodejs.org/en/download.");
    const command = windows ? process.execPath : "npx";
    const parameters = [...(windows ? [npxCli] : []), "--yes", "wrangler@4", ...args];
    return new Promise((resolveResult, reject) => {
        const child = spawn(command, parameters, {
            cwd: directory,
            stdio: [interactive ? "inherit" : "ignore",
                capture ? "pipe" : "inherit", capture ? "pipe" : "inherit"],
            env: { ...process.env, WRANGLER_SEND_METRICS: "false", NO_COLOR: "1" },
        });
        let output = "", errors = "";
        child.stdout?.setEncoding("utf8").on("data", chunk => { output += chunk; });
        child.stderr?.setEncoding("utf8").on("data", chunk => { errors += chunk; });
        child.once("error", reject);
        child.once("close", code => resolveResult({ code, output, errors }));
    });
}

async function command(args, options) {
    const result = await run(args, options);
    if (result.code !== 0) {
        if (result.errors)
            process.stderr.write(result.errors);
        throw new Error(`Wrangler ${args.slice(0, 2).join(" ")} failed (exit ${result.code}).`);
    }
    return result.output;
}

function readJson(path) {
    return JSON.parse(readFileSync(path, "utf8").replace(/^\uFEFF/, ""));
}

function writeAtomic(path, contents) {
    const temp = path + "." + randomBytes(8).toString("hex") + ".tmp";
    try {
        writeFileSync(temp, contents, { flag: "wx", mode: 0o600 });
        renameSync(temp, path);
    } finally {
        if (existsSync(temp))
            unlinkSync(temp);
    }
}

async function authenticate(prompt) {
    const identity = await run(["whoami"], { capture: true });
    const output = identity.output + identity.errors;
    if (identity.code === 0 && /(?:logged in|authenticated)/i.test(output)
        && !/\bnot\b.*(?:logged in|authenticated)|haven.t logged in|login required/i.test(output)) {
        stdout.write(identity.output);
        return;
    }
    console.log("Cloudflare will open your browser. Sign in and click Allow, then return here.");
    // Release readline so Wrangler can use the terminal for first-time sign-in.
    prompt.pause();
    await command(["login"], { interactive: true });
    prompt.resume();
    await command(["whoami"]);
}

async function bindDatabase(config, binding) {
    const list = async () => JSON.parse(await command(["d1", "list", "--json"], { capture: true }));
    let databases = await list();
    if (binding.database_id === zeroId) {
        let db = databases.find(item => item.name === binding.database_name);
        if (!db) {
            await command(["d1", "create", binding.database_name, "--update-config=false"]);
            for (let attempt = 0; attempt < 4 && !db; ++attempt) {
                if (attempt)
                    await delay(attempt * 1000);
                databases = await list();
                db = databases.find(item => item.name === binding.database_name);
            }
        }
        const id = db?.uuid ?? db?.id;
        if (typeof id !== "string" || !uuid.test(id))
            throw new Error("Cloudflare did not return a database ID. Run this batch again to reuse the database.");
        binding.database_id = id;
        writeAtomic(configPath, JSON.stringify(config, null, 2) + "\n");
    } else if (!databases.some(item => (item.uuid ?? item.id)?.toLowerCase() === binding.database_id.toLowerCase())) {
        throw new Error("The database in wrangler.jsonc is unavailable in this Cloudflare account. Sign in to the account that owns it.");
    }
}

async function deploy(workerName, prompt) {
    const result = await run(["deploy"], { capture: true });
    stdout.write(result.output);
    if (result.errors)
        process.stderr.write(result.errors);
    if (result.code !== 0) {
        throw new Error("Deployment failed. For a first-time workers.dev setup, open https://dash.cloudflare.com/ and set Your subdomain under Workers & Pages, then run this batch again.");
    }
    const looksRight = url => new RegExp(`^https://${workerName}\\.[a-z0-9-]+\\.workers\\.dev$`).test(url);
    const fromOutput = result.output.match(/https:\/\/[a-z0-9.-]+\.workers\.dev/gi)?.find(looksRight);
    if (fromOutput)
        return fromOutput;
    const input = (await prompt.question("Paste the exact workers.dev URL shown in Workers & Pages: "))
        .trim().replace(/\/$/, "").toLowerCase();
    if (!looksRight(input))
        throw new Error("That URL does not match this Worker. Copy its full HTTPS workers.dev URL and run again.");
    return input;
}

async function checkSite(url) {
    for (let attempt = 0; attempt < 5; ++attempt) {
        if (attempt)
            await delay(2000);
        try {
            const response = await fetch(url + "/api/snapshot", {
                headers: { "User-Agent": "MWLeaderboard-Setup/1.0", Accept: "application/x-ndjson" },
                redirect: "error",
                signal: AbortSignal.timeout(6000),
            });
            if (response.ok && (await response.text()).match(/^MWLeaderboard\/[12]\n/))
                return;
        } catch {
            // A newly deployed workers.dev endpoint can take a few seconds to respond.
        }
    }
    throw new Error(`The leaderboard did not respond at ${url}/api/snapshot. Check the Cloudflare deployment and rerun this batch.`);
}

function readIni(path) {
    if (!existsSync(path))
        return { lines: [], eol: "\r\n", encoding: "latin1" };
    const bytes = readFileSync(path);
    const encoding = bytes[0] === 0xFF && bytes[1] === 0xFE ? "utf16le" : "latin1";
    if (bytes[0] === 0xFE && bytes[1] === 0xFF)
        throw new Error("Save MWLeaderboard.ini as UTF-8 or UTF-16 LE in Notepad before retrying.");
    const content = bytes.toString(encoding);
    const bom = content.match(/^(\uFEFF|\xEF\xBB\xBF)/)?.[0] ?? "";
    const eol = content.includes("\r\n") || !content.includes("\n") ? "\r\n" : "\n";
    return { lines: content.slice(bom.length).split(/\r?\n/), eol, encoding, bom };
}

function sectionBounds(lines) {
    const start = lines.findIndex(line => iniSection.test(line));
    const next = lines.findIndex((line, index) => index > start && /^\s*\[.+\]/.test(line));
    return [start, next === -1 ? lines.length : next];
}

function readToken(ini) {
    const [start, end] = sectionBounds(ini.lines);
    if (start < 0)
        return "";
    const values = ini.lines.slice(start + 1, end);
    const match = values.map(line => /^\s*PlayerToken\s*=\s*([a-f0-9]{64})\s*$/i.exec(line))
        .find(Boolean);
    return match?.[1].toLowerCase() ?? "";
}

function configureIni(path, ini, url, token, autoRegister = false) {
    const settings = [
        `SubmitUrl=${url}/api/runs`,
        `StandingsUrl=${url}/api/snapshot`,
        `PlayerToken=${token}`,
        `AutoRegister=${Number(autoRegister)}`,
    ];
    const [start, end] = sectionBounds(ini.lines);
    if (start < 0) {
        if (ini.lines.at(-1)?.trim())
            ini.lines.push("");
        ini.lines.push("[Leaderboard]", ...settings, "");
    } else {
        const preserved = ini.lines.slice(start + 1, end).filter(
            line => !/^\s*(SubmitUrl|StandingsUrl|PlayerToken|AutoRegister)\s*=/i.test(line)
        );
        ini.lines.splice(start + 1, end - start - 1, ...settings, ...preserved);
    }
    if (ini.lines.at(-1) !== "")
        ini.lines.push("");
    writeAtomic(path, Buffer.from((ini.bom ?? "") + ini.lines.join(ini.eol), ini.encoding));
}

function digest(token) {
    return createHash("sha256").update(token, "ascii").digest("hex");
}

async function playerToken(dbId, ini, prompt, iniPath) {
    const query = async sql => {
        const output = await command(["d1", "execute", "DB", "--remote", "--json",
            "--command", sql], { capture: true });
        const messages = JSON.parse(output);
        if (!Array.isArray(messages) || !messages.length || messages.some(item => !item.success))
            throw new Error("Cloudflare did not confirm the player database query.");
        return messages.flatMap(item => item.results ?? []);
    };
    const configured = readToken(ini);
    const packaged = readToken(readIni(join(directory, "..", "MWLeaderboard.ini")));
    const privatePath = join(dirname(iniPath), "MWLeaderboard.player");
    const saved = existsSync(privatePath) ? readFileSync(privatePath, "ascii") : "";
    const automatic = /^[a-f0-9]{64}$/.test(saved) ? saved : "";
    for (const existing of new Set([configured, automatic, packaged])) {
        if (!existing)
            continue;
        const player = await query(`SELECT id FROM players WHERE token_hash = '${digest(existing)}'`);
        if (player.length)
            return { token: existing };
    }

    const pendingPath = join(directory, `.setup-player-${dbId}.json`);
    let pending;
    if (existsSync(pendingPath)) {
        pending = readJson(pendingPath);
        if (!playerLabel.test(pending.label) || !/^[a-f0-9]{64}$/.test(pending.token))
            throw new Error("The unfinished player record is invalid.");
    } else {
        const label = (await prompt.question("Player label [Gabriel]: ")).trim() || "Gabriel";
        if (!playerLabel.test(label))
            throw new Error("Use 1-40 letters, digits, spaces, dashes or underscores for the player label.");
        pending = { label, token: randomBytes(32).toString("hex") };
        writeFileSync(pendingPath, JSON.stringify(pending) + "\n", { mode: 0o600, flag: "wx" });
    }

    const hash = digest(pending.token);
    const known = await query(`SELECT token_hash FROM players WHERE label = '${pending.label}'`);
    if (known.length && known[0].token_hash !== hash) {
        unlinkSync(pendingPath);
        throw new Error("That player label already exists. Use its original INI or rerun with a different label. The existing token was kept.");
    }
    if (!known.length) {
        await query(`INSERT INTO players (label, token_hash) VALUES ('${pending.label}', '${hash}')`);
    }
    return { token: pending.token, pendingPath };
}

async function main() {
    if (Number(process.versions.node.split(".")[0]) < 22)
        throw new Error("Install Node.js 22 or newer from https://nodejs.org/en/download.");
    const config = readJson(configPath);
    const binding = config.d1_databases?.find(item => item.binding === "DB");
    if (!binding || !resourceName.test(config.name) || !resourceName.test(binding.database_name)
        || !uuid.test(binding.database_id) || config.workers_dev !== true)
        throw new Error("wrangler.jsonc must have the DB binding and workers_dev enabled.");

    const prompt = createInterface({ input: stdin, output: stdout });
    try {
        console.log("Most Wanted Leaderboard | Cloudflare Free setup");
        console.log("This deploys a public standings page on your Cloudflare account.");
        const input = (await prompt.question("Most Wanted folder, or press Enter to configure the INI beside this batch: "))
            .trim().replace(/^"(.*)"$/, "$1");
        const game = input ? resolve(input) : "";
        if (game && !existsSync(join(game, "speed.exe")))
            throw new Error("Pick the Most Wanted folder that contains speed.exe.");
        const iniPath = game ? join(game, "scripts", "MWLeaderboard.ini") : join(directory, "..", "MWLeaderboard.ini");
        const ini = readIni(iniPath);
        await authenticate(prompt);
        console.log("\nPreparing the leaderboard database...");
        await bindDatabase(config, binding);
        if (!Object.hasOwn(config.vars ?? {}, "OPEN_REGISTRATION")) {
            config.vars = { ...config.vars, OPEN_REGISTRATION: "1" };
            writeAtomic(configPath, JSON.stringify(config, null, 2) + "\n");
        }
        console.log("\nDeploying the Worker...");
        const url = await deploy(config.name, prompt);
        console.log("\nApplying database migrations. The personal-bests migration discards prior non-best runs.");
        await command(["d1", "migrations", "apply", "DB", "--remote"]);
        await checkSite(url);
        configureServer(url);

        const player = await playerToken(binding.database_id, ini, prompt, iniPath);
        mkdirSync(dirname(iniPath), { recursive: true });
        configureIni(iniPath, ini, url, player.token);
        if (player.pendingPath)
            unlinkSync(player.pendingPath);
        const playerDirectory = join(directory, "..", "player-release");
        const playerIni = join(playerDirectory, "MWLeaderboard.ini");
        console.log(`\nLeaderboard is live: ${url}`);
        console.log(`Configured your private INI: ${iniPath}`);
        if (config.vars.OPEN_REGISTRATION === "1") {
            mkdirSync(playerDirectory, { recursive: true });
            writeAtomic(playerIni, Buffer.from("[Leaderboard]\r\nOnline=1\r\nPlayerToken=\r\n", "ascii"));
            console.log(`Created the shareable player INI: ${playerIni}`);
            console.log("Run build_mod.bat to build the ASI with your server address. Share the files in player-release.");
        }
        if (game)
            console.log("Install the current MWLeaderboard.asi in scripts if it is not already there.");
        else
            console.log("Copy the configured INI and the current MWLeaderboard.asi to the game's scripts folder.");
    } finally {
        prompt.close();
    }
}

main().catch(error => {
    console.error("\nSetup stopped: " + error.message);
    process.exitCode = 1;
});
