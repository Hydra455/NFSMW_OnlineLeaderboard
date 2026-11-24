import { createHash, randomBytes } from "node:crypto";
import { writeFileSync } from "node:fs";

const [, , label, action] = process.argv;
if (!label || !/^[a-zA-Z0-9][a-zA-Z0-9 _-]{0,39}$/.test(label)
    || (action !== undefined && action !== "--rotate")) {
    console.error('Usage: node player-token.mjs "Player label" [--rotate]');
    process.exit(1);
}
const token = randomBytes(32).toString("hex");
const digest = createHash("sha256").update(token, "ascii").digest("hex");
const sql = action === "--rotate"
    ? `UPDATE players SET token_hash = '${digest}' WHERE label = '${label}';\n`
    : `INSERT INTO players (label, token_hash) VALUES ('${label}', '${digest}');\n`;
writeFileSync("player.sql", sql, { flag: "wx" });
console.log("PlayerToken=" + token);
console.log("Apply player.sql to D1 before using the token. Keep the token private.");
