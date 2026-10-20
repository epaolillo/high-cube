-- Every application publishes ports. Private was only a flag that skipped -p.
UPDATE applications SET type = 'web' WHERE type != 'web';
