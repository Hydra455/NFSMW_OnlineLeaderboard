import { existsSync, mkdirSync, readFileSync, writeFileSync, renameSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const directory = dirname(fileURLToPath(import.meta.url));
const project = resolve(directory, "..");
const header = join(project, "src", "ServerConfig.h");

export function configureServer(address) {
    const url = new URL(address);
    if (url.protocol !== "https:" || url.username || url.password || url.search || url.hash
        || url.pathname !== "/" || url.port || !/^[a-z0-9.-]+$/i.test(url.hostname))
        throw new Error("Use the HTTPS root address of your leaderboard.");
    const contents = `#pragma once

namespace server_config {
inline constexpr wchar_t BaseUrl[] = L"${url.origin}";
}
`;
    const temporary = header + ".tmp";
    writeFileSync(temporary, contents, "utf8");
    renameSync(temporary, header);
}

function readAddress(path) {
    const bytes = readFileSync(path);
    const encoding = bytes[0] === 0xff && bytes[1] === 0xfe ? "utf16le" : "utf8";
    const lines = bytes.toString(encoding).replace(/^\uFEFF/, "").split(/\r?\n/);
    let section = false;
    for (const line of lines) {
        if (/^\s*\[.*\]/.test(line)) {
            section = /^\s*\[Leaderboard\]\s*$/i.test(line);
            continue;
        }
        const setting = /^\s*(SubmitUrl|StandingsUrl)\s*=\s*(https:\/\/[^\s]+)\s*$/i.exec(line);
        if (section && setting) {
            const url = new URL(setting[2]);
            if (!["/api/runs", "/api/snapshot"].includes(url.pathname))
                throw new Error("The INI contains an unexpected leaderboard endpoint.");
            url.pathname = "/";
            return url.href;
        }
    }
    return "";
}

function main() {
    const supplied = process.argv[2];
    if (supplied?.startsWith("https://")) {
        configureServer(supplied);
        console.log("Embedded server configuration prepared.");
        return;
    }
    const sources = supplied ? [resolve(supplied)]
        : [join(project, "MWLeaderboard.ini"), join(project, "player-release", "MWLeaderboard.ini")];
    for (const source of sources) {
        if (!existsSync(source)) {
            if (supplied)
                throw new Error("The configured INI was not found.");
            continue;
        }
        const address = readAddress(source);
        if (address) {
            configureServer(address);
            console.log("Embedded server configuration prepared. Player credentials stay private.");
            return;
        }
    }
    if (!supplied && /BaseUrl\[\] = L"https:\/\//.test(readFileSync(header, "utf8"))) {
        console.log("Using the previously prepared embedded server address.");
        return;
    }
    throw new Error('Run build_mod.bat "https://YOUR-WORKER.workers.dev" or pass the path to your old configured INI.');
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    try {
        main();
        const release = join(project, "player-release");
        mkdirSync(release, { recursive: true });
        writeFileSync(join(release, "MWLeaderboard.ini"),
            "[Leaderboard]\r\nOnline=1\r\nPlayerToken=\r\n", "ascii");
    } catch (error) {
        console.error("Build configuration failed. " + error.message);
        process.exitCode = 1;
    }
}
