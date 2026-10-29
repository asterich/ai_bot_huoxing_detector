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
CREATE TABLE IF NOT EXISTS forward_occurrences(
 id INTEGER PRIMARY KEY, scope TEXT NOT NULL, message_id TEXT NOT NULL,
 part INTEGER NOT NULL CHECK(part>=0), resource_namespace TEXT NOT NULL,
 resource_id TEXT NOT NULL, card_digest TEXT NOT NULL,
 status TEXT NOT NULL, matched_occurrence INTEGER REFERENCES forward_occurrences(id),
 received_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')),
 UNIQUE(scope,message_id,part)
);
CREATE INDEX IF NOT EXISTS idx_forward_resource
 ON forward_occurrences(scope,resource_namespace,resource_id,id);
CREATE INDEX IF NOT EXISTS idx_forward_card ON forward_occurrences(scope,card_digest,id);
-- A common, versioned message-identity index. Legacy image/forward tables remain authoritative.
CREATE TABLE IF NOT EXISTS message_hashes(
 scope TEXT NOT NULL, kind INTEGER NOT NULL, digest TEXT NOT NULL,
 basis INTEGER NOT NULL, storage_id INTEGER NOT NULL, sender_id TEXT NOT NULL DEFAULT '',
 PRIMARY KEY(scope,kind,digest)
);
