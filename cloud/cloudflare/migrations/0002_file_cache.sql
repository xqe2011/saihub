CREATE TABLE file_cache (
  version TEXT NOT NULL,
  filename TEXT NOT NULL,
  content TEXT NOT NULL,
  PRIMARY KEY (version, filename)
);
