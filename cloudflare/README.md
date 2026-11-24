# Deploy the Most Wanted leaderboard on Cloudflare Free

You need a Cloudflare account, a `workers.dev` subdomain in that account, and Node.js 22 or newer. Perform the following steps in the extracted `MWLeaderboard/cloudflare` directory. Keep Workers and D1 on their Free plans.

## Automatic setup on Windows

Extract the full ZIP and double-click `setup_cloudflare.bat` in the main `MWLeaderboard` folder. Paste the game folder containing `speed.exe`, or press Enter to configure the INI beside the batch file. If you have not signed in with Wrangler before, follow the Cloudflare authorization prompt in your browser. If Cloudflare asks you to initialize a `workers.dev` subdomain, set it under **Workers & Pages → Your subdomain**, then run the batch again.

The launcher creates or reuses the named database, deploys the Worker, applies migrations, checks the public endpoint, and registers a player token. `Gabriel` is the default administrative label. The launcher updates the INI after deployment passes its endpoint check. On a rerun it reuses a token already configured for the same database. Keep the configured host INI private. The batch creates a separate, shareable INI in `player-release\MWLeaderboard.ini`. Run `build_mod.bat` afterward to compile the server address into the ASI and prepare the player files.

The steps below also work if you prefer to use Wrangler directly.

## Deploy

1. Sign in and create a D1 database.

   ```powershell
   npx.cmd wrangler@4 login
   npx.cmd wrangler@4 d1 create mw-leaderboard
   ```

2. In `wrangler.jsonc`, replace the all-zero `database_id` with the ID shown by the create command. If Wrangler asks whether it should add the binding, decline and edit the existing `DB` binding. The Worker and database can both use the default name in the file. If the Worker name is already used in your account, change the top-level `name`.
3. Deploy the Worker and then apply the migrations.

   ```powershell
   npx.cmd wrangler@4 deploy
   npx.cmd wrangler@4 d1 migrations apply mw-leaderboard --remote
   ```

   Copy the `https://...workers.dev` address printed by Wrangler. Its root opens the browser leaderboard. The board is public to anyone with the URL and shows submitted profile names, events, cars, and times.

## Upgrade an existing leaderboard

Run the updated batch against the existing D1 database. The migration preserves player tokens and the current personal bests, then discards the old per-attempt table. To keep a copy of that history, export the database before upgrading with `npx.cmd wrangler@4 d1 export mw-leaderboard --remote --output=leaderboard-backup.sql` and keep the export private. Deploy future updates against the same database. Earlier Worker versions that write to the removed table require a database restore before rollback.

After the upgrade, a faster timed result or a higher Speedtrap score replaces that player’s record and car for the event variant. An equal time can fill in a missing car, and slower times receive a successful acknowledgement so the ASI can file their local JSON normally.

The `0003_speedtrap_scores.sql` migration adds the score field. Previous timed Speedtrap records remain stored until a scored run replaces them. They are omitted from the speed rankings. Upgrade the Worker before distributing the new ASI, which submits accumulated scores and requests personalized version 2 snapshots. Personalized snapshots use `Cache-Control: no-store`.

## Share the mod with players

Give players the ASI and INI from `player-release` after building. They copy both to `scripts` in a game installation with a 32-bit ASI loader. The ASI supplies the server address. The first queued result creates a private local token in `scripts\MWLeaderboard.player` and registers it with the Worker. The player should keep that file through updates.

After embedding the Worker address using the build command in the main README, the shareable INI has this format:

```ini
[Leaderboard]
Online=1
PlayerToken=
```

`OPEN_REGISTRATION` in `wrangler.jsonc` enables enrollment for anyone with the public endpoint. Setting it to `"0"` and redeploying closes enrollment. Existing tokens still submit. For manual enrollment, `node player-token.mjs "Gabriel"` generates `player.sql` and prints the token. Apply the file with `npx.cmd wrangler@4 d1 execute mw-leaderboard --remote --file=player.sql`, then remove it and share the token privately. Add `--rotate` when generating SQL to replace an existing player's token and preserve their results.

## Check the deployment

- Deploy the updated Worker, then build and install the updated ASI. Finish one event of each supported race type. Speedtrap ranks by accumulated speed in km/h. The other modes rank by elapsed time.
- Open the Worker URL in a browser and verify the result. Then open **Leaderboard** in the game and use **Switch View** to toggle Online and Local.
- Have a second player install the same ASI and the shareable INI, then complete that same event variant with a different in-game profile. Both players should be able to see the same standings.
- A new Cloudflare board starts empty. Redeploying against the existing database keeps current personal bests. A configured mod can upload JSON still waiting in its queue. Existing local archives and SQLite history stay available through their original local views.
- Online standings show up to 100 public entries per event variant plus the requesting player’s personal row, with a 5,000-row snapshot limit. Results are client-reported.

References

- [Cloudflare D1 setup](https://developers.cloudflare.com/d1/get-started/)
- [Workers Free limits](https://developers.cloudflare.com/workers/platform/limits/)
- [D1 Free limits](https://developers.cloudflare.com/d1/platform/pricing/)
