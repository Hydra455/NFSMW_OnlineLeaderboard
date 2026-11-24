ALTER TABLE personal_bests ADD COLUMN speed_milli_kmh INTEGER
    CHECK(speed_milli_kmh IS NULL OR speed_milli_kmh BETWEEN 0 AND 1000000000);
