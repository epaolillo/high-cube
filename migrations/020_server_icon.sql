-- Visual icon for a server. Stores a catalog logo path or an uploaded, square-cropped data URL.
ALTER TABLE servers ADD COLUMN icon TEXT NOT NULL DEFAULT '';
