-- HTTP basic auth in front of every hostname of an application.
-- htpasswd lines ("username:crypt-hash"), one per user. Empty disables it.
ALTER TABLE applications ADD COLUMN basic_auth_users TEXT NOT NULL DEFAULT '';
