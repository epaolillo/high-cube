-- Passwords set from database management. The engine only stores a hash, so the
-- panel keeps the plaintext it assigned in order to show and copy it later.
-- The management account password stays in the application environment.
CREATE TABLE application_db_user_passwords (
  application_id INTEGER NOT NULL,
  username TEXT NOT NULL,
  password TEXT NOT NULL,
  PRIMARY KEY (application_id, username)
);
