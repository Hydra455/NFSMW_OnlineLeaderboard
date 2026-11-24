CREATE UNIQUE INDEX best_player_run_id ON personal_bests(player_id, id);

DROP TRIGGER update_personal_best;
DROP TABLE runs;
