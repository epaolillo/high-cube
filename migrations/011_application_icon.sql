-- Visual icon for an application. Stores either a catalog logo path
-- ("/app-images/<logo>") or an uploaded, square-cropped data URL.
ALTER TABLE applications ADD COLUMN icon TEXT NOT NULL DEFAULT '';
