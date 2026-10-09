PRAGMA foreign_keys=OFF;
BEGIN TRANSACTION;
CREATE TABLE stations (
    callsign TEXT PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    member_id TEXT NOT NULL DEFAULT '',
    street_address TEXT NOT NULL DEFAULT '',
    city TEXT NOT NULL DEFAULT '',
    county TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT '',
    zip TEXT NOT NULL DEFAULT '',
    grid_square TEXT NOT NULL DEFAULT '',
    license_class TEXT NOT NULL DEFAULT '',
    email TEXT NOT NULL DEFAULT '',
    data_source INTEGER NOT NULL DEFAULT 0,
    last_updated INTEGER NOT NULL DEFAULT 0
);
INSERT INTO stations VALUES('KX0ZZA','O''Brien, Sam "Skip"','','','Ringgold','Catoosa','GA','30736','','','',0,1790086400);
INSERT INTO stations VALUES('AB4ZZE','','','','','','','','','','',0,1790086490);
INSERT INTO stations VALUES('VE3ZZD','Tremblay, Élise','','','Montréal','','QC','','','','',0,1790518445);
INSERT INTO stations VALUES('KX0TST','Tester, Pat Q','','1 Main St','Chattanooga','Hamilton','TN','37415','EM75','','',0,1790172800);
INSERT INTO stations VALUES('KD4ZZB','Peña, José','','','Cleveland','','TN','37311','','','',0,1790172820);
INSERT INTO stations VALUES('NX4ZZC/P','Lee, Chris','','','','','','','','','',0,1789481780);
INSERT INTO stations VALUES('WZZZ123','Family, Fixture','','','Chattanooga','','TN','37415','','','',0,1790259200);
CREATE TABLE nets (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    mode TEXT NOT NULL DEFAULT '',
    default_frequency TEXT NOT NULL DEFAULT '',
    default_location TEXT NOT NULL DEFAULT '',
    default_grid_square TEXT NOT NULL DEFAULT '',
    recurrence_description TEXT NOT NULL DEFAULT '',
    notes TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL DEFAULT 0,
    imported_at INTEGER NOT NULL DEFAULT 0,
    is_ad_hoc INTEGER NOT NULL DEFAULT 0,
    repeater_offset TEXT NOT NULL DEFAULT '',
    pl_tone TEXT NOT NULL DEFAULT '',
    partial_match_canada INTEGER NOT NULL DEFAULT 0,
    service TEXT NOT NULL DEFAULT 'amateur'
);
INSERT INTO nets VALUES(1,'Test County Skywarn','FM','145.390','37415','EM75','Tuesdays at 8pm ET',replace('Weather spotters.\u000aSecond line, with "quotes".','\u000a',char(10)),1787408000,0,0,'-0.6','107.2',0,'amateur');
INSERT INTO nets VALUES(2,'Bare Net','','','','','','',1788272000,0,0,'','',0,'amateur');
INSERT INTO nets VALUES(3,'Cross-Border HF Net','SSB','7.255','','','Sundays 1400Z','',1789136000,1789222400,0,'','',1,'amateur');
INSERT INTO nets VALUES(4,'Fusion Net','Fusion','442.100','30736','','','',1789568000,0,0,'+5','',0,'amateur');
INSERT INTO nets VALUES(5,'Family GMRS Net','FM','462.5625','37415','','','',1789740800,0,0,'','',0,'gmrs');
INSERT INTO nets VALUES(6,'Tailgate Test','FM','146.520','','','','',1790172800,0,1,'','',0,'amateur');
CREATE TABLE net_instances (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    net_id INTEGER NOT NULL REFERENCES nets(id),
    instance_date TEXT NOT NULL,
    net_control_callsign TEXT NOT NULL DEFAULT '',
    alternate_net_control_callsign TEXT NOT NULL DEFAULT '',
    logger_callsign TEXT NOT NULL DEFAULT '',
    created_by TEXT NOT NULL DEFAULT '',
    frequency TEXT NOT NULL DEFAULT '',
    location TEXT NOT NULL DEFAULT '',
    status INTEGER NOT NULL DEFAULT 0,
    closed_at INTEGER NOT NULL DEFAULT 0,
    operator_role INTEGER NOT NULL DEFAULT 0,
    started_at INTEGER NOT NULL DEFAULT 0,
    notes TEXT NOT NULL DEFAULT '',
    pushed_at INTEGER NOT NULL DEFAULT 0
);
INSERT INTO net_instances VALUES(1,1,'2026-09-15','KX0TST','','','KX0TST','','',1,1789484300,0,1789481600,replace('Severe watch until 10pm.\u000aNo damage reports.','\u000a',char(10)),0);
INSERT INTO net_instances VALUES(2,1,'2026-09-22','KX0TST','','KX0ZZA','KX0ZZA','','',1,1790088200,2,1790086400,'',0);
INSERT INTO net_instances VALUES(3,3,'2026-09-27','','KX0TST','','KX0TST','','',0,0,1,1790518400,'',0);
INSERT INTO net_instances VALUES(4,4,'2026-09-26','KX0TST','','','KX0TST','','',1,1790432600,0,1790432000,'',0);
INSERT INTO net_instances VALUES(5,5,'2026-09-24','KX0TST','','','KX0TST','','',1,1790260100,0,1790259200,'',1790260400);
INSERT INTO net_instances VALUES(6,6,'2026-09-23','KX0TST','','','KX0TST','','',1,1790173700,0,1790172800,'',0);
CREATE TABLE check_ins (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    net_instance_id INTEGER NOT NULL REFERENCES net_instances(id),
    callsign TEXT NOT NULL REFERENCES stations(callsign),
    sequence_number INTEGER NOT NULL DEFAULT 0,
    signal_report TEXT NOT NULL DEFAULT '',
    remarks TEXT NOT NULL DEFAULT '',
    comment TEXT NOT NULL DEFAULT '',
    checked_in_at INTEGER NOT NULL DEFAULT 0,
    designated_role INTEGER NOT NULL DEFAULT -1,
    name TEXT NOT NULL DEFAULT ''
);
INSERT INTO check_ins VALUES(1,1,'KX0TST',1,'','','',1789481600,-1,'');
INSERT INTO check_ins VALUES(2,1,'KX0ZZA',2,'59','Spotter 12','Heavy rain',1789481660,-1,'');
INSERT INTO check_ins VALUES(3,1,'KD4ZZB',3,'57','','',1789481720,1,'');
INSERT INTO check_ins VALUES(4,1,'NX4ZZC/P',4,'','Portable','',1789481780,2,'');
INSERT INTO check_ins VALUES(5,2,'KX0ZZA',1,'','','',1790086400,-1,'');
INSERT INTO check_ins VALUES(6,2,'KX0TST',2,'','','',1790086430,-1,'');
INSERT INTO check_ins VALUES(7,2,'AB4ZZE',3,'55','First time','',1790086490,-1,'');
INSERT INTO check_ins VALUES(8,3,'KX0TST',1,'','','',1790518400,-1,'');
INSERT INTO check_ins VALUES(9,3,'VE3ZZD',2,'5x9','','Montréal relay',1790518445,-1,'');
INSERT INTO check_ins VALUES(10,4,'KX0TST',1,'','','',1790432000,-1,'');
INSERT INTO check_ins VALUES(11,5,'WZZZ123',1,'','','',1790259230,-1,'Pat');
INSERT INTO check_ins VALUES(12,5,'WZZZ123',2,'','','',1790259230,-1,'Alex');
INSERT INTO check_ins VALUES(13,6,'KX0TST',1,'','','',1790172800,-1,'');
INSERT INTO check_ins VALUES(14,6,'KD4ZZB',2,'','Simplex','',1790172820,-1,'');
CREATE TABLE net_saved_stations (
    net_id INTEGER NOT NULL REFERENCES nets(id),
    callsign TEXT NOT NULL REFERENCES stations(callsign),
    default_remarks TEXT NOT NULL DEFAULT '',
    name TEXT NOT NULL DEFAULT '' COLLATE NOCASE,
    member_id TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (net_id, callsign, name)
);
INSERT INTO net_saved_stations VALUES(1,'KX0ZZA','Mobile spotter','','SW-2002');
INSERT INTO net_saved_stations VALUES(1,'AB4ZZE','','','');
INSERT INTO net_saved_stations VALUES(3,'VE3ZZD','Relays from VE3','','VE-77');
INSERT INTO net_saved_stations VALUES(4,'KX0ZZA','','','CLUB-7');
INSERT INTO net_saved_stations VALUES(2,'KX0TST','','','10001');
INSERT INTO net_saved_stations VALUES(5,'WZZZ123','Mom','Pat','');
INSERT INTO net_saved_stations VALUES(5,'WZZZ123','Kid','Alex','');
CREATE TABLE import_runs (
    source TEXT PRIMARY KEY,
    status TEXT NOT NULL DEFAULT 'never_run',
    started_at INTEGER NOT NULL DEFAULT 0,
    completed_at INTEGER NOT NULL DEFAULT 0,
    records_imported INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NOT NULL DEFAULT ''
, phase TEXT NOT NULL DEFAULT '', percent INTEGER NOT NULL DEFAULT 0, heartbeat_at INTEGER NOT NULL DEFAULT 0, requested_at INTEGER NOT NULL DEFAULT 0);
INSERT INTO import_runs VALUES('uls','complete',1789996300,1789996400,1,'','',0,0,0);
INSERT INTO import_runs VALUES('ised','complete',1789996300,1789996400,1,'','',0,0,0);
INSERT INTO import_runs VALUES('zip_centroids','complete',1789996300,1789996400,1,'','',0,0,0);
INSERT INTO import_runs VALUES('zip_county_data','complete',1789996300,1789996400,1,'','',0,0,0);
INSERT INTO import_runs VALUES('gmrs','complete',1789996300,1789996400,1,'','',0,0,0);
INSERT INTO import_runs VALUES('ca_postal_centroids','complete',1789996300,1789996400,1,'','',0,0,0);
INSERT INTO import_runs VALUES('data_refresh','complete',1789996300,1789996400,0,'','',100,0,0);
CREATE TABLE uls_stations (
    callsign TEXT PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    street_address TEXT NOT NULL DEFAULT '',
    city TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT '',
    zip TEXT NOT NULL DEFAULT '',
    license_class TEXT NOT NULL DEFAULT '',
    last_updated INTEGER NOT NULL DEFAULT 0
);
INSERT INTO uls_stations VALUES('W4ZZF','LICENSEE, FIXTURE A','2 Test Rd','CHATTANOOGA','TN','37421','G',1789996400);
CREATE TABLE gmrs_stations (
    callsign TEXT PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    street_address TEXT NOT NULL DEFAULT '',
    city TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT '',
    zip TEXT NOT NULL DEFAULT '',
    license_class TEXT NOT NULL DEFAULT '',
    last_updated INTEGER NOT NULL DEFAULT 0
);
INSERT INTO gmrs_stations VALUES('WZZZ123','FAMILY, FIXTURE','','CHATTANOOGA','TN','37415','',1789996400);
CREATE TABLE ised_stations (
    callsign TEXT PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    street_address TEXT NOT NULL DEFAULT '',
    city TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT '',
    zip TEXT NOT NULL DEFAULT '',
    license_class TEXT NOT NULL DEFAULT '',
    last_updated INTEGER NOT NULL DEFAULT 0
);
INSERT INTO ised_stations VALUES('VA3ZZG','Fixture, Ised','','Ottawa','ON','K1A 0B1','',1789996400);
CREATE TABLE zip_centroids (
    zip TEXT PRIMARY KEY,
    lat REAL NOT NULL,
    lon REAL NOT NULL
);
INSERT INTO zip_centroids VALUES('37415',35.1121,-85.2793);
CREATE TABLE zip_counties (
    zip TEXT PRIMARY KEY,
    county TEXT NOT NULL DEFAULT ''
);
INSERT INTO zip_counties VALUES('37415','Hamilton');
CREATE TABLE zip_place_counties (
    zip TEXT NOT NULL,
    place TEXT NOT NULL,
    county TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (zip, place)
);
INSERT INTO zip_place_counties VALUES('30736','RINGGOLD','Catoosa');
CREATE TABLE users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL,
    public_key TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT 0,
    last_login_at INTEGER NOT NULL DEFAULT 0,
    view_only INTEGER NOT NULL DEFAULT 0,
    amateur_callsign TEXT NOT NULL DEFAULT '',
    gmrs_callsign TEXT NOT NULL DEFAULT '',
    transfer_method INTEGER NOT NULL DEFAULT 0,
    added_by_user INTEGER NOT NULL DEFAULT 0,
    disabled INTEGER NOT NULL DEFAULT 0
);
INSERT INTO users VALUES(1,'KX0TST','ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixtureKeyOneFixtureKeyOneFixtureKeyOne0 laptop',1787408000,1790003600,0,'KX0TST','',1,0,0);
INSERT INTO users VALUES(2,'KX0TST','ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixtureKeyTwoFixtureKeyTwoFixtureKeyTwo0 phone',1787408000,0,0,'KX0TST','',2,0,1);
INSERT INTO users VALUES(3,'KX0ZZA','ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixtureKeyThreeFixtureKeyThreeFixtureKe0',1787408000,0,1,'KX0ZZA','',0,0,0);
INSERT INTO users VALUES(4,'kd4zzb','ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixtureKeyFourFixtureKeyFourFixtureKey0',1787408000,0,0,'KD4ZZB','',0,0,0);
INSERT INTO users VALUES(5,'WQXX000','ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixtureKeyFiveFixtureKeyFiveFixtureKey0 gmrs',1787408000,0,0,'','WQXX000',0,0,0);
INSERT INTO users VALUES(6,'KX0TST','ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixtureKeySixFixtureKeySixFixtureKeySix tablet',1787408000,0,0,'KX0TST','',0,1,0);
CREATE TABLE key_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    at INTEGER NOT NULL,
    actor TEXT NOT NULL,
    action TEXT NOT NULL,
    username TEXT NOT NULL DEFAULT '',
    detail TEXT NOT NULL DEFAULT ''
);
INSERT INTO key_log VALUES(1,1790004000,'console','added','KX0TST','laptop ED25519 SHA256:FixtureOne');
INSERT INTO key_log VALUES(2,1790004100,'KX0TST','added','KX0TST','tablet ED25519 SHA256:FixtureSix');
INSERT INTO key_log VALUES(3,1790004200,'console','disabled','KX0TST','phone ED25519 SHA256:FixtureTwo');
CREATE TABLE server_options (
    name TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
INSERT INTO server_options VALUES('self_service_keys','1');
PRAGMA writable_schema=ON;
CREATE TABLE IF NOT EXISTS sqlite_sequence(name,seq);
DELETE FROM sqlite_sequence;
INSERT INTO sqlite_sequence VALUES('nets',6);
INSERT INTO sqlite_sequence VALUES('net_instances',6);
INSERT INTO sqlite_sequence VALUES('check_ins',14);
INSERT INTO sqlite_sequence VALUES('users',6);
INSERT INTO sqlite_sequence VALUES('key_log',3);
CREATE INDEX idx_net_instances_net ON net_instances(net_id);
CREATE INDEX idx_check_ins_net_instance ON check_ins(net_instance_id);
CREATE INDEX idx_check_ins_callsign ON check_ins(callsign);
CREATE INDEX idx_net_saved_stations_callsign ON net_saved_stations(callsign);
CREATE INDEX idx_uls_stations_zip_callsign ON uls_stations(zip, callsign);
CREATE INDEX idx_gmrs_stations_zip_callsign ON gmrs_stations(zip, callsign);
CREATE INDEX idx_ised_stations_zip_callsign ON ised_stations(zip, callsign);
CREATE INDEX idx_zip_centroids_lat_lon ON zip_centroids(lat, lon, zip);
PRAGMA writable_schema=OFF;
COMMIT;
PRAGMA user_version = 19;
