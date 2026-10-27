PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
INSERT OR IGNORE INTO meta VALUES('version','cpp26-sift-gray960-v1-le');
CREATE TABLE IF NOT EXISTS images(
 id INTEGER PRIMARY KEY, scope TEXT NOT NULL, digest TEXT NOT NULL,
 gray BLOB NOT NULL, points BLOB NOT NULL, descriptors BLOB NOT NULL,
 npoints INTEGER NOT NULL, UNIQUE(scope,digest)
);
CREATE INDEX IF NOT EXISTS idx_images_scope ON images(scope,id);
CREATE TABLE IF NOT EXISTS occurrences(
 id INTEGER PRIMARY KEY, scope TEXT NOT NULL, message_id TEXT NOT NULL,
 part INTEGER NOT NULL CHECK(part>=0), sender_id TEXT NOT NULL,
 image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
 received_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),
 matched_image_id INTEGER REFERENCES images(id) ON DELETE SET NULL,
 method TEXT NOT NULL, inliers INTEGER NOT NULL,
 inlier_ratio REAL NOT NULL, coverage REAL NOT NULL, pixel_agreement REAL NOT NULL,
 UNIQUE(scope,message_id,part)
);
CREATE INDEX IF NOT EXISTS idx_occurrences_image ON occurrences(image_id,id);
